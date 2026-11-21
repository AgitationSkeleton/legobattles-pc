// DISCORD SIGN-IN (docs/501). Online play through the official servers and
// publishing need a Discord account; there is no password. The game opens
//   https://lbserver.viosarcade.xyz/auth/start?state=<32 hex>
// in the browser, Discord sends the player back to /auth/callback, and the game
// polls /auth/poll?state= for its session token.
//
// THE TOKEN: "v1.<payload>.<signature>", base64url; the payload JSON is
// {sub, uid, name, iat, exp}; the signature is Ed25519 over "v1.<payload>"
// with the secret AUTH_SIGNING_KEY (a private JWK). Its public half is served
// at /auth/key; the fallback holds it as AUTH_PUBLIC_KEY and verifies tokens
// itself, refreshing the banned list from the main server (/auth/bans).
//
// THE ACCOUNTS OBJECT keeps each Discord id's account: its uid, name, first
// and last address and country, and ban. A first sign-in is posted to the
// accounts channel (ACCOUNTS_CHANNEL) with Ban / Unban buttons, pressed by a
// moderator (MODERATORS) and forwarded by the bot as "lvl:acct:..." presses.
//
// Secrets: DISCORD_CLIENT_SECRET, AUTH_SIGNING_KEY (main only),
// LEVELS_BOT_TOKEN (posting). Vars: DISCORD_CLIENT_ID, ACCOUNTS_CHANNEL,
// MODERATORS, AUTH_BASE (the public https base), AUTH_UPSTREAM (fallback:
// where the main server is), AUTH_PUBLIC_KEY (fallback).

import { DurableObject } from 'cloudflare:workers';
import { hasSlur } from './slur.js';
import { postCopies, editCopies } from './modpost.js';

const DISCORD = 'https://discord.com/api/v10';
const STATE_OK = /^[0-9a-f]{32}$/;
const SUB_OK = /^[0-9]{5,25}$/;
const TOKEN_DAYS = 30;          // the ACCOUNT token (aud "main"): HTTPS to this server only
const PLAY_HOURS = 2;           // the PLAY token (aud "play"): the lobby servers, any transport
const STATE_MS = 10 * 60 * 1000;
const MAX_STATES = 5000;        // sign-ins under way at once
const TICKET_OK = /^[0-9a-f]{32}$/;
// security review: per address, in this isolate (a speed bump; the Accounts
// object keeps its own count of sign-ins under way).
const RATES = { start: [20, 600000], poll: [120, 60000], play: [60, 60000], verify: [600, 60000], signout: [10, 600000] };
const rateMap = new Map();
function overRate(kind, ip) {
  const [count, ms] = RATES[kind];
  const now = Date.now();
  if (rateMap.size > 20000) rateMap.clear();
  const key = kind + ' ' + netKey(ip);
  const r = rateMap.get(key);
  if (!r || now - r.at >= ms) { rateMap.set(key, { at: now, n: 1 }); return false; }
  return ++r.n > count;
}
// An address's network for limits: its IPv4, or its IPv6 /64.
export function netKey(ip) {
  const s = String(ip || '');
  return s.includes(':') ? s.split(':').slice(0, 4).join(':') : s;
}
const CROCKFORD = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';

function json(status, payload) {
  return new Response(JSON.stringify(payload), {
    status,
    headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
  });
}
function page(status, title, text) {
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  return new Response(`<!doctype html><meta charset="utf-8"><title>${esc(title)}</title>` +
    '<body style="font-family:sans-serif;background:#1b1d2a;color:#eee;display:flex;align-items:center;' +
    'justify-content:center;height:90vh"><div style="max-width:30em;text-align:center">' +
    `<h2>${esc(title)}</h2><p>${esc(text)}</p></div></body>`,
  { status, headers: { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' } });
}

// ---- base64url and the key ---------------------------------------------------
const enc = new TextEncoder();
function b64u(bytes) {
  let s = '';
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
function unb64u(s) {
  const t = String(s).replace(/-/g, '+').replace(/_/g, '/');
  const bin = atob(t + '==='.slice((t.length + 3) % 4));
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; ++i) out[i] = bin.charCodeAt(i);
  return out;
}

const keyCache = new Map();   // per isolate: 'priv' / 'pub' -> CryptoKey
async function privateKey(env) {
  if (!env.AUTH_SIGNING_KEY) return null;
  if (!keyCache.has('priv')) {
    const jwk = JSON.parse(env.AUTH_SIGNING_KEY);
    keyCache.set('priv', await crypto.subtle.importKey('jwk', jwk, { name: 'Ed25519' }, false, ['sign']));
  }
  return keyCache.get('priv');
}
// The public JWK: the main server derives it from its private key; the
// fallback is given it (AUTH_PUBLIC_KEY).
export function publicJwk(env) {
  if (env.AUTH_SIGNING_KEY) {
    const { kty, crv, x } = JSON.parse(env.AUTH_SIGNING_KEY);
    return { kty, crv, x };
  }
  return env.AUTH_PUBLIC_KEY ? JSON.parse(env.AUTH_PUBLIC_KEY) : null;
}
async function publicKey(env) {
  if (!keyCache.has('pub')) {
    const jwk = publicJwk(env);
    if (!jwk) return null;
    keyCache.set('pub', await crypto.subtle.importKey('jwk', jwk, { name: 'Ed25519' }, false, ['verify']));
  }
  return keyCache.get('pub');
}

export async function signToken(env, claims) {
  const key = await privateKey(env);
  if (!key) throw new Error('no signing key');
  const body = 'v1.' + b64u(enc.encode(JSON.stringify(claims)));
  const sig = new Uint8Array(await crypto.subtle.sign({ name: 'Ed25519' }, key, enc.encode(body)));
  return body + '.' + b64u(sig);
}

// The token's claims when its signature and expiry are good, else null. Does
// NOT check bans or the audience (verifyToken does).
export async function readToken(env, token) {
  const t = String(token || '');
  const parts = t.split('.');
  if (parts.length !== 3 || parts[0] !== 'v1' || t.length > 2048) return null;
  const key = await publicKey(env);
  if (!key) return null;
  let ok = false;
  try {
    ok = await crypto.subtle.verify({ name: 'Ed25519' }, key, unb64u(parts[2]), enc.encode(parts[0] + '.' + parts[1]));
  } catch { ok = false; }
  if (!ok) return null;
  let c;
  try { c = JSON.parse(new TextDecoder().decode(unb64u(parts[1]))); } catch { return null; }
  if (!c || !SUB_OK.test(String(c.sub || '')) || !(c.exp > Date.now() / 1000)) return null;
  return c;
}

// ---- bans, as each server knows them -----------------------------------------
// The main server asks its Accounts object (cached a minute per isolate); the
// fallback holds the main server's list, refreshed every 5 minutes.
// The banned accounts, and the accounts that signed out (every token made
// before then is void): { set, revoked }. FAILS CLOSED (security review):
// until a list has been had, null, and no token is good.
const banCache = { at: 0, set: new Set(), revoked: new Map(), ok: false };
function takeBans(d) {
  if (!d || !Array.isArray(d.banned)) return false;
  banCache.set = new Set(d.banned.map(String));
  banCache.revoked = new Map(Object.entries(d.revoked && typeof d.revoked === 'object' ? d.revoked : {}));
  banCache.ok = true;
  return true;
}
async function bannedSet(env) {
  const now = Date.now();
  if (env.AUTH_SIGNING_KEY && env.ACCOUNTS) {
    if (now - banCache.at > 60000 || !banCache.ok) {
      try {
        const r = await accounts(env).fetch('https://accounts/internal/bans');
        if (takeBans(await r.json().catch(() => null))) banCache.at = now;
      } catch { /* the last list, if any */ }
    }
  } else if (env.AUTH_UPSTREAM && (now - banCache.at > 300000 || !banCache.ok)) {
    try {
      const r = await fetch(String(env.AUTH_UPSTREAM).replace(/\/$/, '') + '/auth/bans', { cf: { cacheTtl: 0 } });
      takeBans(await r.json());
    } catch { /* keep the last list */ }
    banCache.at = now;
  }
  return banCache.ok ? banCache : null;
}
export function forgetBans() { banCache.at = 0; }

// The signed-in player for a request, or null: the token from the JSON body's
// "auth" or the query's ?auth=; good signature, not expired, of one of the
// audiences `auds` (docs/501: "main" is the account token, which alone can
// publish, delete and change scores; "play" the short-lived lobby token),
// not banned, not signed out since it was made.
export async function verifyToken(env, token, auds = ['main', 'play']) {
  const c = await readToken(env, token);
  if (!c || !auds.includes(c.aud)) return null;
  const b = await bannedSet(env);
  if (!b || b.set.has(String(c.sub))) return null;
  const out = Number(b.revoked.get(String(c.sub)) || 0);
  if (out && (Number(c.iat) || 0) * 1000 <= out) return null;
  return c;
}

// ---- the version gate ---------------------------------------------------------
// "0.4.26" against MIN_CLIENT_VERSION; a suffix ("-beta.2") sorts below the
// plain release. Missing or unreadable: too old.
export function versionAtLeast(v, min) {
  const parse = (s) => {
    const m = /^(\d+)\.(\d+)\.(\d+)(-[\w.]+)?/.exec(String(s || '').trim());
    return m ? [Number(m[1]), Number(m[2]), Number(m[3]), m[4] ? 0 : 1] : null;
  };
  const a = parse(v), b = parse(min);
  if (!b) return true;
  if (!a) return false;
  for (let i = 0; i < 4; ++i) if (a[i] !== b[i]) return a[i] > b[i];
  return true;
}
export const UPDATE_MESSAGE = 'This version of LEGO Battles PC is too old for the online servers. ' +
  'Update the game (Settings > General > Install update).';
export const SIGNIN_MESSAGE = 'Sign in with Discord to play online or publish (Network > Account). Direct IP play needs no account.';

function accounts(env) { return env.ACCOUNTS.get(env.ACCOUNTS.idFromName('accounts')); }

export function isModerator(env, user) {
  return String(env.MODERATORS || '').split(',').map((s) => s.trim()).filter(Boolean).includes(String(user));
}

// ---- the public routes ---------------------------------------------------------
export async function authRoute(request, env, path) {
  const url = new URL(request.url);
  // The fallback (no private key) sends sign-ins to the main server.
  if (!env.AUTH_SIGNING_KEY) {
    if (path === '/auth/key') return json(200, { ok: true, key: publicJwk(env) });
    if (path === '/auth/verify' && request.method === 'POST') return verifyRoute(request, env);
    const up = String(env.AUTH_UPSTREAM || '').replace(/\/$/, '');
    if (up && (path === '/auth/start' || path === '/auth/poll' || path === '/auth/bans'))
      return Response.redirect(up + path + url.search, 302);
    return json(404, { ok: false, error: 'no such endpoint' });
  }
  if (!env.ACCOUNTS) return json(503, { ok: false, error: 'sign-in not configured' });
  const ip = request.headers.get('CF-Connecting-IP') || '';
  if (path === '/auth/key') return json(200, { ok: true, key: publicJwk(env) });
  if (path === '/auth/bans') {   // from this isolate's list (a minute old at most)
    const b = await bannedSet(env);
    if (!b) return json(503, { ok: false, error: 'not available' });
    return json(200, { ok: true, banned: [...b.set], revoked: Object.fromEntries(b.revoked) });
  }
  if (path === '/auth/verify' && request.method === 'POST') {
    if (overRate('verify', ip)) return json(429, { ok: false, error: 'slow down' });
    return verifyRoute(request, env);
  }
  // docs/501: a PLAY token for the lobby servers, for an account token. Only
  // over HTTPS: the account token never crosses the network in the clear.
  if (path === '/auth/play' && request.method === 'POST') {
    if (url.protocol !== 'https:' && env.AUTH_ALLOW_HTTP !== '1')
      return json(403, { ok: false, error: 'HTTPS only' });
    if (overRate('play', ip)) return json(429, { ok: false, error: 'slow down' });
    let b = {};
    try { b = JSON.parse(await request.text()); } catch { /* empty */ }
    const c = await verifyToken(env, b.auth, ['main']);
    if (!c) return json(401, { ok: false, error: SIGNIN_MESSAGE, reason: 'signin', message: SIGNIN_MESSAGE });
    const iat = Math.floor(Date.now() / 1000);
    const exp = iat + PLAY_HOURS * 3600;
    const token = await signToken(env, { sub: c.sub, uid: c.uid, name: c.name, un: c.un, aud: 'play', iat, exp });
    return json(200, { ok: true, token, exp });
  }
  // Signing out voids every token the account had (both kinds).
  if (path === '/auth/signout' && request.method === 'POST') {
    if (overRate('signout', ip)) return json(429, { ok: false, error: 'slow down' });
    let b = {};
    try { b = JSON.parse(await request.text()); } catch { /* empty */ }
    const c = await verifyToken(env, b.auth, ['main']);
    if (!c) return json(200, { ok: true });
    await accounts(env).fetch('https://accounts/internal/revoke', { method: 'POST', body: JSON.stringify({ sub: c.sub }) });
    forgetBans();
    return json(200, { ok: true });
  }
  if (path === '/auth/start') {
    // security review: the browser comes back to the game itself, on this
    // machine (RFC 8252): the callback sends it to http://127.0.0.1:<port>
    // with a one-time ticket, and /auth/poll gives the token only with that
    // ticket. A sign-in link sent to someone else sends THEIR browser to
    // THEIR machine, so whoever made the link never sees the ticket.
    const state = String(url.searchParams.get('state') || '');
    const port = Number.parseInt(url.searchParams.get('port') || '', 10);
    if (!STATE_OK.test(state) || !(port >= 1024 && port <= 65535))
      return page(400, 'Sign-in link not valid', 'Start the sign-in again from the game.');
    if (!env.DISCORD_CLIENT_ID || !env.DISCORD_CLIENT_SECRET)
      return page(503, 'Sign-in is not available', 'The server is not set up for Discord sign-in yet.');
    if (overRate('start', ip)) return page(429, 'Too many sign-ins', 'Wait a few minutes and try again.');
    const r = await accounts(env).fetch('https://accounts/internal/state/new', { method: 'POST', body: JSON.stringify({ state, port }) });
    if (r.status !== 200) return page(503, 'Sign-in is busy', 'Try again in a few minutes.');
    const q = new URLSearchParams({
      client_id: env.DISCORD_CLIENT_ID, response_type: 'code', scope: 'identify', state,
      redirect_uri: redirectUri(env),
    });
    // (DISCORD_OAUTH_BASE: a test's fake Discord, never set live.)
    return Response.redirect(String(env.DISCORD_OAUTH_BASE || 'https://discord.com') + '/oauth2/authorize?' + q.toString(), 302);
  }
  if (path === '/auth/callback') return callback(request, env, url);
  if (path === '/auth/poll') {
    const state = String(url.searchParams.get('state') || '');
    const ticket = String(url.searchParams.get('ticket') || '');
    if (!STATE_OK.test(state) || !TICKET_OK.test(ticket)) return json(400, { ok: false, error: 'bad state' });
    if (overRate('poll', ip)) return json(429, { ok: false, error: 'slow down' });
    const r = await accounts(env).fetch('https://accounts/internal/state/take?state=' + state + '&ticket=' + ticket);
    return new Response(r.body, { status: r.status, headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' } });
  }
  return json(404, { ok: false, error: 'no such endpoint' });
}

function redirectUri(env) {
  return String(env.AUTH_BASE || 'https://lbserver.viosarcade.xyz').replace(/\/$/, '') + '/auth/callback';
}

async function verifyRoute(request, env) {
  let body = {};
  try { body = JSON.parse(await request.text()); } catch { /* empty */ }
  const c = await verifyToken(env, body.token);
  return c ? json(200, { ok: true, sub: String(c.sub), uid: c.uid, name: c.name, aud: c.aud })
           : json(200, { ok: false, error: 'signin' });
}

async function callback(request, env, url) {
  const state = String(url.searchParams.get('state') || '');
  const code = String(url.searchParams.get('code') || '');
  if (!STATE_OK.test(state) || !code) {
    const why = url.searchParams.get('error') === 'access_denied' ? 'You cancelled the sign-in.' : 'The sign-in link was not valid.';
    return page(400, 'Not signed in', why + ' Start it again from the game.');
  }
  const acc = accounts(env);
  const known = await (await acc.fetch('https://accounts/internal/state/check?state=' + state)).json();
  if (!known.ok) return page(400, 'Sign-in expired', 'Start the sign-in again from the game.');
  // The code for an access token, the token for the user.
  const api = env.DISCORD_OAUTH_BASE ? env.DISCORD_OAUTH_BASE + '/api/v10' : DISCORD;
  const tr = await fetch(api + '/oauth2/token', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      client_id: env.DISCORD_CLIENT_ID, client_secret: env.DISCORD_CLIENT_SECRET,
      grant_type: 'authorization_code', code, redirect_uri: redirectUri(env),
    }).toString(),
  });
  const tok = await tr.json().catch(() => ({}));
  if (!tr.ok || !tok.access_token) return page(502, 'Sign-in failed', 'Discord did not accept the sign-in. Try again.');
  const ur = await fetch(api + '/users/@me', { headers: { Authorization: 'Bearer ' + tok.access_token } });
  const me = await ur.json().catch(() => ({}));
  if (!ur.ok || !SUB_OK.test(String(me.id || ''))) return page(502, 'Sign-in failed', 'Discord did not say who you are. Try again.');
  const ip = request.headers.get('CF-Connecting-IP') || '';
  const country = request.headers.get('CF-IPCountry') || (request.cf && request.cf.country) || '';
  const r = await acc.fetch('https://accounts/internal/signin', {
    method: 'POST',
    body: JSON.stringify({ state, sub: String(me.id), username: String(me.username || ''),
                           global_name: String(me.global_name || ''), ip, country }),
  });
  const d = await r.json().catch(() => ({}));
  if (d.banned) return page(403, 'Not signed in', 'This Discord account is banned from LEGO Battles PC online play.');
  if (d.refused) return page(403, 'Not signed in', 'Your Discord display name or username is not allowed on LEGO Battles PC. Change it on Discord, then sign in again.');
  if (!d.ok || !TICKET_OK.test(String(d.ticket || '')) || !(d.port >= 1024 && d.port <= 65535))
    return page(500, 'Sign-in failed', 'Something went wrong on the server. Try again.');
  // Back to the game on this machine, with the one-time ticket.
  return Response.redirect(`http://127.0.0.1:${d.port}/lb-signin?state=${state}&ticket=${d.ticket}`, 302);
}

// ---- the Accounts object --------------------------------------------------------
function cleanName(s) {
  const v = String(s || '').replace(/[^\w \-'.!?]/gu, '').trim().slice(0, 24);
  return v || 'Player';
}
function newUid() {
  const b = crypto.getRandomValues(new Uint8Array(8));
  return 'LB-' + [...b].map((x) => CROCKFORD[x & 31]).join('');
}

export class Accounts extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.env = env;
    this.sql = ctx.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS accounts (
      sub TEXT PRIMARY KEY, uid TEXT UNIQUE, name TEXT, created INTEGER,
      first_ip TEXT, first_country TEXT, last_ip TEXT, last_country TEXT, last_seen INTEGER,
      banned INTEGER DEFAULT 0, ban_reason TEXT DEFAULT '', ban_by TEXT DEFAULT '', ban_at INTEGER DEFAULT 0,
      message TEXT DEFAULT '')`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS states (
      state TEXT PRIMARY KEY, created INTEGER, token TEXT DEFAULT '', name TEXT DEFAULT '', sub TEXT DEFAULT '')`);
    // SG-0149: the Discord @username too, so moderators can find the person.
    try { this.sql.exec("ALTER TABLE accounts ADD COLUMN username TEXT DEFAULT ''"); } catch { /* already there */ }
    try { this.sql.exec("ALTER TABLE accounts ADD COLUMN mirror TEXT DEFAULT ''"); } catch { /* already there */ }
    // security review: the loopback port and the one-time ticket (RFC 8252),
    // sign-outs, and an index for the expiry sweep.
    try { this.sql.exec('ALTER TABLE states ADD COLUMN port INTEGER DEFAULT 0'); } catch { /* already there */ }
    try { this.sql.exec("ALTER TABLE states ADD COLUMN ticket TEXT DEFAULT ''"); } catch { /* already there */ }
    try { this.sql.exec('ALTER TABLE accounts ADD COLUMN revoked INTEGER DEFAULT 0'); } catch { /* already there */ }
    this.sql.exec('CREATE INDEX IF NOT EXISTS states_created ON states(created)');
  }

  one(q, ...a) { return this.sql.exec(q, ...a).toArray()[0]; }

  async fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname;
    const now = Date.now();
    this.sql.exec('DELETE FROM states WHERE created < ?', now - STATE_MS);
    let body = {};
    if (request.method === 'POST') { try { body = JSON.parse(await request.text()); } catch { /* empty */ } }
    switch (path) {
      case '/internal/state/new': {
        const port = Number(body.port) | 0;
        if (!STATE_OK.test(String(body.state || '')) || !(port >= 1024 && port <= 65535)) return json(400, { ok: false });
        if (this.one('SELECT COUNT(*) AS n FROM states').n >= MAX_STATES) return json(503, { ok: false });
        this.sql.exec('INSERT OR IGNORE INTO states (state, created, port) VALUES (?, ?, ?)', body.state, now, port);
        return json(200, { ok: true });
      }
      case '/internal/state/check':
        return json(200, { ok: !!this.one('SELECT state FROM states WHERE state = ?', url.searchParams.get('state') || '') });
      case '/internal/state/take': {
        const s = this.one('SELECT * FROM states WHERE state = ?', url.searchParams.get('state') || '');
        if (!s) return json(404, { ok: false, error: 'expired' });
        // Only with the ticket the browser carried back to the game.
        if (!s.token || !s.ticket || s.ticket !== String(url.searchParams.get('ticket') || ''))
          return json(200, { ok: false, pending: true });
        this.sql.exec('DELETE FROM states WHERE state = ?', s.state);
        return json(200, { ok: true, token: s.token, name: s.name, id: s.sub });
      }
      case '/internal/signin': return this.signin(body, now);
      case '/internal/bans': {
        const revoked = {};
        for (const r of this.sql.exec('SELECT sub, revoked FROM accounts WHERE revoked > 0').toArray()) revoked[r.sub] = r.revoked;
        return json(200, { ok: true, banned: this.sql.exec('SELECT sub FROM accounts WHERE banned = 1').toArray().map((r) => r.sub),
                           revoked });
      }
      case '/internal/revoke':   // a sign-out: every token made until now is void
        if (SUB_OK.test(String(body.sub || ''))) this.sql.exec('UPDATE accounts SET revoked = ? WHERE sub = ?', now, String(body.sub));
        return json(200, { ok: true });
      case '/internal/get': {
        const a = this.one('SELECT sub, uid, name, username, banned FROM accounts WHERE sub = ?', url.searchParams.get('sub') || '');
        return json(a ? 200 : 404, a ? { ok: true, ...a } : { ok: false });
      }
      case '/internal/find': {   // docs/503: the bot's account search
        const q = String(url.searchParams.get('q') || '').trim().slice(0, 64);
        if (!q) return json(200, { ok: true, accounts: [] });
        const like = '%' + q.toLowerCase().replace(/[%_\\]/g, (c) => '\\' + c) + '%';
        const rows = this.sql.exec(
          `SELECT sub, uid, name, username, created, last_seen, first_country, banned FROM accounts
             WHERE sub = ? OR lower(uid) = lower(?) OR lower(username) = lower(?)
                OR lower(name) LIKE ? ESCAPE '\\' OR lower(username) LIKE ? ESCAPE '\\'
             ORDER BY last_seen DESC LIMIT 15`, q, q, q, like, like).toArray();
        return json(200, { ok: true, accounts: rows });
      }
      case '/internal/ban': return this.ban(body, true, now);
      case '/internal/unban': return this.ban(body, false, now);
      case '/internal/list': {
        const rows = this.sql.exec('SELECT sub, uid, name, username, created, first_country, last_country, last_seen, banned, ban_reason ' +
                                   'FROM accounts ORDER BY created DESC LIMIT 500').toArray();
        return json(200, { ok: true, accounts: rows });
      }
      default: return json(404, { ok: false, error: 'no such endpoint' });
    }
  }

  async signin(b, now) {
    const sub = String(b.sub || '');
    if (!SUB_OK.test(sub) || !STATE_OK.test(String(b.state || ''))) return json(400, { ok: false });
    if (!this.one('SELECT state FROM states WHERE state = ?', b.state)) return json(400, { ok: false, error: 'expired' });
    // docs/502: a Discord display name or username with a slur is refused.
    // The note in the accounts channel names the account by id only.
    if (hasSlur(b.global_name) || hasSlur(b.username) || hasSlur(cleanName(b.global_name || b.username))) {
      this.ctx.waitUntil(post(this.env, this.env.ACCOUNTS_CHANNEL,
        { content: `🚫 Refused a sign-in from <@${sub}> \`${sub}\`: the Discord name is not allowed.`,
          allowed_mentions: { parse: [] } }));
      return json(200, { ok: false, refused: 'name' });
    }
    const name = cleanName(b.global_name || b.username);
    const un = String(b.username || '').replace(/[^A-Za-z0-9_.]/g, '').slice(0, 32);
    let a = this.one('SELECT * FROM accounts WHERE sub = ?', sub);
    const fresh = !a;
    if (fresh) {
      let uid = newUid();
      while (this.one('SELECT uid FROM accounts WHERE uid = ?', uid)) uid = newUid();
      this.sql.exec('INSERT INTO accounts (sub, uid, name, username, created, first_ip, first_country, last_ip, last_country, last_seen) ' +
                    'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)', sub, uid, name, un, now, b.ip, b.country, b.ip, b.country, now);
      a = this.one('SELECT * FROM accounts WHERE sub = ?', sub);
    } else {
      this.sql.exec('UPDATE accounts SET name = ?, username = ?, last_ip = ?, last_country = ?, last_seen = ? WHERE sub = ?',
                    name, un, b.ip, b.country, now, sub);
    }
    if (a.banned) return json(200, { ok: false, banned: true });
    const iat = Math.floor(now / 1000);
    const token = await signToken(this.env, { sub, uid: a.uid, name, un, aud: 'main', iat, exp: iat + TOKEN_DAYS * 86400 });
    const ticket = [...crypto.getRandomValues(new Uint8Array(16))].map((x) => x.toString(16).padStart(2, '0')).join('');
    this.sql.exec('UPDATE states SET token = ?, name = ?, sub = ?, ticket = ? WHERE state = ?', token, name, sub, ticket, b.state);
    const port = this.one('SELECT port FROM states WHERE state = ?', b.state).port;
    if (fresh) this.ctx.waitUntil(this.announce(a, b));
    return json(200, { ok: true, name, uid: a.uid, fresh, ticket, port });
  }

  // The accounts channel: a first sign-in, with Ban / Unban.
  async announce(a, b) {
    const ch = this.env.ACCOUNTS_CHANNEL;
    if (!this.env.LEVELS_BOT_TOKEN || !ch) return;
    const r = await post(this.env, ch, accountMessage(a, `New account: first sign-in with Discord.`));
    if (r.id || r.mirror) this.sql.exec('UPDATE accounts SET message = ?, mirror = ? WHERE sub = ?', r.id, r.mirror, a.sub);
  }

  async ban(b, on, now) {
    const sub = String(b.sub || '');
    const a = this.one('SELECT * FROM accounts WHERE sub = ?', sub);
    if (!SUB_OK.test(sub)) return json(400, { ok: false });
    if (!a) {   // a ban before the first sign-in still sticks
      if (!on) return json(404, { ok: false, error: 'no such account' });
      this.sql.exec('INSERT INTO accounts (sub, uid, name, created, banned, ban_reason, ban_by, ban_at) VALUES (?, ?, ?, ?, 1, ?, ?, ?)',
                    sub, newUid(), 'Player', now, String(b.reason || ''), String(b.by || ''), now);
    } else {
      this.sql.exec('UPDATE accounts SET banned = ?, ban_reason = ?, ban_by = ?, ban_at = ? WHERE sub = ?',
                    on ? 1 : 0, on ? String(b.reason || '') : '', String(b.by || ''), now, sub);
    }
    forgetBans();
    const acct = this.one('SELECT * FROM accounts WHERE sub = ?', sub);
    if (on) this.ctx.waitUntil(purge(this.env, sub));
    else if (this.env.LEVELS)
      this.ctx.waitUntil(this.env.LEVELS.get(this.env.LEVELS.idFromName('levels'))
        .fetch('https://levels/internal/unpurge', { method: 'POST', body: JSON.stringify({ sub }) }).catch(() => null));
    const who = b.by ? ` by <@${b.by}>` : '';
    this.ctx.waitUntil((async () => {
      if (acct.message || acct.mirror)
        await editCopies(this.env, this.env.ACCOUNTS_CHANNEL, acct.message, acct.mirror,
                         accountMessage(acct, on ? 'Banned' + (b.by ? ' by a moderator' : '') +
                                                   (b.reason && b.reason !== 'moderator' ? ': ' + b.reason : '') + '.' : 'Unbanned.'));
      await post(this.env, this.env.ACCOUNTS_CHANNEL,
                 { content: `${on ? '⛔ Banned' : '✅ Unbanned'} <@${sub}>` +
                             (acct.username ? ` (@${acct.username})` : '') + ` \`${sub}\` (\`${acct.uid}\`)${who}` +
                             (on ? '. Their maps, units and scores were removed.' : '.'),
                   allowed_mentions: { parse: [] } });
    })());
    return json(200, { ok: true });
  }
}

// A ban removes the account's uploads and scores (docs/501), and their lobbies.
async function purge(env, sub) {
  const body = JSON.stringify({ sub });
  const jobs = [];
  if (env.LEVELS) jobs.push(env.LEVELS.get(env.LEVELS.idFromName('levels')).fetch('https://levels/internal/purge', { method: 'POST', body }));
  if (env.SCORES) jobs.push(env.SCORES.get(env.SCORES.idFromName('scores')).fetch('https://scores/internal/purge', { method: 'POST', body }));
  if (env.DIRECTORY) jobs.push(env.DIRECTORY.get(env.DIRECTORY.idFromName('directory')).fetch('https://directory/internal/kick', { method: 'POST', body }));
  await Promise.allSettled(jobs);
}

// SG-0149: a Discord account as moderators see it: the mention (which shows
// as a raw id to anyone whose client hasn't seen the user), the display name,
// the @username and the numeric id, so a server ban needs no lookup.
export function discordWho(sub, username, name) {
  return `<@${sub}>` + (name ? `\n${String(name).replace(/[*_`~|>]/g, '')}` : '') +
         (username ? ` (@${username})` : '') + `\n\`${sub}\``;
}

function accountMessage(a, note) {
  const flag = a.first_country ? ` (${a.first_country})` : '';
  return {
    embeds: [{
      title: `${a.banned ? '⛔ ' : ''}${a.name}`,
      color: a.banned ? 0xed4245 : 0x5865f2,
      description: note,
      fields: [
        { name: 'Discord', value: discordWho(a.sub, a.username, a.name), inline: true },
        { name: 'Account id', value: `\`${a.uid}\``, inline: true },
        { name: 'Country', value: (a.first_country || '?') + (a.last_country && a.last_country !== a.first_country ? ` (now ${a.last_country})` : ''), inline: true },
        { name: 'IP address', value: `\`${a.first_ip || '?'}\`` + (a.last_ip && a.last_ip !== a.first_ip ? `\nnow \`${a.last_ip}\`` : ''), inline: true },
      ],
      timestamp: new Date(a.created || Date.now()).toISOString(),
      footer: { text: 'LEGO Battles PC accounts' + flag },
    }],
    components: [{ type: 1, components: [
      a.banned ? { type: 2, style: 3, label: 'Unban', custom_id: `lvl:acct:unban:${a.sub}` }
               : { type: 2, style: 4, label: 'Ban', custom_id: `lvl:acct:ban:${a.sub}` },
    ] }],
    allowed_mentions: { parse: [] },
  };
}

// docs/503: to the accounts channel and the mirror; { id, mirror }.
function post(env, channel, msg) {
  return postCopies(env, channel, () => ({ body: JSON.stringify(msg), json: true }));
}

// The bot's "lvl:acct:ban:<sub>" / "lvl:acct:unban:<sub>", forwarded through
// /levels/admin by Levels (it holds the shared key check) with the presser.
// docs/503: a line about an account, for the bot's replies.
export function acctLine(a) {
  return `**${String(a.name || '?').replace(/[*_`~|>]/g, '')}**` + (a.username ? ` (@${a.username})` : '') +
         ` <@${a.sub}> \`${a.sub}\` \`${a.uid}\`` + (a.first_country ? ` ${a.first_country}` : '') +
         (a.banned ? ' ⛔ banned' : '');
}

export async function findAccounts(env, q) {
  if (!env.ACCOUNTS) return [];
  const r = await accounts(env).fetch('https://accounts/internal/find?q=' + encodeURIComponent(String(q || '')));
  const d = await r.json().catch(() => ({}));
  return Array.isArray(d.accounts) ? d.accounts : [];
}

export async function accountButton(env, action, sub, user, reason) {
  if (!isModerator(env, user)) return { ok: false, message: 'Only a moderator can do that.' };
  if (!env.ACCOUNTS) return { ok: false, message: 'Accounts are not set up.' };
  const r = await accounts(env).fetch('https://accounts/internal/' + (action === 'ban' ? 'ban' : 'unban'),
    { method: 'POST', body: JSON.stringify({ sub, by: String(user), reason: reason || 'moderator' }) });
  const d = await r.json().catch(() => ({}));
  return d.ok ? { ok: true, message: action === 'ban' ? 'Banned; their uploads and scores are being removed.' : 'Unbanned.' }
              : { ok: false, message: d.error || 'That did not work.' };
}
