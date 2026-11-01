// Community units (src/units.js), against a local `wrangler dev` on a fresh
// store, started as for test/levels.mjs:
//   npx wrangler dev --port 8811 --var LEVELS_KEY:testkey --var LEVELS_ADMIN_USER:100000000000000001 --var SWEAR_TEST_WORDS:blip
//   SERVER=http://127.0.0.1:8811 UNIT=<a unit folder> node test/units.mjs
// UNIT is a folder as Balance > Custom units writes it (unit.json, sheets/,
// portraits/, sounds/). Local only: never point it at the live server. No
// Discord token is set, so nothing is posted anywhere.
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const SERVER = process.env.SERVER || 'http://127.0.0.1:8811';
const UNIT = process.env.UNIT || '../../port/runner/build/mods/units/RedEngineer';
const ADMIN = '100000000000000001';
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

// The bundle exactly as the port makes it (lb_cunits.cpp make_bundle).
function files(dir) {
  const v = [];
  if (existsSync(join(dir, 'unit.json'))) v.push('unit.json');
  if (existsSync(join(dir, 'sheets')))
    for (const n of readdirSync(join(dir, 'sheets'))) if (/\.(png|NCBR)$/.test(n)) v.push('sheets/' + n);
  if (existsSync(join(dir, 'sounds')))
    for (const n of readdirSync(join(dir, 'sounds'))) if (n.endsWith('.wav')) v.push('sounds/' + n);
  for (const f of ['portraits/card.png', 'portraits/card_top_screen.png', 'portraits/head.png', 'model/texture.png'])
    if (existsSync(join(dir, f))) v.push(f);
  return v.sort();
}
function bundle(entries) {
  const parts = [Buffer.from('LBUB\x01'), Buffer.from([entries.length & 255, entries.length >> 8])];
  for (const [p, d] of entries) {
    const len = Buffer.alloc(4);
    len.writeUInt32LE(d.length);
    parts.push(Buffer.from([p.length & 255, p.length >> 8]), Buffer.from(p), len, d);
  }
  return Buffer.concat(parts);
}
const entries = files(UNIT).map((p) => [p, readFileSync(join(UNIT, p))]);
const good = bundle(entries);
const unitJson = JSON.parse(readFileSync(join(UNIT, 'unit.json'), 'utf8'));
const sheet = readFileSync(join(UNIT, 'sheets', readdirSync(join(UNIT, 'sheets')).find((n) => n.endsWith('.png'))));
const thumb = readFileSync(join(UNIT, 'portraits', 'head.png'));
const wavs = existsSync(join(UNIT, 'sounds')) ? readdirSync(join(UNIT, 'sounds')).filter((n) => n.endsWith('.wav')) : [];
const sounds = wavs.slice(0, 3).map((n) => ({ name: n.replace(/\.wav$/, ''), wav: readFileSync(join(UNIT, 'sounds', n)).toString('base64') }));
const b64 = (u8) => Buffer.from(u8).toString('base64');
const key1 = 'a'.repeat(32), key2 = 'b'.repeat(32), key3 = 'd'.repeat(32);
const up = (o) => call('/units/upload', { key: key1, name: 'Red Test', author: 'Tester', category: unitJson.category,
  template: unitJson.index, bundle: b64(good), sheet: b64(sheet), thumb: b64(thumb), sounds, ...o });

// Refusals before anything is stored.
check('a bad key is refused', (await up({ key: 'nothex' })).status === 400);
check('a swearing name is refused', (await up({ name: 'blip guard' })).status === 400);
check('markup in a name is refused', (await up({ name: '<b>x</b>' })).status === 400);
check('a bad category is refused', (await up({ category: 20 })).status === 400);
check('a template that is not the bundle\'s is refused', (await up({ template: unitJson.index + 1 })).status === 400);
check('a truncated bundle is refused', (await up({ bundle: b64(good.subarray(0, good.length - 3)) })).status === 400);
check('bytes after the bundle are refused', (await up({ bundle: b64(Buffer.concat([good, Buffer.from('X')])) })).status === 400);
let r = await up({ bundle: b64(bundle([...entries, ['../evil.png', thumb]])) });
check('a path outside the unit is refused', r.status === 400 && /path/.test(r.data.error), r);
r = await up({ bundle: b64(bundle([...entries, ['sounds/x.exe', thumb]])) });
check('a file the importer does not read is refused', r.status === 400, r);
r = await up({ bundle: b64(bundle([...entries, ['sounds/x.wav', thumb]])) });
check('a bundled sound that is not a WAV is refused', r.status === 400 && /WAV/.test(r.data.error), r);
check('the bundle carries its sounds', entries.some(([p]) => p.startsWith('sounds/')));
r = await up({ bundle: b64(bundle(entries.map(([p, d]) => [p, p.endsWith('.png') ? Buffer.from('not a png') : d]))) });
check('a PNG that is not a PNG is refused', r.status === 400 && /PNG/.test(r.data.error), r);
r = await up({ bundle: b64(bundle(entries.map(([p, d]) => [p, p === 'unit.json' ? Buffer.from('{"index": 999}') : d]))) });
check('a template index out of range is refused', r.status === 400, r);
r = await up({ bundle: b64(bundle(entries.map(([p, d]) => [p, p === 'unit.json' ? Buffer.from('{"index":2,"fields":{"cost":"x"}}') : d]))) });
check('a field that is not a number is refused', r.status === 400, r);
check('a preview sheet that is not a PNG is refused', (await up({ sheet: b64(Buffer.from('GIF89a')) })).status === 400);
check('a sound that is not a WAV is refused', (await up({ sounds: [{ name: 'x', wav: b64(thumb) }] })).status === 400);
check('a sound with a bad name is refused', (await up({ sounds: [{ name: '../x', wav: sounds[0] ? sounds[0].wav : '' }] })).status === 400);
check('nine sounds are refused', (await up({ sounds: Array(9).fill(sounds[0]) })).status === 400);
check('not base64 is refused', (await up({ bundle: '!!!' })).status === 400);

// A good upload waits for review.
r = await up();
check('a good upload is accepted as pending', r.status === 200 && r.data.status === 'pending', r);
const id = r.data && r.data.id;
check('the same unit again is refused', (await up({ key: key2 })).status === 409);
check('a pending unit is not listed', !(await call('/units/list')).data.units.some((u) => u.id === id));
check('a pending unit cannot be fetched', (await call('/units/get?id=' + id)).status === 404);

// The bot's door, shared with levels.
check('admin without the key is refused', (await admin('lvl:unit:approve:' + id, ADMIN, 'wrong')).status === 403);
r = await admin('lvl:unit:approve:' + id, '123');
check('someone else pressing Approve is told no', /Only the moderator/.test(r.data.reply || ''), r);
r = await admin('lvl:unit:approve:' + id);
check('the moderator approves', r.status === 200 && /approved/.test(JSON.stringify(r.data.update)), r);
check('the approved message offers Unpublish', /lvl:unit:reject:/.test(JSON.stringify(r.data.update)), r);
r = await admin('lvl:panel');
check('the panel counts units', /Units: pending 0, published 1/.test(JSON.stringify(r.data.post)), r);
let list = await call('/units/list');
const row = list.data.units.find((u) => u.id === id);
check('an approved unit is listed', !!row && row.name === 'Red Test' && row.template === unitJson.index &&
      row.category === unitJson.category && row.sounds === sounds.length, list);
check('the category filter keeps it', (await call('/units/list?category=' + unitJson.category)).data.units.some((u) => u.id === id));
check('another category leaves it out', !(await call('/units/list?category=' + ((unitJson.category + 1) % 20))).data.units.some((u) => u.id === id));
r = await call('/units/get?id=' + id);
check('it downloads byte for byte', r.status === 200 && Buffer.from(r.data.bundle, 'base64').equals(good), r.status);
r = await call('/units/thumb?id=' + id);
check('the thumbnail is served', r.status === 200 && Buffer.from(r.data.png, 'base64').equals(thumb), r.status);
await call('/units/get?id=' + id + '&count=1');
check('a counted fetch is a download', (await call('/units/list')).data.units.find((u) => u.id === id).downloads === 1);

// Likes.
r = await call('/units/like', { key: key2, id, like: true });
check('a like counts', r.status === 200 && r.data.likes === 1, r);
await call('/units/like', { key: key2, id, like: true });
check('a second like from the same player does not', (await call('/units/like', { key: key3, id, like: true })).data.likes === 2);
check('an unlike takes it back', (await call('/units/like', { key: key3, id, like: false })).data.likes === 1);

// Deleting: only the uploader.
check('someone else cannot delete it', (await call('/units/delete', { key: key2, id })).status === 404);

// A ban takes down everything the uploader sent and stops new uploads.
const second = bundle(entries.map(([p, d]) => [p, p === 'unit.json' ? Buffer.from(JSON.stringify({ ...unitJson, name: 'B' })) : d]));
r = await up({ name: 'Red Two', bundle: b64(second) });
check('a second unit uploads', r.status === 200, r);
const id2 = r.data && r.data.id;
r = await admin('lvl:unit:ban:' + id2);
check('the moderator bans the uploader', /banned/.test(JSON.stringify(r.data.update)), r);
check('their approved unit comes down too', !(await call('/units/list')).data.units.some((u) => u.id === id));
check('they cannot upload again', (await up({ name: 'Red Three', bundle: b64(bundle(entries.slice(1).concat([entries[0]]))) })).status === 403);
check('an unknown unit button is answered', /Unknown/.test((await admin('lvl:unit:explode:1')).data.reply || ''));

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
