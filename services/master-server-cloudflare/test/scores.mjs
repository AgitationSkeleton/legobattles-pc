// SG-0018: the score tables, against a local `wrangler dev` with the minimum match
// length off and an innocent test word standing in for a swear word:
//   npx wrangler dev --port 8788 --var SCORE_MIN_SECONDS:0 --var SWEAR_TEST_WORDS:blip
//   SERVER=http://127.0.0.1:8788 node test/scores.mjs
// Local only: never point it at the live server. Run it on a fresh dev store
// (delete .wrangler/state) so the tables start empty.
const SERVER = process.env.SERVER || 'http://127.0.0.1:8788';
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail) : '')); }
};

async function call(path, body) {
  const r = await fetch(SERVER + path, body === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  return { status: r.status, data };
}

const stats = (o) => ({ units_built: 10, units_lost: 4, units_killed: 6, buildings_built: 3,
                        buildings_lost: 1, buildings_killed: 2, bricks: 900, ...o });

// A lobby: Tester1 hosts, the others join; the match starts.
async function lobby(host, joiners, opts = {}) {
  const a = await call('/announce', { name: 'LEGO Battles', host_name: host, port: 28700, players: 1,
                                      max_players: 4, version: 'test', no_scores: !!opts.hostOut });
  const { code, token } = a.data;
  for (const j of joiners) await call('/join', { code, name: j, no_scores: !!(opts.out || []).includes(j) });
  await call('/heartbeat', { code, token, players: 1 + joiners.length, state: 'playing', map_name: 'The Pond' });
  return { code, token };
}

let l = await lobby('Tester1', ['Tester2']);
const two = [{ name: 'Tester1', won: true, hero: 133, ...stats() },
             { name: 'Tester2', won: false, hero: 179, ...stats({ units_lost: 9 }) }];
let r = await call('/score', { ...l, token: 'wrong', ruleset: 'Vanilla', cheats: 0, players: two });
check('a wrong token is refused', r.status === 404, r);
r = await call('/score', { ...l, ruleset: 'Vanilla', cheats: 1, players: two });
check('a match with cheats is refused', r.status === 422, r);
r = await call('/score', { ...l, ruleset: 'Vanilla', cheats: 0,
                           players: [two[0], { ...two[1], name: 'Stranger' }] });
check('a name not in the lobby is refused', r.status === 422, r);
r = await call('/score', { ...l, ruleset: 'Vanilla', cheats: 0, players: [two[0]] });
check('one player is not a match', r.status === 422, r);
r = await call('/score', { ...l, ruleset: 'Vanilla', cheats: 0, banned: [], players: two });
check('a good report counts both players', r.status === 200 && r.data.counted === 2, r);
r = await call('/score', { ...l, ruleset: 'Vanilla', cheats: 0, players: two });
check('a second report of the match is refused', r.status === 409, r);

// The next match in the same lobby gets its own report.
await call('/heartbeat', { ...l, players: 2, state: 'open' });
await call('/heartbeat', { ...l, players: 2, state: 'playing' });
r = await call('/score', { ...l, ruleset: 'Vanilla', cheats: 0, players: [
  { ...two[0], won: false, hero: 80 }, { ...two[1], won: true }] });
check('the next match reports again', r.status === 200 && r.data.counted === 2, r);
await call('/remove', { ...l, reason: 'end' });

// Bans, a ruleset with a '+', an opted-out joiner, a swearing name.
l = await lobby('Tester3', ['Tester1', 'Tester4', 'Blips'], { out: ['Tester4'] });
r = await call('/score', { ...l, ruleset: 'Vanil+ 1.2.1', cheats: 0, banned: [177, 133],
                           players: [{ name: 'Tester3', won: true, hero: 81, ...stats() },
                                     { name: 'tester1', won: false, hero: 133, ...stats() },
                                     { name: 'Tester4', won: false, hero: 80, ...stats() },
                                     { name: 'Blips', won: false, hero: 80, ...stats() }] });
check('the opted-out player and the swearing name are left out',
      r.status === 200 && r.data.counted === 2 && r.data.skipped === 2, r);
await call('/remove', { ...l, reason: 'end' });

l = await lobby('Tester5', ['Tester6'], { hostOut: true });
r = await call('/score', { ...l, ruleset: 'Vanilla', cheats: 0, players: [
  { name: 'Tester5', won: true, hero: 81, ...stats() }, { name: 'Tester6', won: false, hero: 81, ...stats() }] });
check('a host who opted out is left out', r.status === 200 && r.data.counted === 1, r);
await call('/remove', { ...l, reason: 'end' });

// The game's lobby record keeps 11 characters of a joiner's name.
l = await lobby('Tester7', ['TesterLongName']);
r = await call('/score', { ...l, ruleset: 'Fusion 1.2', cheats: 0, players: [
  { name: 'Tester7', won: true, hero: 81, ...stats() }, { name: 'TesterLongN', won: false, hero: 81, ...stats() },
  { name: 'Nobody', won: false, hero: 81, ...stats() }] });
check('an 11-character name matches its player; an unknown one is left out',
      r.status === 200 && r.data.counted === 2 && r.data.skipped === 1, r);
await call('/remove', { ...l, reason: 'end' });
r = await call('/scores?ruleset=Fusion%201.2');
check('the full name is listed', r.data.rows.some((x) => x.name === 'TesterLongName'), r.data.rows);

r = await call('/scores/rulesets');
const names = (r.data && r.data.rulesets || []).map((x) => x.ruleset);
check('the rulesets are listed', names.includes('Vanilla') && names.includes('Vanil+ 1.2.1'), r.data);

r = await call('/scores?ruleset=Vanilla');
const rows = r.data && r.data.rows || [];
const t1 = rows.find((x) => x.name === 'Tester1');
const t2 = rows.find((x) => x.name === 'Tester2');
check('Vanilla: Tester1 1 win 1 loss', t1 && t1.wins === 1 && t1.losses === 1 && t1.matches === 2, rows);
check('Vanilla: Tester2 1 win 1 loss, units lost summed', t2 && t2.wins === 1 && t2.units_lost === 18, rows);
r = await call('/scores');
check('nobody opted out or swearing is listed',
      !r.data.rows.some((x) => x.name === 'Blips' || x.name === 'Tester4'), r.data.rows);
check('Vanilla: Tester6 listed, the opted-out host Tester5 not',
      rows.some((x) => x.name === 'Tester6') && !rows.some((x) => x.name === 'Tester5'), rows);

r = await call('/scores');
const all = r.data.rows;
const a1 = all.find((x) => x.name === 'Tester1');
check('all rulesets: Tester1 3 matches, a name in any case is one player', a1 && a1.matches === 3, all);
check('all rulesets: Tester1 most-played hero 133', a1 && a1.hero === 133, a1);
check('sorted by wins', all.every((x, i) => i === 0 || all[i - 1].wins >= x.wins), all);

// SG-0039: the game's "All" table, one row per player and preset.
r = await call('/scores?by=preset');
const per = r.data.rows.filter((x) => x.name === 'Tester1');
const pv = per.find((x) => x.preset === 'Vanilla');
check('by preset: Tester1 has a row per preset, each tagged',
      per.length >= 2 && per.every((x) => typeof x.preset === 'string' && x.preset), per);
check('by preset: Tester1 Vanilla row is the Vanilla table\'s (2 matches)', pv && pv.matches === 2, pv);
check('by preset: the rows add up to the merged table', per.reduce((n, x) => n + x.matches, 0) === 3, per);
check('without by=preset no row carries a preset', all.every((x) => x.preset === undefined), all[0]);

r = await call('/scores?bans=on');
check('bans on: only the banned match', r.data.rows.length === 2 &&
      r.data.rows.every((x) => ['Tester3', 'Tester1'].includes(x.name)), r.data.rows);
r = await call('/scores?bans=off&ruleset=Vanilla');
check('bans off: the Vanilla matches', r.data.rows.some((x) => x.name === 'Tester2'), r.data.rows);
r = await call('/scores?banned=177');
check('Sheriff (177) banned: that match only', r.data.rows.length === 2, r.data.rows);
r = await call('/scores?banned=999');
check('a unit never banned: no rows', r.data.rows.length === 0, r.data.rows);

// docs/443: preset families, and ban lists (all-of, exact). New players, so the
// checks above are not touched.
async function played(host, join, ruleset, banned, preset) {
  const lb = await lobby(host, [join]);
  const rr = await call('/score', { ...lb, ruleset, preset, cheats: 0, banned, players: [
    { name: host, won: true, hero: 81, ...stats() }, { name: join, won: false, hero: 81, ...stats() }] });
  await call('/remove', { ...lb, reason: 'end' });
  return rr;
}
await played('Fam1', 'Fam2', 'Rebalance 1.18.1', [12, 40, 77], 'Rebalance');
await played('Fam1', 'Fam2', 'Rebalance 1.15.2', [12, 40], undefined);   // an old client: no preset
await played('Fam1', 'Fam2', 'Rebalance 1.15.2', [], 'Rebalance');
await played('Fam3', 'Fam4', 'FunLite2 (PvE)', [], undefined);
r = await call('/scores?preset=Rebalance');
const fam1 = r.data.rows.find((x) => x.name === 'Fam1');
check('two Rebalance versions merge under one preset', fam1 && fam1.matches === 3 && !r.data.rows.some((x) => x.name === 'Fam3'), r.data.rows);
r = await call('/scores?ruleset=Rebalance%201.15.2');
check('an exact ruleset is still one version', r.data.rows.find((x) => x.name === 'Fam1').matches === 2, r.data.rows);
r = await call('/scores?preset=FunLite');
check('a report without a preset is filed under its derived family (FunLite2 (PvE) -> FunLite)',
      r.data.rows.some((x) => x.name === 'Fam3'), r.data.rows);
r = await call('/scores/rulesets');
check('rulesets carry their preset', r.data.rulesets.some((x) => x.ruleset === 'Rebalance 1.15.2' && x.preset === 'Rebalance'), r.data);
const fm = async (q) => ((await call('/scores?preset=Rebalance&' + q)).data.rows.find((x) => x.name === 'Fam1') || { matches: 0 }).matches;
check('bans=off: the one match with no bans', await fm('bans=off') === 1);
check('bans=on: the two with bans', await fm('bans=on') === 2);
check('banned=12,40: all of them banned (both bans matches)', await fm('bans=on&banned=12,40') === 2);
check('banned=12,40,77: only the match with all three', await fm('bans=on&banned=12,40,77') === 1);
check('banned=40,77,12 in any order', await fm('bans=on&banned=40,77,12') === 1);
check('banned=77 alone (the old single form)', await fm('banned=77') === 1);
check('exact=1 with 12,40 is only the match banning exactly those', await fm('bans=on&banned=12,40&exact=1') === 1);
check('exact=1 with 12,40,77', await fm('bans=on&banned=12,40,77&exact=1') === 1);
check('exact=1 with 12 is none', await fm('bans=on&banned=12&exact=1') === 0);
check('a unit never banned among them: none', await fm('bans=on&banned=12,999') === 0);
check('junk ids are ignored, not matched', await fm('bans=on&banned=12,abc,-3,99999,40') === 2);
const many = Array.from({ length: 80 }, (_, i) => i + 100).join(',');
check('a list longer than 64 is capped (and so matches nothing)', await fm('banned=' + many) === 0);

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
