// The LEGO Battles master server, as a Cloudflare Worker.
//
// A DIRECTORY -- the same service as services/master-server's Python one,
// with the same endpoints, field names, 90 s TTL and room-code alphabet, so
// the port's client (port/runner/src/lb_net.cpp) talks to either. Hosts
// advertise a lobby; clients ask for the list and get addresses back. Game
// traffic goes player to player -- except through the relay (src/relay.js,
// docs/424), for the players the direct route cannot connect:
//
//   GET /relay/<code>?role=host|join   a WebSocket (the host's carries its
//                                      token as X-Token or ?token=)
//
// What this one adds is the lobby's life after it is advertised, so that it can
// be logged to Discord (src/discord.js):
//
//   POST /heartbeat  {code, token, players, state?, map_name?, players_list?}
//                    state "open" (listed), "starting" (unlisted: the host
//                    pressed Start) or "playing" (the match is on; the first
//                    one is the "match started" message)
//   POST /remove     {code, token, reason?, winners?, losers?}  reason "disband"
//                    (the default: the lobby closed without a match) or "end" (a
//                    match ended; winners / losers: arrays of player names that
//                    the "match ended" line names)
//   POST /join       {code, name}   a player is in this lobby (the joiner says
//   POST /leave      {code, name}   so; the cartridge's own lobby carries it)
//
// And the introduction that lets two routers open for each other (docs/400):
//
//   POST /punch      {code, port, lan_address?, lan_port?}  a joiner's public
//                    UDP port (from STUN); its address is the one this server
//                    sees, never one it names, so a host can only be pointed
//                    back at whoever asked
//   POST /poll       {code, token} -> {punch: [{address, port}]}  the host,
//                    every 2 s while it waits: who to say hello to. Kept in
//                    memory for 30 s; nothing is stored.
//
// A host announces its STUN mapping as address/port, plus lan_address and
// lan_port. Anyone whose public address equals the host's is behind the same
// router and is given the LAN pair instead (in the list, and in /poll for the
// host).
//
// All of it is kept by ONE Durable Object, so a list is always consistent.
//
// And the score tables (SG-0018, src/scores.js):
//
//   POST /score      {code, token, ruleset, cheats, banned, map_name, seconds,
//                    version, players: [{name, won, hero, units_built, ...}]}
//                    the host, once per match, before its /remove. A player who
//                    opted out (no_scores on /announce or /join) or whose name
//                    the game's swear filter would change is left out.
//   GET /scores, GET /scores/rulesets   the tables (src/scores.js)
//
// And taking rows out (docs/443): the game's player id (player_key on /announce
// and /join; only its sha256 is kept) lets a player remove their own rows, and
// the operator removes a name:
//   POST /scores/forget  {player_key}   10 a hour per address
//   POST /admin/scores/remove {name}, GET /admin/scores/names?q=   (ADMIN_KEY)

import { DurableObject } from 'cloudflare:workers';
import { discord, discordImage, plain, lasted, settingsText } from './discord.js';
import { hasSwear } from './swear.js';
import { hasSlur, NAME_REFUSED } from './slur.js';
import { authRoute, verifyToken, versionAtLeast, UPDATE_MESSAGE, SIGNIN_MESSAGE, netKey } from './auth.js';

const TTL_MS = 90 * 1000;
const CROCKFORD = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
const CODE_LENGTH = 4;
const MAX_NAME = 24;
const MAX_BODY = 8192;
const MAX_IMAGE_BYTES = 400 * 1024;                     // docs/428
const MAX_IMAGE_BODY = Math.ceil(MAX_IMAGE_BYTES * 4 / 3) + 4096;
const NAME_OK = /^[\w \-'.!?]{1,24}$/u;

// GitHub issue #2 (a load test on the Python server, whose findings apply
// here too): one address could list lobbies without end and flood the
// Discord channel, and a host could list itself at any address. The game's
// own traffic, for scale: a host announces once per lobby, heartbeats every
// 30 s and polls every 2 s (6 s after two minutes); the Join screen fetches
// the list every 5 s. Every limit sits far above that; past one, a 429 (a 503
// when full), which the game takes as a refusal and retries later.
const MAX_LOBBIES = 200;          // listed at once, every address together
const MAX_LOBBIES_PER_IP = 3;     // listed at once from one address
const MAX_LIST = 100;             // lobbies in one /list reply
// Per address: [requests, ms].
const RATE_GET = [120, 60000];
const RATE_POST = [120, 60000];
const RATE_ANNOUNCE = [6, 600000];
const RATE_MEMBER = [30, 600000];  // /join and /leave, which make Discord lines
const RATE_PUNCH = [20, 60000];
const RATE_RELAY = [30, 60000];    // security review: relay sockets opened per address
const RATE_SCORE = [6, 3600000];   // match reports per host account (or address)
const RATE_ADMIN = [10, 600000];   // wrong admin keys per address
const MAX_IMAGES_PER_LOBBY = 3;
const MAX_OPTIONS = 24;            // a lobby's settings: entries, and each key's/value's length
const MAX_OPTION_TEXT = 64;
const MAX_CHECKED_TEXT = 64;       // characters of one string the slur check looks at

// security review: a lobby's settings, bounded (short keys, short scalars).
function cleanOptions(v) {
  const out = {};
  if (!v || typeof v !== 'object' || Array.isArray(v)) return out;
  for (const [k, x] of Object.entries(v).slice(0, MAX_OPTIONS)) {
    if (typeof k !== 'string' || !k || k.length > MAX_OPTION_TEXT) continue;
    if (x === null || typeof x === 'boolean') out[k] = x;
    else if (typeof x === 'number' && Number.isFinite(x) && Math.abs(x) < 1e9) out[k] = x;
    else if (typeof x === 'string') out[k] = x.slice(0, MAX_OPTION_TEXT);
  }
  return out;
}

// docs/502: every string of a request body that a lobby, a reply or a
// Discord line can show, each cut short (the check is not free).
function shownTexts(body) {
  const out = [];
  const add = (v) => { if (typeof v === 'string' && v) out.push(v.slice(0, MAX_CHECKED_TEXT)); };
  for (const k of ['name', 'host_name', 'map_name', 'mode', 'version']) add(body[k]);
  for (const k of ['players_list', 'winners', 'losers'])
    if (Array.isArray(body[k])) for (const x of body[k].slice(0, 16)) add(x && typeof x === 'object' ? x.name : x);
  if (body.options && typeof body.options === 'object')
    for (const [k, v] of Object.entries(body.options).slice(0, MAX_OPTIONS)) { add(k); add(v); }
  return out;
}

function sameKey(a, b) {
  const x = new TextEncoder().encode(String(a)), y = new TextEncoder().encode(String(b));
  if (x.length !== y.length) return false;
  let d = 0;
  for (let i = 0; i < x.length; ++i) d |= x[i] ^ y[i];
  return d === 0;
}
// The Discord channel: token buckets, every line together and per source
// address (the lobby's host, or the joiner): [burst, ms per token].
const DISCORD_GLOBAL = [30, 2000];
const DISCORD_PER_SOURCE = [10, 30000];

const V4 = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;
function isV4(s) { return V4.test(String(s == null ? '' : s)); }
function isPublicV4(s) {
  if (!isV4(s)) return false;
  const [a, b] = s.split('.').map(Number);
  if (a === 0 || a === 10 || a === 127 || a >= 224) return false;
  if ((a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 169 && b === 254)) return false;
  return !(a === 100 && b >= 64 && b <= 127);
}
// issue #2, finding 3: the address a lobby is listed at. A host names its
// public address (UPnP or STUN), normally the one this server sees; it is
// believed only then, or when this server cannot check it (the host reached
// it over IPv6). Anything else is listed at the address seen, so a listing
// can never point joiners at a third party.
function hostAddress(claimed, peer) {
  const c = String(claimed == null ? '' : claimed).trim();
  if (c === peer && isV4(c)) return c;
  if (isPublicV4(c) && String(peer).includes(':')) return c;
  return peer;
}

function json(status, payload) {
  return new Response(JSON.stringify(payload), {
    status,
    headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
  });
}

function cleanName(value, fallback) {
  let v = String(value == null ? '' : value).trim();
  if (!v) return fallback;
  v = v.slice(0, MAX_NAME);
  return NAME_OK.test(v) ? v : fallback;
}

function toInt(v, dflt) {
  const n = Number.parseInt(v, 10);
  return Number.isFinite(n) ? n : dflt;
}

// docs/400: a LAN address is only ever a private IPv4 dotted quad, and goes
// only to players behind the same router; anything else is dropped.
const PRIVATE_V4 = /^(10\.\d{1,3}|172\.(1[6-9]|2\d|3[01])|192\.168)\.\d{1,3}\.\d{1,3}$/;
function lanAddress(v) {
  const s = String(v == null ? '' : v).trim();
  return PRIVATE_V4.test(s) ? s : '';
}
function lanPort(v) {
  const n = toInt(v, 0);
  return n >= 1 && n <= 65535 ? n : 0;
}

// docs/443: a player's id. The game sends a random 128-bit key (32 hex); the
// server keeps only its sha256, and never in a public view or a Discord line.
const KEY_OK = /^[0-9a-fA-F]{32}$/;
async function ownerOf(key) {
  const s = String(key == null ? '' : key);
  if (!KEY_OK.test(s)) return '';
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s.toLowerCase()));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, '0')).join('');
}
const FORGET_PER_HOUR = 10;

// ---------------------------------------------------------------- Worker ----

export { RelayRoom } from './relay.js';
export { Accounts } from './auth.js';
export { Scores } from './scores.js';
export { Levels } from './levels.js';
import { MAX_UPLOAD_BODY } from './levels.js';
import { MAX_UNIT_BODY } from './units.js';

// Community levels (src/levels.js): every /levels path goes to the Levels
// object. An upload's size is checked before its body is read, and the
// caller's address reaches the object only to be hashed there for bans.
// /levels/admin is the Discord bot's, behind its own key. Community units
// (src/units.js, /units paths) live in the same object.
async function levels(request, env, path) {
  if (!env.LEVELS) return json(503, { ok: false, error: 'levels not configured' });
  if (request.method === 'POST') {
    const len = Number.parseInt(request.headers.get('Content-Length') || '', 10);
    const cap = path.startsWith('/units/') ? MAX_UNIT_BODY : MAX_UPLOAD_BODY;
    if (!Number.isFinite(len) || len > cap) return json(413, { ok: false, error: 'too large' });
  }
  const headers = new Headers();
  headers.set('Content-Type', 'application/json');
  const k = request.headers.get('X-Levels-Key');
  if (k) headers.set('X-Levels-Key', k);
  headers.set('X-Levels-Addr', request.headers.get('CF-Connecting-IP') || '');
  const init = { method: request.method, headers };
  if (request.method === 'POST') init.body = await request.text();
  const url = new URL(request.url);
  return env.LEVELS.get(env.LEVELS.idFromName('levels')).fetch('https://levels' + path + url.search, init);
}

// SG-0018: a checked match report needs at least this long a match, and two
// players who are counted.
const MIN_SCORE_SECONDS = 60;
const MAX_SCORE_PLAYERS = 4;

// A balance pack's name as the game shows it ("Vanil+ 1.2.1", "FactionWars
// 2.0.1", "Custom"); anything else is Vanilla.
function rulesetName(v) {
  const s = String(v == null ? '' : v).trim().slice(0, 48);
  return /^[\w +.\-'()]{1,48}$/u.test(s) ? s : 'Vanilla';
}

// docs/501: THE GATE. With REQUIRE_SIGNIN = "1" (the cutover), the lobby
// routes and the relay need a signed-in player on a current version: the
// token ("auth" in a POST body, ?auth= on a GET) and the version ("version",
// ?v=) against MIN_CLIENT_VERSION. Returns the player's claims, or a Response
// to send instead. Off, it lets everything through (claims null).
export async function gate(env, auth, version) {
  if (String(env.REQUIRE_SIGNIN || '') !== '1') return { claims: null };
  if (!versionAtLeast(version, env.MIN_CLIENT_VERSION))
    return { refuse: json(426, { ok: false, error: UPDATE_MESSAGE, reason: 'update', message: UPDATE_MESSAGE }) };
  const claims = await verifyToken(env, auth);
  if (!claims) return { refuse: json(401, { ok: false, error: SIGNIN_MESSAGE, reason: 'signin', message: SIGNIN_MESSAGE }) };
  return { claims };
}

// docs/424: the relay's front door. A host proves it owns the lobby with its
// token; a joiner only needs the lobby to be open. Then the WebSocket goes to
// the lobby's own room (src/relay.js).
async function relay(request, env, code) {
  if ((request.headers.get('Upgrade') || '').toLowerCase() !== 'websocket')
    return json(426, { ok: false, error: 'expected a WebSocket upgrade' });
  if (!env.RELAY) return json(503, { ok: false, error: 'relay not configured' });
  const url = new URL(request.url);
  const role = url.searchParams.get('role');
  if (role !== 'host' && role !== 'join')
    return json(400, { ok: false, error: 'role must be host or join' });
  const g = await gate(env, url.searchParams.get('auth'), url.searchParams.get('v'));
  if (g.refuse) return g.refuse;
  const dir = env.DIRECTORY.get(env.DIRECTORY.idFromName('directory'));
  const token = request.headers.get('X-Token') || url.searchParams.get('token') || '';
  const q = new URLSearchParams({ code, role, token, sub: g.claims ? String(g.claims.sub) : '',
                                  peer: request.headers.get('CF-Connecting-IP') || '' });
  const auth = await dir.fetch('https://directory/internal/relay-auth?' + q.toString());
  if (auth.status !== 200) return auth;
  return env.RELAY.get(env.RELAY.idFromName(code)).fetch(request);
}

export default {
  async fetch(request, env) {
    // issue #2, finding 4: HEAD is a GET answered with its headers only.
    if (request.method === 'HEAD') {
      const r = await route(new Request(request.url, { method: 'GET', headers: request.headers }), env);
      return new Response(null, { status: r.status, headers: r.headers });
    }
    return route(request, env);
  },
};

async function route(request, env) {
  {
    if (request.method !== 'GET' && request.method !== 'POST')
      return json(405, { ok: false, error: 'GET or POST' });
    const path = new URL(request.url).pathname;
    const m = /^\/relay\/([0-9A-Za-z]{1,8})\/?$/.exec(path);
    if (m && request.method === 'GET') return relay(request, env, m[1].toUpperCase());
    // Only the Worker itself asks the Directory these.
    if (path.startsWith('/internal/')) return json(404, { ok: false, error: 'no such endpoint' });
    // docs/501: Discord sign-in (src/auth.js).
    if (path.startsWith('/auth/')) return authRoute(request, env, path.replace(/\/$/, ''));
    if (/^\/levels\/(list|get|mini|like|upload|delete|admin|hide|edit|mine)\/?$/.test(path)) return levels(request, env, path.replace(/\/$/, ''));
    if (/^\/units\/(list|get|thumb|like|upload|delete|hide|edit)\/?$/.test(path)) return levels(request, env, path.replace(/\/$/, ''));
    // SG-0018: the tables are read straight from the Scores object.
    if (request.method === 'GET' && /^\/scores(\/rulesets)?\/?$/.test(path)) {
      if (!env.SCORES) return json(503, { ok: false, error: 'scores not configured' });
      return env.SCORES.get(env.SCORES.idFromName('scores')).fetch(request);
    }
    const stub = env.DIRECTORY.get(env.DIRECTORY.idFromName('directory'));
    return stub.fetch(request);
  }
}

// ------------------------------------------------------------- Directory ----

export class Directory extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.env = env;
    this.lobbies = new Map();   // code -> lobby
    this.bans = new Map();      // address -> {reason, created}
    this.rate = new Map();      // kind + address -> {window, count}  (issue #2)
    this.buckets = new Map();   // Discord: source -> {tokens, at}  (issue #2)
    this.held = 0;              // Discord lines held back since the last said so
    this.heldSaid = 0;
    this.forgets = new Map();   // address -> {window, count}  (docs/443; memory only)
    this.punches = new Map();   // code -> [{address, port, at}]  (docs/400; memory only)
    ctx.blockConcurrencyWhile(async () => {
      const saved = await ctx.storage.get('lobbies');
      if (saved) for (const l of saved) this.lobbies.set(l.code, l);
      const bans = await ctx.storage.get('bans');
      if (bans) for (const [a, b] of bans) this.bans.set(a, b);
    });
  }

  async save() {
    await this.ctx.storage.put('lobbies', [...this.lobbies.values()]);
    if (this.lobbies.size) {
      const next = Math.min(...[...this.lobbies.values()].map((l) => l.seen)) + TTL_MS + 1000;
      await this.ctx.storage.setAlarm(Math.max(next, Date.now() + 1000));
    } else {
      await this.ctx.storage.deleteAlarm();
    }
  }

  // A lobby announced with `quiet` -- a host on Both lists the same lobby on
  // a second server, which then stays out of the Discord channel the first
  // one already posts to -- is listed and served as usual but never logged.
  log(text, l, source) {
    if (l && l.quiet) return;
    // issue #2: a flood of lobbies or joins cannot flood the channel. A line
    // over a limit is dropped and counted; the next one that goes says how
    // many were held back (at most every 10 minutes).
    const src = source !== undefined ? source : (l && l.peer) || '';
    if ((src && !this.take('s:' + src, DISCORD_PER_SOURCE)) || !this.take('all', DISCORD_GLOBAL)) {
      if (++this.held === 1) console.log(`[discord] lines held back (the latest from ${src || '?'})`);
      return;
    }
    const now = Date.now();
    if (this.held && now - this.heldSaid > 600000) {
      text += `\n⚠️ ${this.held} lobby line${this.held === 1 ? '' : 's'} held back (too many at once).`;
      this.held = 0;
      this.heldSaid = now;
    }
    discord(this.env, this.ctx, text);
  }

  take(key, [burst, every]) {
    const now = Date.now();
    if (this.buckets.size > 4096)
      for (const [k, b] of this.buckets) if (b.tokens + (now - b.at) / every >= burst) this.buckets.delete(k);
    const b = this.buckets.get(key);
    const tokens = b ? Math.min(burst, b.tokens + (now - b.at) / every) : burst;
    if (tokens < 1) { this.buckets.set(key, { tokens, at: now }); return false; }
    this.buckets.set(key, { tokens: tokens - 1, at: now });
    return true;
  }

  // The host and everyone the joiners reported, in order.
  roster(l) {
    return [{ slot: 0, name: l.host_name, address: '' }].concat(
      l.members.map((m, i) => ({ slot: i + 1, name: m.name, address: '' })));
  }

  players(l) {
    return Math.min(l.max_players, Math.max(l.players, 1 + l.members.length));
  }

  // docs/442: the fallback server (FALLBACK_MASTER, "host:port"), named in
  // /health and /list so a client learns where to go when this one cannot be
  // reached. The fallback itself sets none.
  fallbackField() {
    const f = String(this.env.FALLBACK_MASTER || '').trim();
    return /^[A-Za-z0-9.-]{1,253}:[0-9]{1,5}$/.test(f) ? { fallback: f } : {};
  }

  // `viewer` is the asker's address: one behind the host's own router is
  // given the host's LAN address (docs/400).
  publicView(l, viewer) {
    const local = viewer && viewer === l.address && l.lan_address && l.lan_port;
    return {
      code: l.code, name: l.name, host_name: l.host_name,
      address: local ? l.lan_address : l.address,
      port: local ? l.lan_port : l.port, players: this.players(l), max_players: l.max_players,
      map_name: l.map_name, mode: l.mode, version: l.version,
      options: l.options, created: l.created / 1000, seen: l.seen / 1000,
      state: l.state, players_list: this.roster(l),
    };
  }

  // Drop what nobody refreshed. A lobby that goes quiet is reported once.
  sweep(now) {
    let changed = false;
    for (const [code, l] of this.lobbies) {
      if (now - l.seen <= TTL_MS) continue;
      this.lobbies.delete(code);
      changed = true;
      if (l.state === 'playing') {
        this.log(`⏹️ **${plain(l.host_name)}**'s match ended (the host stopped reporting) ` +
                 `after ${lasted(now - (l.started || l.created))}.`, l);
      } else {
        this.log(`💤 **${plain(l.host_name)}**'s lobby \`${code}\` expired (the host stopped responding).`, l);
      }
    }
    return changed;
  }

  async alarm() {
    if (this.sweep(Date.now())) await this.save();
    else if (this.lobbies.size) await this.save();   // reschedule
  }

  // A fixed window per kind and address; the map is pruned as it ages.
  limited(kind, addr, [count, ms]) {
    const now = Date.now();
    if (now - (this.pruned || 0) > 60000) {
      this.pruned = now;
      for (const [k, r] of this.rate) if (now - r.window > 3600000) this.rate.delete(k);
    }
    const key = kind + ' ' + netKey(addr);   // security review: an IPv6 /64 is one address
    const r = this.rate.get(key);
    if (!r || now - r.window >= ms) {
      this.rate.set(key, { window: now, count: 1 });
      return false;
    }
    r.count += 1;
    return r.count > count * this.scale();
  }

  // TEST_LIMIT_SCALE: a test's `wrangler dev --var` (the older suites post
  // more from one address than a player does), never set live.
  scale() { return Math.max(1, toInt(this.env.TEST_LIMIT_SCALE, 1)); }

  slowDown(ms) {
    return new Response(JSON.stringify({ ok: false, error: 'slow down' }), {
      status: 429,
      headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store',
                 'Retry-After': String(Math.ceil(ms / 1000)) },
    });
  }

  newCode() {
    for (let i = 0; i < 64; ++i) {
      const bytes = crypto.getRandomValues(new Uint8Array(CODE_LENGTH));
      const code = [...bytes].map((b) => CROCKFORD[b & 31]).join('');
      if (!this.lobbies.has(code)) return code;
    }
    throw new Error('could not allocate a free room code');
  }

  async fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname.replace(/\/+$/, '') || '/';
    const peer = request.headers.get('CF-Connecting-IP') || '0.0.0.0';
    const now = Date.now();
    if (this.sweep(now)) await this.save();

    if (path.startsWith('/admin/')) return this.admin(request, path);
    if (path === '/internal/kick') {   // docs/501: from Accounts, on a ban
      const b = await request.json().catch(() => ({}));
      let n = 0;
      for (const [code, l] of this.lobbies) if (l.host_sub && l.host_sub === String(b.sub || '')) { this.lobbies.delete(code); ++n; }
      if (n) await this.save();
      return json(200, { ok: true, closed: n });
    }

    if (request.method === 'GET' && path !== '/internal/relay-auth' && this.limited('get', peer, RATE_GET))
      return this.slowDown(RATE_GET[1]);
    if (request.method === 'GET' && (path === '/list' || path.startsWith('/lobby/'))) {
      const g = await gate(this.env, url.searchParams.get('auth'), url.searchParams.get('v'));
      if (g.refuse) return g.refuse;
    }
    if (request.method === 'GET') {
      if (path === '/' || path === '/health')
        return json(200, { ok: true, service: 'legobattles-master', relay: !!this.env.RELAY,
                           lobbies: [...this.lobbies.values()].filter((l) => l.state === 'open').length,
                           ...this.fallbackField() });
      // docs/424: the relay's check, from the Worker only (it refuses
      // /internal/ from outside). A joiner may join a lobby that is open or
      // starting -- its match may begin while the relay is opening.
      if (path === '/internal/relay-auth') {
        // security review: relay sockets opened per address, and with sign-in
        // on, a joiner's socket only for an account that joined the lobby.
        if (this.limited('relay', url.searchParams.get('peer') || '', RATE_RELAY)) return this.slowDown(RATE_RELAY[1]);
        const l = this.lobbies.get(String(url.searchParams.get('code') || '').toUpperCase());
        if (!l) return json(404, { ok: false, error: 'no such lobby' });
        const sub = String(url.searchParams.get('sub') || '');
        if (url.searchParams.get('role') === 'host') {
          if (l.token !== String(url.searchParams.get('token') || ''))
            return json(401, { ok: false, error: 'unauthorized' });
        } else if (l.state === 'playing') {
          return json(404, { ok: false, error: 'that match has started' });
        } else if (sub && l.host_sub && !l.members.some((m) => m.sub === sub)) {
          return json(403, { ok: false, error: 'join the lobby first' });
        }
        return json(200, { ok: true });
      }
      if (path === '/list') {
        // issue #2, finding 2: at most MAX_LIST lobbies in one reply, with the total.
        const open = [...this.lobbies.values()].filter((l) => l.state === 'open')
          .sort((a, b) => a.created - b.created);
        return json(200, { ok: true, lobbies: open.slice(0, MAX_LIST).map((l) => this.publicView(l, peer)),
                           total: open.length, ...this.fallbackField() });
      }
      if (path.startsWith('/lobby/')) {
        const l = this.lobbies.get(path.slice(7).toUpperCase());
        if (!l || l.state !== 'open') return json(404, { ok: false, error: 'no such lobby' });
        return json(200, { ok: true, lobby: this.publicView(l, peer) });
      }
      return json(404, { ok: false, error: 'no such endpoint' });
    }

    // The body is read before any refusal: one left unread can drop the
    // connection (as admin() notes).
    const text = await request.text().catch(() => '');
    if (this.bans.has(peer)) return json(403, { ok: false, error: 'banned' });
    if (this.limited('post', peer, RATE_POST)) return this.slowDown(RATE_POST[1]);
    if (path === '/announce' && this.limited('announce', peer, RATE_ANNOUNCE)) return this.slowDown(RATE_ANNOUNCE[1]);
    if ((path === '/join' || path === '/leave') && this.limited('member', peer, RATE_MEMBER))
      return this.slowDown(RATE_MEMBER[1]);
    if (path === '/punch' && this.limited('punch', peer, RATE_PUNCH)) return this.slowDown(RATE_PUNCH[1]);
    let body;
    try {
      // docs/428: a match picture is a base64 PNG, far past the usual limit.
      const limit = path === '/match_image' ? MAX_IMAGE_BODY : MAX_BODY;
      if (!text || text.length > limit) throw new Error();
      body = JSON.parse(text);
      if (!body || typeof body !== 'object') throw new Error();
    } catch {
      return json(400, { ok: false, error: 'bad or oversized body' });
    }

    // docs/501: every lobby POST needs a signed-in player on a current version.
    const g = await gate(this.env, body.auth, body.version);
    if (g.refuse) return g.refuse;
    const me = g.claims;
    // docs/502: a slur in anything a lobby, a reply or a Discord line shows
    // refuses the request.
    if (['/announce', '/join', '/heartbeat', '/leave', '/remove', '/match_image'].includes(path) &&
        shownTexts(body).some((t) => hasSlur(t)))
      return json(403, { ok: false, error: NAME_REFUSED, reason: 'name' });
    // docs/501: changing scores takes the ACCOUNT token, not a play token.
    if ((path === '/scores/hide' || path === '/scores/forget') && me && me.aud !== 'main')
      return json(401, { ok: false, error: SIGNIN_MESSAGE, reason: 'signin', message: SIGNIN_MESSAGE });
    switch (path) {
      case '/scores/hide': return this.scoresHide(body, me);
      case '/announce': return this.announce(body, peer, now, me);
      case '/heartbeat': return this.heartbeat(body, now);
      case '/remove': return this.remove(body, now);
      case '/join': return this.join(body, peer, now, me);
      case '/leave': return this.leave(body, peer, now, me);
      case '/punch': return this.punch(body, peer, now);
      case '/poll': return this.poll(body, now);
      case '/match_image': return this.matchImage(body, now);
      case '/score': return this.score(body, now, peer);
      case '/scores/forget': return this.forget(body, peer, now, me);
      default: return json(404, { ok: false, error: 'no such endpoint' });
    }
  }

  // SG-0018: the host's report of a match, checked here, kept by Scores. The
  // server cannot see the match, so it checks what it can: the lobby's token,
  // that a match started at least a minute ago, one report per match, and
  // that every name was in the lobby.
  async score(body, now, peer) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l || l.token !== String(body.token || ''))
      return json(404, { ok: false, error: 'unknown lobby' });
    if (this.limited('score', l.host_sub || peer, RATE_SCORE))
      return json(429, { ok: false, error: 'too many match reports; try later' });
    if (!this.env.SCORES) return json(503, { ok: false, error: 'scores not configured' });
    if (l.state !== 'playing' || !l.started) return json(409, { ok: false, error: 'no match is on' });
    if (l.score_posted) return json(409, { ok: false, error: 'already reported for this match' });
    if (toInt(body.cheats, 0) !== 0) return json(422, { ok: false, error: 'a match with cheats does not count' });
    const seconds = Math.round((now - l.started) / 1000);
    // (SCORE_MIN_SECONDS: a test's `wrangler dev --var`, never set live.)
    if (seconds < toInt(this.env.SCORE_MIN_SECONDS, MIN_SCORE_SECONDS)) return json(422, { ok: false, error: 'the match was too short' });
    const roster = new Map(this.roster(l).map((p) => [p.name.toLowerCase(), p.name]));
    const owners = new Map([[l.host_name.toLowerCase(), l.host_owner || '']]);   // docs/443
    for (const m of l.members) if (!owners.has(m.name.toLowerCase())) owners.set(m.name.toLowerCase(), m.owner || '');
    const subs = new Map([[l.host_name.toLowerCase(), l.host_sub || '']]);   // docs/501
    for (const m of l.members) if (!subs.has(m.name.toLowerCase())) subs.set(m.name.toLowerCase(), m.sub || '');
    const out = new Set((l.no_scores || []).map((n) => n.toLowerCase()));
    const seen = new Set();
    const players = [];
    let skipped = 0;
    for (const p of (Array.isArray(body.players) ? body.players : []).slice(0, MAX_SCORE_PLAYERS)) {
      if (!p || typeof p !== 'object') continue;
      // The game's lobby records keep 11 characters of a name, already through
      // its swear filter; a name that matches no one in the lobby is left out.
      const said = String(p.name || '').trim().toLowerCase();
      let key = roster.has(said) ? said : '';
      if (!key && said.length >= 10)
        for (const k of roster.keys()) if (k.slice(0, 11) === said) { key = k; break; }
      if (!key) { ++skipped; continue; }
      if (seen.has(key)) return json(422, { ok: false, error: 'a player is named twice' });
      seen.add(key);
      const name = roster.get(key);
      if (out.has(key) || hasSwear(name, this.env.SWEAR_TEST_WORDS)) { ++skipped; continue; }
      const hero = toInt(p.hero, -1);
      players.push({ ...p, name, won: !!p.won, hero: hero >= 0 && hero < 4096 ? hero : -1,
                     owner: owners.get(key) || '', sub: subs.get(key) || '' });   // (a client cannot name its own owner)
    }
    if (seen.size < 2) return json(422, { ok: false, error: 'a match needs two players from the lobby' });
    // security review: with sign-in on, two different signed-in accounts.
    if (String(this.env.REQUIRE_SIGNIN || '') === '1' &&
        new Set([...seen].map((k) => subs.get(k)).filter(Boolean)).size < 2)
      return json(422, { ok: false, error: 'a match needs two signed-in players' });
    l.score_posted = true;
    await this.save();
    if (!players.length) return json(200, { ok: true, counted: 0, skipped });
    const banned = (Array.isArray(body.banned) ? body.banned : []).slice(0, 256)
      .map((b) => toInt(b, -1)).filter((b) => b >= 0 && b < 4096);
    const report = {
      ruleset: rulesetName(body.ruleset),
      // docs/443: the preset family the game sends; without one Scores derives it.
      preset: body.preset ? rulesetName(body.preset) : '',
      banned: [...new Set(banned)].sort((a, b) => a - b),
      map: l.map_name || cleanName(body.map_name, ''),
      seconds, version: l.version, players,
    };
    const r = await this.env.SCORES.get(this.env.SCORES.idFromName('scores'))
      .fetch('https://scores/internal/score', { method: 'POST', body: JSON.stringify(report) });
    if (r.status !== 200) return r;
    return json(200, { ok: true, counted: players.length, skipped });
  }

  // docs/443: a player removes their own rows. Ten tries an hour per address,
  // kept in memory; a missing or short key is a 400.
  // docs/501: a signed-in player's own rows go, all of them.
  async forget(body, peer, now, me) {
    if (me) {
      if (!this.env.SCORES) return json(503, { ok: false, error: 'scores not configured' });
      return this.scoresCall('/internal/forget', { sub: String(me.sub) });
    }
    const owner = await ownerOf(body.player_key);
    if (!owner) return json(400, { ok: false, error: 'player_key must be 32 hex characters' });
    const r = this.forgets.get(peer);
    if (!r || now - r.window > 3600000) this.forgets.set(peer, { window: now, count: 1 });
    else if (++r.count > FORGET_PER_HOUR) return json(429, { ok: false, error: 'too many tries; try later' });
    if (!this.env.SCORES) return json(503, { ok: false, error: 'scores not configured' });
    return this.scoresCall('/internal/forget', { owner });
  }

  // docs/501: hide or show a signed-in player's rows on the tables.
  async scoresHide(body, me) {
    if (!me) return json(401, { ok: false, error: SIGNIN_MESSAGE, reason: 'signin', message: SIGNIN_MESSAGE });
    if (!this.env.SCORES) return json(503, { ok: false, error: 'scores not configured' });
    return this.scoresCall('/internal/hide', { sub: String(me.sub), hidden: body.hidden !== false });
  }

  scoresCall(path, body) {
    return this.env.SCORES.get(this.env.SCORES.idFromName('scores'))
      .fetch('https://scores' + path, body === undefined ? {} : { method: 'POST', body: JSON.stringify(body) });
  }

  // docs/428: the host's picture of its match (rosters, the map's Free Play
  // preview, the bans), drawn by the game from its own ROM. One per match,
  // posted as an image to every webhook. The server only checks that it is a
  // PNG of a sensible size; it never sees game data otherwise.
  async matchImage(body, now) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l || l.token !== String(body.token || ''))
      return json(404, { ok: false, error: 'unknown lobby' });
    if (l.image_posted || (l.images || 0) >= MAX_IMAGES_PER_LOBBY)
      return json(409, { ok: false, error: 'already posted for this match' });
    if (l.state !== 'playing') return json(409, { ok: false, error: 'no match is on' });
    let png;
    try {
      const bin = atob(String(body.png || ''));
      png = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; ++i) png[i] = bin.charCodeAt(i);
    } catch {
      return json(400, { ok: false, error: 'png is not base64' });
    }
    const sig = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A];
    if (png.length < 64 || png.length > MAX_IMAGE_BYTES || sig.some((b, i) => png[i] !== b))
      return json(400, { ok: false, error: 'not a PNG of an accepted size' });
    l.image_posted = true;
    l.images = (l.images || 0) + 1;
    await this.save();
    if (l.quiet) return json(200, { ok: true, quiet: true });   // the other server posts it
    if (!this.take('img:' + netKey(l.peer), [3, 600000]) || !this.take('all', DISCORD_GLOBAL))
      return json(200, { ok: true, held: true });
    const map = l.map_name || cleanName(body.map_name, '');
    discordImage(this.env, this.ctx,
                 `🗺️ **${plain(l.host_name)}**'s match` + (map ? ` on **${plain(map)}**` : '') + '.',
                 png);
    return json(200, { ok: true });
  }

  async announce(body, peer, now, me) {
    const port = toInt(body.port, 0);
    if (!(port >= 1 && port <= 65535)) return json(400, { ok: false, error: 'port out of range' });
    const ceiling = toInt(this.env.MAX_PLAYERS, 8);
    const maxp = toInt(body.max_players, 0);
    if (!(maxp >= 1 && maxp <= ceiling))
      return json(400, { ok: false, error: `max_players must be 1..${ceiling}` });
    const players = Math.max(0, Math.min(toInt(body.players, 1), maxp));
    // issue #2, finding 2: lobbies from one address, and in all.
    let mine = 0;
    for (const o of this.lobbies.values()) if (netKey(o.peer) === netKey(peer)) ++mine;
    if (mine >= MAX_LOBBIES_PER_IP * this.scale())
      return json(429, { ok: false, error: 'too many lobbies from this address' });
    if (me) {   // docs/501: and two at once per account, from any number of addresses
      let acct = 0;
      for (const o of this.lobbies.values()) if (o.host_sub === String(me.sub)) ++acct;
      if (acct >= 2 * this.scale()) return json(429, { ok: false, error: 'you already have a lobby listed' });
    }
    if (this.lobbies.size >= MAX_LOBBIES) return json(503, { ok: false, error: 'the server is full' });
    let code;
    try { code = this.newCode(); } catch { return json(503, { ok: false, error: 'no room code is free' }); }
    const token = [...crypto.getRandomValues(new Uint8Array(16))]
      .map((b) => b.toString(16).padStart(2, '0')).join('');
    const l = {
      code, token,
      name: cleanName(body.name, 'LEGO Battles'),
      host_name: cleanName(body.host_name, 'Player'),
      address: hostAddress(body.address, peer),
      peer,   // issue #2: the address it was announced from (never in a view)
      host_sub: me ? String(me.sub) : '',   // docs/501: the host's Discord account (never in a view)
      port, players, max_players: maxp,
      lan_address: lanAddress(body.lan_address), lan_port: lanPort(body.lan_port),
      map_name: cleanName(body.map_name, ''),
      mode: cleanName(body.mode, ''),
      version: String(body.version || '').slice(0, 32),
      options: cleanOptions(body.options),
      created: now, seen: now, state: 'open', started: 0, members: [],
      // SG-0018: the players who keep out of the score tables.
      no_scores: body.no_scores ? [cleanName(body.host_name, 'Player')] : [],
      host_owner: await ownerOf(body.player_key),   // docs/443; never in a view
      quiet: body.quiet === true || body.quiet === 1,   // the second server of a host on Both
    };
    this.lobbies.set(code, l);
    await this.save();
    const settings = settingsText(l.options);
    this.log(`🎮 **${plain(l.host_name)}** is hosting a LEGO Battles lobby \`${code}\`` +
             ` (up to ${maxp} players${l.version ? ', build ' + plain(l.version) : ''})` +
             (settings ? '\n' + settings : ''), l);
    return json(200, { ok: true, code, token, ttl: TTL_MS / 1000 });
  }

  async heartbeat(body, now) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l || l.token !== String(body.token || ''))
      return json(404, { ok: false, error: 'unknown lobby' });
    l.seen = now;
    l.players = Math.max(0, toInt(body.players, l.players));
    if (body.map_name) l.map_name = cleanName(body.map_name, l.map_name);
    const state = String(body.state || '');
    if (state === 'starting' && l.state === 'open') l.state = 'starting';
    if (state === 'playing' && l.state !== 'playing') {
      l.state = 'playing';
      l.started = now;
      const who = this.roster(l).map((p) => '**' + plain(p.name) + '**').join(', ');
      this.log(`▶️ **${plain(l.host_name)}**'s lobby \`${l.code}\` started a match` +
               (l.map_name ? ` on **${plain(l.map_name)}**` : '') + `: ${who}.`, l);
    }
    if (state === 'open' && l.state !== 'open') {   // back to the lobby
      // The next match gets its own picture and report -- after one that ran
      // (security review: open/playing flips must not re-arm them).
      if (l.state === 'playing' && l.started &&
          now - l.started >= toInt(this.env.SCORE_MIN_SECONDS, MIN_SCORE_SECONDS) * 1000) {
        l.image_posted = false;   // docs/428
        l.score_posted = false;   // SG-0018
      }
      l.state = 'open';
    }
    await this.save();
    return json(200, { ok: true, ttl: TTL_MS / 1000 });
  }

  async remove(body, now) {
    const code = String(body.code || '').toUpperCase();
    const l = this.lobbies.get(code);
    if (!l || l.token !== String(body.token || ''))
      return json(404, { ok: false, error: 'unknown lobby' });
    this.lobbies.delete(code);
    await this.save();
    const reason = String(body.reason || '');
    // SG-0019: the host says who won and who lost (names only, a few, cleaned).
    const names = (v) => (Array.isArray(v) ? v : []).slice(0, 8)
      .map((n) => cleanName(n, '')).filter((n) => n);
    const winners = names(body.winners);
    const losers = names(body.losers);
    const bold = (list) => list.map((n) => '**' + plain(n) + '**').join(', ');
    let result = '';
    if (winners.length) result += ` ${bold(winners)} won` + (losers.length ? `, ${bold(losers)} lost.` : '.');
    else if (losers.length) result += ` ${bold(losers)} lost.`;
    if (l.state === 'playing' || reason === 'end') {
      this.log(`⏹️ **${plain(l.host_name)}**'s match` +
               (l.map_name ? ` on **${plain(l.map_name)}**` : '') +
               ` ended after ${lasted(now - (l.started || l.created))}.` + result, l);
    } else {
      this.log(`❌ **${plain(l.host_name)}** disbanded the lobby \`${code}\`` +
               ` after ${lasted(now - l.created)}.`, l);
    }
    return json(200, { ok: true });
  }

  async join(body, peer, now, me) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l) return json(404, { ok: false, error: 'unknown lobby' });
    const name = cleanName(body.name, 'Player');
    // SG-0018: a joiner who keeps out of the score tables says so here.
    l.no_scores = (l.no_scores || []).filter((n) => n !== name);
    if (body.no_scores) l.no_scores.push(name);
    const owner = await ownerOf(body.player_key);
    const had = l.members.find((m) => m.name === name);
    const sub = me ? String(me.sub) : '';
    // security review: a name another account joined with stays theirs, and
    // an account has one name in a lobby.
    if (had && had.sub && sub && had.sub !== sub) return json(409, { ok: false, error: 'that name is taken in this lobby' });
    if (sub && l.members.some((m) => m.sub === sub && m.name !== name)) {
      l.members = l.members.filter((m) => !(m.sub === sub && m.name !== name));
      await this.save();
    }
    if (had && ((owner && had.owner !== owner) || (sub && had.sub !== sub))) {
      if (owner) had.owner = owner;
      if (sub) had.sub = sub;
      await this.save();
    }
    if (!had) {
      if (l.members.length >= l.max_players - 1) return json(409, { ok: false, error: 'lobby full' });
      l.members.push({ name, joined: now, owner, sub });
      await this.save();
      this.log(`➕ **${plain(name)}** joined **${plain(l.host_name)}**'s lobby \`${l.code}\`` +
               ` (${this.players(l)}/${l.max_players}).`, l, peer);
    }
    return json(200, { ok: true });
  }

  // docs/400: a joiner's public UDP port, for the host's /poll.
  async punch(body, peer, now) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l || l.state !== 'open') return json(404, { ok: false, error: 'unknown lobby' });
    const port = toInt(body.port, 0);
    if (!(port >= 1 && port <= 65535)) return json(400, { ok: false, error: 'port out of range' });
    const local = peer === l.address;
    const entry = local && lanAddress(body.lan_address) && lanPort(body.lan_port)
      ? { address: lanAddress(body.lan_address), port: lanPort(body.lan_port), at: now }
      : { address: peer, port, at: now };
    const list = (this.punches.get(l.code) || [])
      .filter((p) => now - p.at < 30000 && !(p.address === entry.address && p.port === entry.port));
    list.push(entry);
    this.punches.set(l.code, list.slice(-8));
    return json(200, { ok: true });
  }

  // docs/400: the host's question while it waits. Nothing is written.
  async poll(body, now) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l || l.token !== String(body.token || ''))
      return json(404, { ok: false, error: 'unknown lobby' });
    const list = (this.punches.get(l.code) || []).filter((p) => now - p.at < 30000);
    if (list.length) this.punches.set(l.code, list); else this.punches.delete(l.code);
    return json(200, { ok: true, punch: list.map((p) => ({ address: p.address, port: p.port })) });
  }

  async leave(body, peer, now, me) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l) return json(200, { ok: true });   // the lobby is gone already
    const name = cleanName(body.name, 'Player');
    const at = l.members.findIndex((m) => m.name === name);
    // security review: only the account that joined takes its name out.
    if (at >= 0 && l.members[at].sub && (!me || String(me.sub) !== l.members[at].sub))
      return json(403, { ok: false, error: 'not your name' });
    if (at >= 0) {
      l.members.splice(at, 1);
      await this.save();
      this.log(`➖ **${plain(name)}** left **${plain(l.host_name)}**'s ` +
               (l.state === 'playing' ? 'match' : `lobby \`${l.code}\``) + '.', l, peer);
    }
    return json(200, { ok: true });
  }

  // The Python GUI's right-click menu, by hand: ADMIN_KEY is a secret.
  async admin(request, path) {
    const key = this.env.ADMIN_KEY;
    // (Read the body before answering: a refusal that leaves it unread can drop the connection in `wrangler dev`.)
    const text = request.method === 'POST' ? await request.text().catch(() => '') : '';
    const peer = request.headers.get('CF-Connecting-IP') || '';
    if (!key || !sameKey(request.headers.get('Authorization') || '', 'Bearer ' + key)) {
      if (this.limited('admin', peer, RATE_ADMIN)) return this.slowDown(RATE_ADMIN[1]);
      return json(403, { ok: false, error: 'forbidden' });
    }
    if (request.method === 'GET' && path === '/admin/lobbies')
      return json(200, { ok: true, lobbies: [...this.lobbies.values()].map((l) => ({ ...this.publicView(l), token: undefined })),
                         bans: [...this.bans.entries()] });
    // docs/443: look a name up, and remove one from the score tables.
    if (request.method === 'GET' && path === '/admin/scores/names') {
      if (!this.env.SCORES) return json(503, { ok: false, error: 'scores not configured' });
      return this.env.SCORES.get(this.env.SCORES.idFromName('scores'))
        .fetch('https://scores/internal/names?q=' + encodeURIComponent(new URL(request.url).searchParams.get('q') || ''));
    }
    let body = {};
    try { body = JSON.parse(text); } catch { /* empty */ }
    if (request.method === 'POST' && path === '/admin/scores/remove') {
      if (!this.env.SCORES) return json(503, { ok: false, error: 'scores not configured' });
      return this.scoresCall('/internal/remove-name', { name: String(body.name || '') });
    }
    if (path === '/admin/close') {
      const ok = this.lobbies.delete(String(body.code || '').toUpperCase());
      await this.save();
      return json(ok ? 200 : 404, { ok });
    }
    if (path === '/admin/ban' && body.address) {
      this.bans.set(String(body.address), { reason: String(body.reason || ''), created: Date.now() });
      await this.ctx.storage.put('bans', [...this.bans.entries()]);
      return json(200, { ok: true });
    }
    if (path === '/admin/unban' && body.address) {
      const ok = this.bans.delete(String(body.address));
      await this.ctx.storage.put('bans', [...this.bans.entries()]);
      return json(ok ? 200 : 404, { ok });
    }
    return json(404, { ok: false, error: 'no such endpoint' });
  }
}
