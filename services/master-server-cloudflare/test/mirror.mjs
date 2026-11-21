// docs/503: moderation messages in two channels, and the bot's commands,
// against `wrangler dev` with a fake Discord API (started here, port 8797):
//   npx wrangler dev --port 8796 --var REQUIRE_SIGNIN:1 --var MIN_CLIENT_VERSION:0 \
//     --var LEVELS_KEY:testkey --var LEVELS_BOT_TOKEN:x --var DISCORD_API_BASE:http://127.0.0.1:8797 \
//     --var LEVELS_CHANNEL:111 --var UNITS_CHANNEL:222 --var ACCOUNTS_CHANNEL:333 --var MIRROR_CHANNEL:999 \
//     --var "AUTH_SIGNING_KEY:$(cat DEVKEY.json)"
//   KEY=DEVKEY.json node test/mirror.mjs
import http from 'node:http';
import { readFileSync } from 'node:fs';
import crypto from 'node:crypto';

const SERVER = process.env.SERVER || 'http://127.0.0.1:8796';
const MOD = '748343958190555217';
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail).slice(0, 300) : '')); }
};

// The fake Discord: every request recorded; a POST gets a fresh message id.
const seen = [];
let next = 5000;
const fake = http.createServer((req, res) => {
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    seen.push({ method: req.method, url: req.url, body });
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ id: String(++next) }));
  });
});
await new Promise((r) => fake.listen(8797, '127.0.0.1', r));
const posts = (ch) => seen.filter((s) => s.method === 'POST' && s.url === `/channels/${ch}/messages`);
const patches = (ch) => seen.filter((s) => s.method === 'PATCH' && s.url.startsWith(`/channels/${ch}/messages/`));
const settle = () => new Promise((r) => setTimeout(r, 800));

const priv = crypto.createPrivateKey({ key: JSON.parse(readFileSync(process.env.KEY, 'utf8')), format: 'jwk' });
const b64u = (b) => Buffer.from(b).toString('base64url');
function token(sub, name) {
  const iat = Math.floor(Date.now() / 1000);
  const body = 'v1.' + b64u(JSON.stringify({ sub, uid: 'LB-TEST0003', name, un: name.toLowerCase(), aud: 'main', iat, exp: iat + 3600 }));
  return body + '.' + b64u(crypto.sign(null, Buffer.from(body), priv));
}
const call = async (path, body, headers = {}) => {
  const r = await fetch(SERVER + path, { method: 'POST', headers: { 'Content-Type': 'application/json', ...headers },
                                         body: JSON.stringify(body) });
  return [r.status, await r.json().catch(() => null)];
};
const admin = (b) => call('/levels/admin', { user_id: MOD, ...b }, { 'X-Levels-Key': 'testkey' }).then(([, d]) => d);
const cmd = (channel_id, command, args = {}) => admin({ channel_id, command, args });

const map = readFileSync('../../out/eur/plain/Maps/mp01.map');
const tbp = readFileSync('../../out/eur/plain/BP/DetailTiles_mp01.tbp');
const variant = (k) => { const m = Buffer.from(map); const at = m.indexOf(Buffer.from([0x14, 0x08, 0xff])); m[at - 3] = (m[at - 3] + k) & 0x3f; return m; };
const ta = token('666666666666666666', 'Mirror');

// An upload: its message in the maps channel and the mirror.
let [st, d] = await call('/levels/upload', { auth: ta, name: 'Twin Pond', author: '', map: variant(3).toString('base64'), tbp: tbp.toString('base64') });
check('a map upload', st === 200, d);
const lid = d && d.id;
await settle();
check('...posted in the maps channel', posts(111).length === 1, seen.map((s) => s.url));
check('...and in the mirror', posts(999).length === 1);
// A button press: both copies edited.
d = await admin({ custom_id: `lvl:approve:${lid}` });
check('a press answers with the update', !!(d && d.update));
await settle();
check('...and both copies are edited', patches(111).length === 1 && patches(999).length === 1, seen.slice(-3).map((s) => s.method + ' ' + s.url));

// Commands, each in its own channel.
d = await cmd('222', 'approve', { kind: 'map', id: lid });
check('a map command in the units channel is refused', /maps channel/.test(d.reply), d);
d = await cmd('555', 'queue');
check('a command outside the moderation channels is refused', /only in the moderation channels/.test(d.reply), d);
d = await cmd('111', 'unapprove', { id: lid });
check('unapprove in the maps channel', /back in the queue/.test(d.reply), d);
d = await cmd('111', 'queue');
check('the queue lists it', new RegExp(`#${lid} `).test(d.reply), d);
const before = posts(111).length;
d = await cmd('111', 'queue', { repost: true });
await settle();
check('the queue posts it again, in both channels', /posted again: 1/.test(d.reply) && posts(111).length === before + 1, d);
d = await cmd('999', 'approve', { kind: 'map', id: lid });
check('the mirror channel takes map commands', /approved/.test(d.reply), d);
d = await cmd('222', 'autoapprove', { on: true });
check('auto-approve for units, in the units channel', /units is now \*\*on/.test(d.reply), d);
d = await cmd('111', 'autoapprove', { on: false });
check('auto-approve for maps stays separate', /maps is now \*\*off/.test(d.reply), d);
[st, d] = await call('/levels/upload', { auth: ta, name: 'Queued Pond', author: '', map: variant(4).toString('base64'), tbp: tbp.toString('base64') });
check('with maps auto-approve off, a map waits', d && d.status === 'pending', d);

// Accounts: search, ban, unban -- in the accounts channel only.
d = await cmd('111', 'users', { query: 'x' });
check('account commands are refused in the maps channel', /accounts channel/.test(d.reply), d);
d = await cmd('333', 'ban', { who: '777777777777777777', reason: 'test' });
check('a ban by Discord id', /Banned/.test(d.reply), d);
await settle();
d = await cmd('333', 'users', { query: '777777777777777777' });
check('the search finds the banned account', /777777777777777777/.test(d.reply) && /banned/.test(d.reply), d);
d = await cmd('333', 'user', { who: '<@666666666666666666>' });
check('user by mention: their uploads counted', /2 map\(s\)/.test(d.reply), d);
d = await cmd('333', 'purge', { who: '666666666666666666' });
check('purge deletes their uploads', /Deleted 2 map/.test(d.reply), d);
d = await cmd('333', 'unban', { who: '777777777777777777' });
check('unban', /Unbanned/.test(d.reply), d);
await settle();
check('account log lines go to both channels', posts(333).length >= 2 && posts(999).length >= 4,
      { a: posts(333).length, m: posts(999).length });
d = await admin({ channel_id: '333', command: 'ban', args: { who: '1' } });
d = await call('/levels/admin', { user_id: '1', channel_id: '333', command: 'ban', args: { who: '777777777777777777' } },
               { 'X-Levels-Key': 'testkey' });
check('a non-moderator\'s command is refused', /moderator/.test(d[1].reply), d[1]);

fake.close();
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
