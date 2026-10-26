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

// docs/443: a ruleset's family ("Rebalance 1.18.1" -> "Rebalance"): the name
// part, without a trailing version or "(PvE)". The game sends it as `preset`;
// this is for rows from before and for a report that lacks it. FunLite 1 and 2
// are one family.
function familyOf(ruleset) {
  let f = String(ruleset || '').trim().replace(/\s+(v?\d[\d.]*|\([^)]*\))$/i, '').trim() || 'Vanilla';
  if (/^funlite\d*$/i.test(f)) f = 'FunLite';
  return f.slice(0, 48);
}

// The ids of a `banned=12,40,77` list: numbers 0..4095, no repeats, at most 64.
function idList(text) {
  const out = new Set();
  for (const part of String(text || '').split(',')) {
    const n = Number.parseInt(part, 10);
    if (/^\s*\d+\s*$/.test(part) && n >= 0 && n < 4096) out.add(n);
    if (out.size >= 64) break;
  }
  return [...out].sort((a, b) => a - b);
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
    // docs/443: the player's id (sha256 of the key the game sent), so a player
    // can remove their own rows. Old tables upgrade; rows from before are NULL.
    try { this.sql.exec('ALTER TABLE results ADD COLUMN owner TEXT'); } catch { /* already there */ }
    // And the matches' preset family, so a table can merge a pack's versions.
    try { this.sql.exec('ALTER TABLE matches ADD COLUMN preset TEXT'); } catch { /* already there */ }
    for (const r of this.sql.exec('SELECT DISTINCT ruleset FROM matches WHERE preset IS NULL').toArray())
      this.sql.exec('UPDATE matches SET preset = ? WHERE preset IS NULL AND ruleset = ?', familyOf(r.ruleset), r.ruleset);
    this.sql.exec('CREATE INDEX IF NOT EXISTS matches_preset ON matches(preset)');
    this.sql.exec('CREATE INDEX IF NOT EXISTS results_owner ON results(owner)');
    this.sql.exec('CREATE INDEX IF NOT EXISTS results_match ON results(match_id)');
    this.sql.exec('CREATE INDEX IF NOT EXISTS matches_ruleset ON matches(ruleset)');
  }

  async fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname.replace(/\/+$/, '');
    if (request.method === 'POST' && path === '/internal/score') return this.record(await request.json());
    // docs/443: removals and the name look-up, reached only through the
    // Directory (its admin check, or its /scores/forget).
    if (request.method === 'POST' && path === '/internal/remove-name') return this.removeName(await request.json());
    if (request.method === 'POST' && path === '/internal/forget') return this.forget(await request.json());
    if (request.method === 'GET' && path === '/internal/names') return this.names(url.searchParams.get('q') || '');
    if (request.method === 'GET' && path === '/scores/rulesets') return this.rulesets();
    if (request.method === 'GET' && path === '/scores') return this.table(url.searchParams);
    return json(404, { ok: false, error: 'no such endpoint' });
  }

  // A report the Directory has already checked.
  record(m) {
    const banned = Array.isArray(m.banned) ? m.banned : [];
    const row = this.sql.exec(
      'INSERT INTO matches (at, ruleset, preset, bans, banned, map, seconds, version) VALUES (?, ?, ?, ?, ?, ?, ?, ?) RETURNING id',
      Date.now(), m.ruleset, m.preset || familyOf(m.ruleset), banned.length ? 1 : 0, ',' + banned.join(',') + ',',
      m.map || '', Math.max(0, m.seconds | 0), m.version || '').one();
    for (const p of m.players) {
      this.sql.exec(
        `INSERT INTO results (match_id, name, name_key, won, hero, ${STATS.join(', ')}, owner)
         VALUES (?, ?, ?, ?, ?, ${STATS.map(() => '?').join(', ')}, ?)`,
        row.id, p.name, p.name.toLowerCase(), p.won ? 1 : 0, p.hero,
        ...STATS.map((k) => count(p[k])), /^[0-9a-f]{64}$/.test(p.owner || '') ? p.owner : null);
    }
    return json(200, { ok: true, match: row.id });
  }

  // Delete the results matching `where`, then the matches left with none.
  removeWhere(where, arg) {
    const results = this.sql.exec(`SELECT COUNT(*) AS n FROM results WHERE ${where}`, arg).one().n;
    if (results) this.sql.exec(`DELETE FROM results WHERE ${where}`, arg);
    const before = this.sql.exec('SELECT COUNT(*) AS n FROM matches').one().n;
    this.sql.exec('DELETE FROM matches WHERE id NOT IN (SELECT DISTINCT match_id FROM results)');
    const after = this.sql.exec('SELECT COUNT(*) AS n FROM matches').one().n;
    return { results, matches: before - after };
  }

  removeName(b) {
    const name = String(b.name || '').trim().toLowerCase();
    if (!name) return json(400, { ok: false, error: 'name required' });
    return json(200, { ok: true, ...this.removeWhere('name_key = ?', name) });
  }

  forget(b) {
    const owner = String(b.owner || '');
    if (!/^[0-9a-f]{64}$/.test(owner)) return json(400, { ok: false, error: 'bad owner' });
    return json(200, { ok: true, ...this.removeWhere('owner = ?', owner) });
  }

  names(q) {
    const like = '%' + q.toLowerCase().slice(0, 48).replace(/[\\%_]/g, (c) => '\\' + c) + '%';
    const rows = this.sql.exec(
      `SELECT MAX(name) AS name, COUNT(*) AS results, SUM(owner IS NOT NULL) AS with_id
         FROM results WHERE name_key LIKE ? ESCAPE '\\' GROUP BY name_key ORDER BY results DESC LIMIT 100`,
      like).toArray();
    return json(200, { ok: true, names: rows });
  }

  rulesets() {
    const rows = this.sql.exec(
      'SELECT ruleset, preset, COUNT(*) AS matches FROM matches GROUP BY ruleset, preset ORDER BY matches DESC LIMIT 100').toArray();
    return json(200, { ok: true, rulesets: rows });
  }

  table(q) {
    const where = [];
    const args = [];
    const ruleset = q.get('ruleset');
    if (ruleset) { where.push('m.ruleset = ?'); args.push(ruleset.slice(0, 48)); }
    const preset = q.get('preset');   // every version of a family (docs/443)
    if (preset) { where.push('m.preset = ?'); args.push(preset.slice(0, 48)); }
    const bans = q.get('bans');
    if (bans === 'on') where.push('m.bans = 1');
    if (bans === 'off') where.push('m.bans = 0');
    // banned=12,40,77: all of them were banned (a lone number is the old form);
    // exact=1: the ban list is exactly those.
    const units = idList(q.get('banned'));
    if (units.length && q.get('exact') === '1') {
      where.push('m.banned = ?');
      args.push(',' + units.join(',') + ',');
    } else {
      for (const unit of units) { where.push('m.banned LIKE ?'); args.push('%,' + unit + ',%'); }
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
