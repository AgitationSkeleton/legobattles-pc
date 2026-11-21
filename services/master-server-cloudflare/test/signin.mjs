// docs/501: Discord sign-in, against `wrangler dev` with the gate on and a
// throwaway signing key (never the production one):
//   node test/signin.mjs --make-key DEVKEY.json      (once: writes a dev key)
//   npx wrangler dev --port 8796 --var REQUIRE_SIGNIN:1 --var LEVELS_KEY:testkey \
//       --var "AUTH_SIGNING_KEY:$(cat DEVKEY.json)"
//   SERVER=http://127.0.0.1:8796 KEY=DEVKEY.json MAP=../../out/eur/plain/Maps/mp01.map node test/signin.mjs
// Tokens are made here with the same key, as the server's /auth/callback
// would after Discord answered. Local only.
import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import crypto from 'node:crypto';

if (process.argv[2] === '--make-key') {
  const { privateKey } = crypto.generateKeyPairSync('ed25519');
  writeFileSync(process.argv[3], JSON.stringify(privateKey.export({ format: 'jwk' })));
  process.exit(0);
}

const SERVER = process.env.SERVER || 'http://127.0.0.1:8796';
const jwk = JSON.parse(readFileSync(process.env.KEY, 'utf8'));
const priv = crypto.createPrivateKey({ key: jwk, format: 'jwk' });
const MAP = process.env.MAP || '../../out/eur/plain/Maps/mp01.map';
const V = '0.4.26';
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail).slice(0, 300) : '')); }
};
const b64u = (b) => Buffer.from(b).toString('base64url');
function token(sub, name, days = 30) {
  const iat = Math.floor(Date.now() / 1000);
  const body = 'v1.' + b64u(JSON.stringify({ sub, uid: 'LB-TEST0000', name, aud: 'main', iat, exp: iat + days * 86400 }));
  return body + '.' + b64u(crypto.sign(null, Buffer.from(body), priv));
}
async function call(method, path, body, query = '') {
  const r = await fetch(SERVER + path + query, {
    method, headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  let data = null;
  try { data = await r.json(); } catch { /* none */ }
  return { status: r.status, data };
}
const ALICE = '111111111111111111', BOB = '222222222222222222';
const ta = token(ALICE, 'Alice'), tb = token(BOB, 'Bob');

// The gate.
check('list without a version: 426', (await call('GET', '/list')).status === 426);
check('list from an old version: 426', (await call('GET', '/list', null, '?v=0.4.25')).status === 426);
let r = await call('GET', '/list', null, `?v=${V}`);
check('list with no token: 401 with the sign-in message', r.status === 401 && /Sign in/.test(r.data.message), r);
check('a forged token: 401', (await call('GET', '/list', null, `?v=${V}&auth=${ta.slice(0, -4)}AAAA`)).status === 401);
check('an expired token: 401', (await call('GET', '/list', null, `?v=${V}&auth=${token(ALICE, 'Alice', -1)}`)).status === 401);
check('signed in: 200', (await call('GET', '/list', null, `?v=${V}&auth=${ta}`)).status === 200);
check('/health stays open', (await call('GET', '/health')).status === 200);

// A lobby from a signed-in host; a signed-in joiner.
r = await call('POST', '/announce', { auth: ta, version: V, name: 'LEGO Battles', host_name: 'Alice', port: 28700, max_players: 3 });
check('announce signed in', r.status === 200 && r.data.code, r);
const lobby = r.data;
check('announce without a token: 401', (await call('POST', '/announce', { version: V, port: 28701, max_players: 3 })).status === 401);
r = await call('POST', '/join', { auth: tb, version: V, code: lobby.code, name: 'Bob' });
check('join signed in', r.status === 200, r);

// /auth/verify (King's check) and the key.
r = await call('POST', '/auth/verify', { token: ta });
check('verify: the account', r.data.ok && r.data.sub === ALICE, r);
check('verify: a bad token', (await call('POST', '/auth/verify', { token: 'v1.x.y' })).data.ok === false);
check('the public key is served', !!(await call('GET', '/auth/key')).data.key);

// Uploads: signed in only; mine, hide, edit, delete.
const map = readFileSync(MAP);
const tbp = readFileSync(process.env.TBP || '../../out/eur/plain/BP/DetailTiles_mp01.tbp');
const up = (o) => call('POST', '/levels/upload', { name: 'Signed Pond', author: '', map: map.toString('base64'), tbp: tbp.toString('base64'), ...o });
check('an upload without a token: 401', (await up({ key: '0'.repeat(32) })).status === 401);
r = await up({ auth: ta });
check('a signed-in upload', r.status === 200 && r.data.id, r);
const lid = r.data.id;
r = await call('POST', '/levels/mine', { auth: ta });
check('mine lists it', r.status === 200 && r.data.levels.some((l) => l.id === lid), r);
check('another player cannot hide it', (await call('POST', '/levels/hide', { auth: tb, id: lid })).status === 404);
check('the uploader hides it', (await call('POST', '/levels/hide', { auth: ta, id: lid, hidden: true })).data.hidden === true);
check('the uploader shows it again', (await call('POST', '/levels/hide', { auth: ta, id: lid, hidden: false })).data.hidden === false);
r = await call('POST', '/levels/edit', { auth: ta, id: lid, name: 'Renamed Pond' });
check('the uploader renames it', r.status === 200 && r.data.name === 'Renamed Pond', r);

// A unit, the same way (test/units.mjs makes the bundle).
const UNIT = process.env.UNIT || '../../port/runner/build/mods/units/RedEngineer';
const uj = JSON.parse(readFileSync(join(UNIT, 'unit.json'), 'utf8'));
const uf = ['unit.json', ...readdirSync(join(UNIT, 'sheets')).filter((n) => /\.(png|NCBR)$/.test(n)).map((n) => 'sheets/' + n),
  ...(existsSync(join(UNIT, 'sounds')) ? readdirSync(join(UNIT, 'sounds')).filter((n) => n.endsWith('.wav')).map((n) => 'sounds/' + n) : []),
  ...['portraits/card.png', 'portraits/card_top_screen.png', 'portraits/card_mask.png', 'portraits/head.png', 'portraits/hud_icon.png',
      'portraits/build_icon.png', 'portraits/end_victory.png', 'portraits/end_defeat.png', 'model/texture.png', 'model/model.obj']
    .filter((f) => existsSync(join(UNIT, f)))].sort();
const parts = [Buffer.from('LBUB\x01'), Buffer.from([uf.length & 255, uf.length >> 8])];
for (const f of uf) {
  const d = readFileSync(join(UNIT, f)), len = Buffer.alloc(4);
  len.writeUInt32LE(d.length);
  parts.push(Buffer.from([f.length & 255, f.length >> 8]), Buffer.from(f), len, d);
}
const sheetName = readdirSync(join(UNIT, 'sheets')).find((n) => n.endsWith('.png'));
r = await call('POST', '/units/upload', { auth: ta, name: 'Signed Red', author: '', category: uj.category, template: uj.index,
  bundle: Buffer.concat(parts).toString('base64'), sheet: readFileSync(join(UNIT, 'sheets', sheetName)).toString('base64'),
  thumb: readFileSync(join(UNIT, 'portraits', 'head.png')).toString('base64'), sounds: [] });
check('a signed-in unit upload', r.status === 200 && r.data.id, r);
const uid1 = r.data && r.data.id;
r = await call('POST', '/levels/mine', { auth: ta });
check('mine lists the unit', r.status === 200 && r.data.units.some((u) => u.id === uid1), r);
check('the uploader hides the unit', (await call('POST', '/units/hide', { auth: ta, id: uid1, hidden: true })).data.hidden === true);
check('the uploader shows the unit', (await call('POST', '/units/hide', { auth: ta, id: uid1, hidden: false })).data.hidden === false);

// A moderator bans Alice: her lobby closes, her level comes down, her token stops working.
const admin = (id, user = '748343958190555217') => fetch(SERVER + '/levels/admin', {
  method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Levels-Key': 'testkey' },
  body: JSON.stringify({ user_id: user, custom_id: id }) }).then((x) => x.json());
r = await admin(`lvl:acct:ban:${ALICE}`, '999999999999999999');
check('a non-moderator cannot ban', /moderator/i.test(r.reply || ''), r);
r = await admin(`lvl:acct:ban:${ALICE}`);
check('a moderator bans', /Banned/.test(r.reply || ''), r);
await new Promise((res) => setTimeout(res, 1500));
r = await call('POST', '/auth/verify', { token: ta });
check('the banned token is refused', r.data.ok === false, r);
r = await call('POST', '/levels/mine', { auth: tb });
check('the other player is unaffected', r.status === 200);
r = await call('GET', '/levels/list');
check('the banned uploader\'s level is gone from the list', !(r.data.levels || []).some((l) => l.id === lid), r);
r = await call('GET', '/units/list');
check('the banned uploader\'s unit is gone from the list', !(r.data.units || []).some((u) => u.id === uid1), r);
r = await call('GET', `/lobby/${lobby.code}`, null, `?v=${V}&auth=${tb}`);
check('the banned host\'s lobby is closed', r.status === 404, r);
check('an unban works', /Unbanned/.test((await admin(`lvl:acct:unban:${ALICE}`)).reply || ''));
await new Promise((res) => setTimeout(res, 1500));
r = await call('POST', '/levels/mine', { auth: ta });
check('after the unban her level is back for review', r.status === 200 && r.data.levels.some((l) => l.id === lid && l.status === 'pending'), r);
check('...and her unit', r.status === 200 && r.data.units.some((u) => u.id === uid1 && u.status === 'pending'), r);
const third = Buffer.from(map);
const at3 = third.indexOf(Buffer.from([0x14, 0x08, 0xff]));
if (at3 > 3) third[at3 - 3] = (third[at3 - 3] + 2) & 0x3f;
r = await up({ auth: ta, name: 'Back Again', map: third.toString('base64') });
check('after the unban she can upload again', r.status === 200, r);

// docs/502 (security review): a slur anywhere a lobby shows text. The text
// comes from LB_SLUR_TEST on the command line; nothing offensive is written here.
if (process.env.LB_SLUR_TEST) {
  const bad = process.env.LB_SLUR_TEST;
  for (const [what, extra] of [['a lobby setting\'s value', { options: { Rules: bad } }],
                               ['a lobby setting\'s name', { options: { [bad]: true } }],
                               ['the player list', { players_list: [{ name: bad }] }],
                               ['the build', { version: '0.4.26 ' + bad }]]) {
    r = await call('POST', '/announce', { auth: tb, version: V, name: 'LEGO Battles', host_name: 'Fine', port: 28750,
                                          max_players: 3, ...extra });
    check(`a slur in ${what} is refused`, r.status === 403, r);
  }
  r = await call('POST', '/remove', { auth: tb, version: V, code: 'ZZZZ', token: 'x', winners: [bad] });
  check('a slur among the winners is refused', r.status === 403, r);
}

// The delete path, for Bob.
const other = Buffer.from(map);   // a different level: move one golden brick
const pickAt = other.indexOf(Buffer.from([0x14, 0x08, 0xff]));
if (pickAt > 3) other[pickAt - 3] = (other[pickAt - 3] + 1) & 0x3f;
r = await up({ auth: tb, name: 'Bob Pond', map: other.toString('base64') });
const bid = r.data && r.data.id;
check('Bob deletes his own', bid && (await call('POST', '/levels/delete', { auth: tb, id: bid })).status === 200, r);

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
