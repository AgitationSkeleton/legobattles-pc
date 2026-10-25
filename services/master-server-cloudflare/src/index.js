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

import { DurableObject } from 'cloudflare:workers';
import { discord, discordImage, plain, lasted, settingsText } from './discord.js';
import { hasSwear } from './swear.js';

const TTL_MS = 90 * 1000;
const CROCKFORD = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
const CODE_LENGTH = 4;
const MAX_NAME = 24;
const MAX_BODY = 8192;
const MAX_IMAGE_BYTES = 400 * 1024;                     // docs/428
const MAX_IMAGE_BODY = Math.ceil(MAX_IMAGE_BYTES * 4 / 3) + 4096;
const NAME_OK = /^[\w \-'.!?]{1,24}$/u;

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

// ---------------------------------------------------------------- Worker ----

export { RelayRoom } from './relay.js';
export { Scores } from './scores.js';

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
  const dir = env.DIRECTORY.get(env.DIRECTORY.idFromName('directory'));
  const token = request.headers.get('X-Token') || url.searchParams.get('token') || '';
  const q = new URLSearchParams({ code, role, token });
  const auth = await dir.fetch('https://directory/internal/relay-auth?' + q.toString());
  if (auth.status !== 200) return auth;
  return env.RELAY.get(env.RELAY.idFromName(code)).fetch(request);
}

export default {
  async fetch(request, env) {
    if (request.method !== 'GET' && request.method !== 'POST')
      return json(405, { ok: false, error: 'GET or POST' });
    const path = new URL(request.url).pathname;
    const m = /^\/relay\/([0-9A-Za-z]{1,8})\/?$/.exec(path);
    if (m && request.method === 'GET') return relay(request, env, m[1].toUpperCase());
    // Only the Worker itself asks the Directory these.
    if (path.startsWith('/internal/')) return json(404, { ok: false, error: 'no such endpoint' });
    // SG-0018: the tables are read straight from the Scores object.
    if (request.method === 'GET' && /^\/scores(\/rulesets)?\/?$/.test(path)) {
      if (!env.SCORES) return json(503, { ok: false, error: 'scores not configured' });
      return env.SCORES.get(env.SCORES.idFromName('scores')).fetch(request);
    }
    const stub = env.DIRECTORY.get(env.DIRECTORY.idFromName('directory'));
    return stub.fetch(request);
  },
};

// ------------------------------------------------------------- Directory ----

export class Directory extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.env = env;
    this.lobbies = new Map();   // code -> lobby
    this.bans = new Map();      // address -> {reason, created}
    this.rate = new Map();      // address -> {window, count}
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

  log(text) {
    discord(this.env, this.ctx, text);
  }

  // The host and everyone the joiners reported, in order.
  roster(l) {
    return [{ slot: 0, name: l.host_name, address: '' }].concat(
      l.members.map((m, i) => ({ slot: i + 1, name: m.name, address: '' })));
  }

  players(l) {
    return Math.min(l.max_players, Math.max(l.players, 1 + l.members.length));
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
                 `after ${lasted(now - (l.started || l.created))}.`);
      } else {
        this.log(`💤 **${plain(l.host_name)}**'s lobby \`${code}\` expired (the host stopped responding).`);
      }
    }
    return changed;
  }

  async alarm() {
    if (this.sweep(Date.now())) await this.save();
    else if (this.lobbies.size) await this.save();   // reschedule
  }

  limited(addr) {
    const now = Date.now();
    const r = this.rate.get(addr);
    if (!r || now - r.window > 60000) {
      this.rate.set(addr, { window: now, count: 1 });
      return false;
    }
    r.count += 1;
    return r.count > 60;   // a client sends a few a minute
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

    if (request.method === 'GET') {
      if (path === '/' || path === '/health')
        return json(200, { ok: true, service: 'legobattles-master', relay: !!this.env.RELAY,
                           lobbies: [...this.lobbies.values()].filter((l) => l.state === 'open').length });
      // docs/424: the relay's check, from the Worker only (it refuses
      // /internal/ from outside). A joiner may join a lobby that is open or
      // starting -- its match may begin while the relay is opening.
      if (path === '/internal/relay-auth') {
        const l = this.lobbies.get(String(url.searchParams.get('code') || '').toUpperCase());
        if (!l) return json(404, { ok: false, error: 'no such lobby' });
        if (url.searchParams.get('role') === 'host') {
          if (l.token !== String(url.searchParams.get('token') || ''))
            return json(401, { ok: false, error: 'unauthorized' });
        } else if (l.state === 'playing') {
          return json(404, { ok: false, error: 'that match has started' });
        }
        return json(200, { ok: true });
      }
      if (path === '/list')
        return json(200, { ok: true, lobbies: [...this.lobbies.values()]
          .filter((l) => l.state === 'open')
          .sort((a, b) => a.created - b.created)
          .map((l) => this.publicView(l, peer)) });
      if (path.startsWith('/lobby/')) {
        const l = this.lobbies.get(path.slice(7).toUpperCase());
        if (!l || l.state !== 'open') return json(404, { ok: false, error: 'no such lobby' });
        return json(200, { ok: true, lobby: this.publicView(l, peer) });
      }
      return json(404, { ok: false, error: 'no such endpoint' });
    }

    if (this.bans.has(peer)) return json(403, { ok: false, error: 'banned' });
    if (this.limited(peer)) return json(429, { ok: false, error: 'slow down' });
    let body;
    try {
      const text = await request.text();
      // docs/428: a match picture is a base64 PNG, far past the usual limit.
      const limit = path === '/match_image' ? MAX_IMAGE_BODY : MAX_BODY;
      if (!text || text.length > limit) throw new Error();
      body = JSON.parse(text);
      if (!body || typeof body !== 'object') throw new Error();
    } catch {
      return json(400, { ok: false, error: 'bad or oversized body' });
    }

    switch (path) {
      case '/announce': return this.announce(body, peer, now);
      case '/heartbeat': return this.heartbeat(body, now);
      case '/remove': return this.remove(body, now);
      case '/join': return this.join(body, now);
      case '/leave': return this.leave(body, now);
      case '/punch': return this.punch(body, peer, now);
      case '/poll': return this.poll(body, now);
      case '/match_image': return this.matchImage(body, now);
      case '/score': return this.score(body, now);
      default: return json(404, { ok: false, error: 'no such endpoint' });
    }
  }

  // SG-0018: the host's report of a match, checked here, kept by Scores. The
  // server cannot see the match, so it checks what it can: the lobby's token,
  // that a match started at least a minute ago, one report per match, and
  // that every name was in the lobby.
  async score(body, now) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l || l.token !== String(body.token || ''))
      return json(404, { ok: false, error: 'unknown lobby' });
    if (!this.env.SCORES) return json(503, { ok: false, error: 'scores not configured' });
    if (l.state !== 'playing' || !l.started) return json(409, { ok: false, error: 'no match is on' });
    if (l.score_posted) return json(409, { ok: false, error: 'already reported for this match' });
    if (toInt(body.cheats, 0) !== 0) return json(422, { ok: false, error: 'a match with cheats does not count' });
    const seconds = Math.round((now - l.started) / 1000);
    // (SCORE_MIN_SECONDS: a test's `wrangler dev --var`, never set live.)
    if (seconds < toInt(this.env.SCORE_MIN_SECONDS, MIN_SCORE_SECONDS)) return json(422, { ok: false, error: 'the match was too short' });
    const roster = new Map(this.roster(l).map((p) => [p.name.toLowerCase(), p.name]));
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
      players.push({ ...p, name, won: !!p.won, hero: hero >= 0 && hero < 4096 ? hero : -1 });
    }
    if (seen.size < 2) return json(422, { ok: false, error: 'a match needs two players from the lobby' });
    l.score_posted = true;
    await this.save();
    if (!players.length) return json(200, { ok: true, counted: 0, skipped });
    const banned = (Array.isArray(body.banned) ? body.banned : []).slice(0, 256)
      .map((b) => toInt(b, -1)).filter((b) => b >= 0 && b < 4096);
    const report = {
      ruleset: rulesetName(body.ruleset),
      banned: [...new Set(banned)].sort((a, b) => a - b),
      map: l.map_name || cleanName(body.map_name, ''),
      seconds, version: l.version, players,
    };
    const r = await this.env.SCORES.get(this.env.SCORES.idFromName('scores'))
      .fetch('https://scores/internal/score', { method: 'POST', body: JSON.stringify(report) });
    if (r.status !== 200) return r;
    return json(200, { ok: true, counted: players.length, skipped });
  }

  // docs/428: the host's picture of its match (rosters, the map's Free Play
  // preview, the bans), drawn by the game from its own ROM. One per match,
  // posted as an image to every webhook. The server only checks that it is a
  // PNG of a sensible size; it never sees game data otherwise.
  async matchImage(body, now) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l || l.token !== String(body.token || ''))
      return json(404, { ok: false, error: 'unknown lobby' });
    if (l.image_posted) return json(409, { ok: false, error: 'already posted for this match' });
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
    await this.save();
    const map = l.map_name || cleanName(body.map_name, '');
    discordImage(this.env, this.ctx,
                 `🗺️ **${plain(l.host_name)}**'s match` + (map ? ` on **${plain(map)}**` : '') + '.',
                 png);
    return json(200, { ok: true });
  }

  async announce(body, peer, now) {
    const port = toInt(body.port, 0);
    if (!(port >= 1 && port <= 65535)) return json(400, { ok: false, error: 'port out of range' });
    const ceiling = toInt(this.env.MAX_PLAYERS, 8);
    const maxp = toInt(body.max_players, 0);
    if (!(maxp >= 1 && maxp <= ceiling))
      return json(400, { ok: false, error: `max_players must be 1..${ceiling}` });
    const players = Math.max(0, Math.min(toInt(body.players, 1), maxp));
    const code = this.newCode();
    const token = [...crypto.getRandomValues(new Uint8Array(16))]
      .map((b) => b.toString(16).padStart(2, '0')).join('');
    const l = {
      code, token,
      name: cleanName(body.name, 'LEGO Battles'),
      host_name: cleanName(body.host_name, 'Player'),
      address: String(body.address || peer).slice(0, 64),
      port, players, max_players: maxp,
      lan_address: lanAddress(body.lan_address), lan_port: lanPort(body.lan_port),
      map_name: cleanName(body.map_name, ''),
      mode: cleanName(body.mode, ''),
      version: String(body.version || '').slice(0, 32),
      options: body.options && typeof body.options === 'object' ? body.options : {},
      created: now, seen: now, state: 'open', started: 0, members: [],
      // SG-0018: the players who keep out of the score tables.
      no_scores: body.no_scores ? [cleanName(body.host_name, 'Player')] : [],
    };
    this.lobbies.set(code, l);
    await this.save();
    const settings = settingsText(l.options);
    this.log(`🎮 **${plain(l.host_name)}** is hosting a LEGO Battles lobby \`${code}\`` +
             ` (up to ${maxp} players${l.version ? ', build ' + plain(l.version) : ''})` +
             (settings ? '\n' + settings : ''));
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
               (l.map_name ? ` on **${plain(l.map_name)}**` : '') + `: ${who}.`);
    }
    if (state === 'open' && l.state !== 'open') {   // back to the lobby
      l.state = 'open';
      l.image_posted = false;   // docs/428: the next match gets its own picture
      l.score_posted = false;   // SG-0018: and its own report
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
               ` ended after ${lasted(now - (l.started || l.created))}.` + result);
    } else {
      this.log(`❌ **${plain(l.host_name)}** disbanded the lobby \`${code}\`` +
               ` after ${lasted(now - l.created)}.`);
    }
    return json(200, { ok: true });
  }

  async join(body, now) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l) return json(404, { ok: false, error: 'unknown lobby' });
    const name = cleanName(body.name, 'Player');
    // SG-0018: a joiner who keeps out of the score tables says so here.
    l.no_scores = (l.no_scores || []).filter((n) => n !== name);
    if (body.no_scores) l.no_scores.push(name);
    if (!l.members.some((m) => m.name === name)) {
      if (l.members.length >= l.max_players - 1) return json(409, { ok: false, error: 'lobby full' });
      l.members.push({ name, joined: now });
      await this.save();
      this.log(`➕ **${plain(name)}** joined **${plain(l.host_name)}**'s lobby \`${l.code}\`` +
               ` (${this.players(l)}/${l.max_players}).`);
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

  async leave(body, now) {
    const l = this.lobbies.get(String(body.code || '').toUpperCase());
    if (!l) return json(200, { ok: true });   // the lobby is gone already
    const name = cleanName(body.name, 'Player');
    const at = l.members.findIndex((m) => m.name === name);
    if (at >= 0) {
      l.members.splice(at, 1);
      await this.save();
      this.log(`➖ **${plain(name)}** left **${plain(l.host_name)}**'s ` +
               (l.state === 'playing' ? 'match' : `lobby \`${l.code}\``) + '.');
    }
    return json(200, { ok: true });
  }

  // The Python GUI's right-click menu, by hand: ADMIN_KEY is a secret.
  async admin(request, path) {
    const key = this.env.ADMIN_KEY;
    if (!key || request.headers.get('Authorization') !== 'Bearer ' + key)
      return json(403, { ok: false, error: 'forbidden' });
    if (request.method === 'GET' && path === '/admin/lobbies')
      return json(200, { ok: true, lobbies: [...this.lobbies.values()].map((l) => ({ ...this.publicView(l), token: undefined })),
                         bans: [...this.bans.entries()] });
    let body = {};
    try { body = await request.json(); } catch { /* empty */ }
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
