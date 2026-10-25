// SG-0018: THE SCORE TABLES. One Durable Object (SQLite) keeps every reported
// match and each player's line in it. The Directory checks a report first (the
// lobby's token, a match that really started, once per match, names on the
// roster, no opted-out players, no swearing names, no cheats) and hands it here
// over /internal/score; nothing reaches this object from outside but the reads:
//
//   GET /scores/rulesets                     -> {rulesets: [{ruleset, matches}]}
//   GET /scores?ruleset=Vanilla&bans=any|on|off&banned=<blueprint>&limit=100
//       -> {rows: [{name, hero, matches, wins, losses, units_built, units_lost,
//                   units_killed, buildings_built, buildings_lost,
//                   buildings_killed, bricks}]}
//
// "bans" picks the matches played with or without unit bans; "banned" only
// those where that unit was banned. A player's hero is the one they brought
// most often in the matches shown. Sorted by wins, then by fewest losses.

import { DurableObject } from 'cloudflare:workers';

const STATS = ['units_built', 'units_lost', 'units_killed', 'buildings_built',
               'buildings_lost', 'buildings_killed', 'bricks'];

function json(status, payload) {
  return new Response(JSON.stringify(payload), {
    status,
    headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
  });
}

function count(v) {
  const n = Number.parseInt(v, 10);
  return Number.isFinite(n) && n >= 0 && n <= 1000000 ? n : -1;
}

export class Scores extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS matches (
        id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER, ruleset TEXT, bans INTEGER,
        banned TEXT, map TEXT, seconds INTEGER, version TEXT)`);
    this.sql.exec(`CREATE TABLE IF NOT EXISTS results (
        match_id INTEGER, name TEXT, name_key TEXT, won INTEGER, hero INTEGER,
        units_built INTEGER, units_lost INTEGER, units_killed INTEGER,
        buildings_built INTEGER, buildings_lost INTEGER, buildings_killed INTEGER,
        bricks INTEGER)`);
    this.sql.exec('CREATE INDEX IF NOT EXISTS results_match ON results(match_id)');
    this.sql.exec('CREATE INDEX IF NOT EXISTS matches_ruleset ON matches(ruleset)');
  }

  async fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname.replace(/\/+$/, '');
    if (request.method === 'POST' && path === '/internal/score') return this.record(await request.json());
    if (request.method === 'GET' && path === '/scores/rulesets') return this.rulesets();
    if (request.method === 'GET' && path === '/scores') return this.table(url.searchParams);
    return json(404, { ok: false, error: 'no such endpoint' });
  }

  // A report the Directory has already checked.
  record(m) {
    const banned = Array.isArray(m.banned) ? m.banned : [];
    const row = this.sql.exec(
      'INSERT INTO matches (at, ruleset, bans, banned, map, seconds, version) VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING id',
      Date.now(), m.ruleset, banned.length ? 1 : 0, ',' + banned.join(',') + ',',
      m.map || '', Math.max(0, m.seconds | 0), m.version || '').one();
    for (const p of m.players) {
      this.sql.exec(
        `INSERT INTO results (match_id, name, name_key, won, hero, ${STATS.join(', ')})
         VALUES (?, ?, ?, ?, ?, ${STATS.map(() => '?').join(', ')})`,
        row.id, p.name, p.name.toLowerCase(), p.won ? 1 : 0, p.hero,
        ...STATS.map((k) => count(p[k])));
    }
    return json(200, { ok: true, match: row.id });
  }

  rulesets() {
    const rows = this.sql.exec(
      'SELECT ruleset, COUNT(*) AS matches FROM matches GROUP BY ruleset ORDER BY matches DESC LIMIT 50').toArray();
    return json(200, { ok: true, rulesets: rows });
  }

  table(q) {
    const where = [];
    const args = [];
    const ruleset = q.get('ruleset');
    if (ruleset) { where.push('m.ruleset = ?'); args.push(ruleset.slice(0, 48)); }
    const bans = q.get('bans');
    if (bans === 'on') where.push('m.bans = 1');
    if (bans === 'off') where.push('m.bans = 0');
    const unit = Number.parseInt(q.get('banned') || '', 10);
    if (Number.isFinite(unit) && unit >= 0 && unit < 4096) {
      where.push('m.banned LIKE ?');
      args.push('%,' + unit + ',%');
    }
    const limit = Math.min(Math.max(Number.parseInt(q.get('limit') || '100', 10) || 100, 1), 200);
    const filter = where.length ? 'WHERE ' + where.join(' AND ') : '';
    // A stat a game could not read is -1: it counts as nothing.
    const sums = STATS.map((k) => `SUM(MAX(r.${k}, 0)) AS ${k}`).join(', ');
    const rows = this.sql.exec(
      `SELECT r.name_key AS name_key, MAX(r.name) AS name, COUNT(*) AS matches,
              SUM(r.won) AS wins, COUNT(*) - SUM(r.won) AS losses, ${sums}
         FROM results r JOIN matches m ON m.id = r.match_id ${filter}
        GROUP BY r.name_key ORDER BY wins DESC, losses ASC, matches DESC LIMIT ?`,
      ...args, limit).toArray();
    // Each player's most-played hero in the same matches.
    const heroes = new Map();
    for (const h of this.sql.exec(
      `SELECT r.name_key AS name_key, r.hero AS hero, COUNT(*) AS n
         FROM results r JOIN matches m ON m.id = r.match_id ${filter}
        GROUP BY r.name_key, r.hero ORDER BY n DESC`, ...args)) {
      if (!heroes.has(h.name_key)) heroes.set(h.name_key, h.hero);
    }
    return json(200, {
      ok: true,
      rows: rows.map((r) => {
        const out = { name: r.name, hero: heroes.has(r.name_key) ? heroes.get(r.name_key) : -1,
                      matches: r.matches, wins: r.wins, losses: r.losses };
        for (const k of STATS) out[k] = r[k];
        return out;
      }),
    });
  }
}
