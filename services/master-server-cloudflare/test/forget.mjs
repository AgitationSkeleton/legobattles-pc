// docs/443: removing score rows -- by a player's id and by the operator. Against a
// local `wrangler dev` (fresh store: delete .wrangler/state), never a live server:
//   npx wrangler dev --port 8788 --var SCORE_MIN_SECONDS:0 --var ADMIN_KEY:testadminkey
//   SERVER=http://127.0.0.1:8788 node test/forget.mjs
// (The fallback server: LB_FALLBACK_VARS=SCORE_MIN_SECONDS=0,ADMIN_KEY=testadminkey.)
// The rate-limit check is last: it uses up this address's ten tries an hour.
const SERVER = process.env.SERVER || 'http://127.0.0.1:8788';
const ADMIN = process.env.ADMIN_KEY || 'testadminkey';
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail) : '')); }
};

async function call(path, body, headers = {}, method) {
  const r = await fetch(SERVER + path, body === undefined && !method ? { headers } : {
    method: method || 'POST', headers: { 'Content-Type': 'application/json', ...headers },
    body: body === undefined ? undefined : JSON.stringify(body) });
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  return { status: r.status, data };
}
const admin = { Authorization: 'Bearer ' + ADMIN };
const KEY_A = 'a1'.repeat(16), KEY_B = 'b2'.repeat(16);
const stats = { units_built: 10, units_lost: 4, units_killed: 6, buildings_built: 3,
                buildings_lost: 1, buildings_killed: 2, bricks: 900 };

async function match(host, hostKey, joiner, joinKey, hostWins) {
  const a = await call('/announce', { name: 'LEGO Battles', host_name: host, port: 28700, players: 1,
                                      max_players: 4, version: 'test', player_key: hostKey });
  const { code, token } = a.data;
  await call('/join', { code, name: joiner, player_key: joinKey });
  await call('/heartbeat', { code, token, players: 2, state: 'playing', map_name: 'The Pond' });
  const r = await call('/score', { code, token, ruleset: 'Vanilla', cheats: 0, players: [
    { name: host, won: hostWins, hero: 133, ...stats }, { name: joiner, won: !hostWins, hero: 179, ...stats }] });
  await call('/remove', { code, token, reason: 'end' });
  return r;
}
const table = async () => (await call('/scores')).data.rows;
const row = (rows, n) => rows.find((x) => x.name === n);

let r = await match('Forget1', KEY_A, 'Forget2', KEY_B, true);
check('a match of two keyed players is counted', r.status === 200 && r.data.counted === 2, r);
await match('Forget1', KEY_A, 'Forget2', KEY_B, false);
await match('Forget3', null, 'Forget4', null, true);   // no keys: an old client
let rows = await table();
check('all four are listed', ['Forget1', 'Forget2', 'Forget3', 'Forget4'].every((n) => row(rows, n)), rows);
check('Forget1 has 2 matches', row(rows, 'Forget1').matches === 2, rows);

// No key can be read back from any public view.
const txt = JSON.stringify([(await call('/list')).data, (await call('/scores')).data,
                            (await call('/admin/lobbies', undefined, admin)).data]);
check('no key or hash is in a public or admin view', !txt.includes(KEY_A) && !/[0-9a-f]{64}/.test(txt), txt.slice(0, 200));

// --- the operator
r = await call('/admin/scores/names?q=forget', undefined, admin);
check('the admin name look-up lists them', r.status === 200 && r.data.names.length === 4, r);
r = await call('/admin/scores/names?q=forget');
check('the look-up needs the key', r.status === 403, r);
r = await call('/admin/scores/remove', { name: 'Forget3' });
check('removal by name fails without the key', r.status === 403, r);
r = await call('/admin/scores/remove', { name: 'Forget3' }, { Authorization: 'Bearer wrong' });
check('removal by name fails with a wrong key', r.status === 403, r);
rows = await table();
check('and removed nothing', row(rows, 'Forget3') && row(rows, 'Forget4'), rows);
r = await call('/admin/scores/remove', { name: 'fORGET3' }, admin);
check('removal by name (any case) with the key: 1 result, and its match is emptied only if both left',
      r.status === 200 && r.data.results === 1 && r.data.matches === 0, r);
r = await call('/admin/scores/remove', { name: 'Forget4' }, admin);
check('the other player of that match goes: now the match is gone',
      r.status === 200 && r.data.results === 1 && r.data.matches === 1, r);
r = await call('/admin/scores/remove', { name: '' }, admin);
check('an empty name is a 400', r.status === 400, r);

// --- /internal is not reachable from outside
for (const p of ['/internal/names?q=a', '/internal/score', '/internal/forget', '/internal/relay-auth?code=ABCD',
                 '/scores/../internal/names', '/scores/%2e%2e/internal/names', '//internal/names']) {
  const g = await call(p);
  const po = await call(p, {});
  check('outside ' + p + ' is refused', g.status >= 400 && po.status >= 400 && !(g.data && g.data.names), [g, po]);
}

// --- a player removes their own rows
r = await call('/scores/forget', {});
check('forget without a key is a 400', r.status === 400, r);
r = await call('/scores/forget', { player_key: 'abc123' });
check('forget with a short key is a 400', r.status === 400, r);
r = await call('/scores/forget', { player_key: 'c3'.repeat(16) });
check('a wrong key removes nothing', r.status === 200 && r.data.results === 0 && r.data.matches === 0, r);
rows = await table();
check('and the table is as it was', row(rows, 'Forget1') && row(rows, 'Forget2'), rows);
r = await call('/scores/forget', { player_key: KEY_A.toUpperCase() });
check('key A removes A\'s 2 rows; the shared matches stay for B',
      r.status === 200 && r.data.results === 2 && r.data.matches === 0, r);
rows = await table();
check('Forget1 is gone, Forget2 stays with its 2 matches',
      !row(rows, 'Forget1') && row(rows, 'Forget2') && row(rows, 'Forget2').matches === 2, rows);
r = await call('/scores/forget', { player_key: KEY_B });
check('key B removes B\'s rows and the now empty matches',
      r.status === 200 && r.data.results === 2 && r.data.matches === 2, r);
r = await call('/scores/forget', { player_key: KEY_B });
check('forgetting twice removes nothing', r.status === 200 && r.data.results === 0, r);

// --- the rate limit: ten tries an hour per address (four were spent above)
let limited = 0, n = 0;
for (; n < 12 && !limited; ++n) {
  const x = await call('/scores/forget', { player_key: 'd4'.repeat(16) });
  if (x.status === 429) limited = n + 1;
}
check('the eleventh try in the hour is refused', limited > 0 && limited + 4 === 11, { limited });
r = await call('/scores/forget', {});
check('a bad key is still a 400 and not a count', r.status === 400 || r.status === 429, r);

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
