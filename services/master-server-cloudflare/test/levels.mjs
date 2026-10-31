// Community levels (src/levels.js), against a local `wrangler dev` on a fresh
// store, with test values for the secrets and an innocent test word standing
// in for a swear word:
//   npx wrangler dev --port 8811 --var LEVELS_KEY:testkey --var LEVELS_ADMIN_USER:100000000000000001 --var SWEAR_TEST_WORDS:blip
//   SERVER=http://127.0.0.1:8811 MAP=../../out/eur/plain/Maps/mp01.map node test/levels.mjs
// Local only: never point it at the live server. No Discord token is set, so
// nothing is posted anywhere.
import { readFileSync } from 'node:fs';

const SERVER = process.env.SERVER || 'http://127.0.0.1:8811';
const MAP = process.env.MAP || '../../out/eur/plain/Maps/mp01.map';
const TBP = process.env.TBP || '../../out/eur/plain/BP/DetailTiles_mp01.tbp';
const ADMIN = '100000000000000001';   // the test moderator (--var LEVELS_ADMIN_USER)
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail).slice(0, 300) : '')); }
};
async function call(path, body, headers = {}) {
  const r = await fetch(SERVER + path, body === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body) });
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  return { status: r.status, data };
}
const admin = (custom_id, user_id = ADMIN, key = 'testkey') =>
  call('/levels/admin', { user_id, custom_id }, { 'X-Levels-Key': key });

const map = readFileSync(MAP);
const tbp = readFileSync(TBP);
const b64 = (u8) => Buffer.from(u8).toString('base64');
const key1 = 'a'.repeat(32), key2 = 'b'.repeat(32), key3 = 'c'.repeat(32);
const up = (o) => call('/levels/upload', { key: key1, name: 'Test Pond', author: 'Tester', map: b64(map), tbp: b64(tbp), ...o });

// Refusals before anything is stored.
check('a bad key is refused', (await up({ key: 'nothex' })).status === 400);
check('a swearing name is refused', (await up({ name: 'The blip pond' })).status === 400);
check('a swearing author is refused', (await up({ author: 'blip' })).status === 400);
check('markup in a name is refused', (await up({ name: '<script>' })).status === 400);
const cut = map.subarray(0, map.length - 10);
check('a truncated map is refused', (await up({ map: b64(cut) })).status === 400);
const bad = Buffer.from(map);
bad[3 + 4 + 0x28 + 5] = 9;   // a terrain class the game has not got
let r = await up({ map: b64(bad) });
check('a bad terrain class is refused', r.status === 400 && /terrain class/.test(r.data.error), r);
const extra = Buffer.concat([map, Buffer.from('X')]);
check('bytes after the map are refused', (await up({ map: b64(extra) })).status === 400);
check('detail tiles of the wrong length are refused', (await up({ tbp: b64(tbp.subarray(0, 50)) })).status === 400);
check('a map without its detail tiles is refused (tile index)', (await up({ tbp: '' })).status === 400);
check('not base64 is refused', (await up({ map: '!!!' })).status === 400);

// A good upload waits for review.
r = await up();
check('a good upload is accepted as pending', r.status === 200 && r.data.status === 'pending', r);
const id = r.data && r.data.id;
check('the same level again is refused', (await up({ key: key2 })).status === 409);
let list = await call('/levels/list');
check('a pending level is not listed', list.status === 200 && !list.data.levels.some((l) => l.id === id), list);
check('a pending level cannot be fetched', (await call('/levels/get?id=' + id)).status === 404);

// The bot's door.
check('admin without the key is refused', (await admin('lvl:approve:' + id, ADMIN, 'wrong')).status === 403);
r = await admin('lvl:approve:' + id, '123');
check('someone else pressing Approve is told no', r.status === 200 && /Only the moderator/.test(r.data.reply), r);
check('...and the level stays pending', !(await call('/levels/list')).data.levels.some((l) => l.id === id));
r = await admin('lvl:approve:' + id);
check('the moderator approves', r.status === 200 && r.data.update && /approved/.test(JSON.stringify(r.data.update)), r);
list = await call('/levels/list');
const row = list.data.levels.find((l) => l.id === id);
check('an approved level is listed', !!row && row.name === 'Test Pond' && row.players === 4 && row.world === 0, list);
r = await call('/levels/get?id=' + id);
check('it downloads byte for byte', r.status === 200 && Buffer.from(r.data.map, 'base64').equals(map) &&
      Buffer.from(r.data.tbp, 'base64').equals(tbp), { status: r.status });

// Settings from the panel.
r = await admin('lvl:panel');
check('the panel message is offered', r.status === 200 && r.data.post && r.data.post.components.length === 3, r);
r = await admin('lvl:cfg:auto');
check('auto-approve turns on', /Auto-approve: \*\*On/.test(JSON.stringify(r.data.update)), r);
const two = Buffer.from(map);
two[two.length - 4] = 0x21;   // same bytes: change a marker instead below
const other = Buffer.from(map);
// A second, different level: move one golden brick (event 0's first pickup).
const pickAt = other.indexOf(Buffer.from([0x14, 0x08, 0xff]));
if (pickAt > 3) other[pickAt - 3] = (other[pickAt - 3] + 1) & 0x3f;
r = await call('/levels/upload', { key: key2, name: 'Second', author: '', map: b64(other), tbp: b64(tbp) });
check('with auto-approve a level goes straight up', r.status === 200 && r.data.status === 'approved', r);
r = await admin('lvl:cfg:uploads');
check('uploads turn off', /Uploads: \*\*Off/.test(JSON.stringify(r.data.update)), r);
check('...and an upload is refused', (await call('/levels/upload', { key: key3, name: 'Third', map: b64(map), tbp: b64(tbp) })).status === 403);
await admin('lvl:cfg:uploads');
await admin('lvl:cfg:auto');

// Ban: the uploader's levels come down and they cannot upload again.
r = await admin('lvl:ban:' + id);
check('ban', r.status === 200 && /banned/.test(JSON.stringify(r.data.update)), r);
check('a banned uploader\'s level is unlisted', !(await call('/levels/list')).data.levels.some((l) => l.id === id));
const third = Buffer.from(other);
if (pickAt > 3) third[pickAt - 2] = (third[pickAt - 2] + 1) & 0x3f;
check('a banned uploader cannot upload', (await call('/levels/upload', { key: key1, name: 'Again', map: b64(third), tbp: b64(tbp) })).status === 403);

// Owner delete.
const second = (await call('/levels/list')).data.levels.find((l) => l.name === 'Second');
check('someone else cannot delete a level', (await call('/levels/delete', { key: key3, id: second.id })).status === 404);
check('its uploader can', (await call('/levels/delete', { key: key2, id: second.id })).status === 200);

// SQL-shaped input is only ever data.
r = await call('/levels/get?id=1%20OR%201=1');
check('an id with SQL in it is refused', r.status === 400 || r.status === 404, r);
r = await call("/levels/list?limit=1;DROP TABLE levels&offset=0");
check('list survives SQL in its parameters', r.status === 200, r);

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
