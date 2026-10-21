// docs/428's /match_image, against `wrangler dev` whose webhook is this test's own receiver:
//   npx wrangler dev --port 8787 --var DISCORD_WEBHOOK:http://127.0.0.1:18799/hook
//   node test/match_image.mjs
// Announces a lobby, posts a tiny PNG, and checks the webhook got a multipart post carrying it; then that a
// second picture for the same match and a bad token are refused. Local only: never point it at the live server.
import http from 'node:http';
import zlib from 'node:zlib';

const SERVER = process.env.SERVER || 'http://127.0.0.1:8787';
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail) : '')); }
};

// A 2x2 RGBA PNG.
function crc32(buf) {
  let c, crc = 0xFFFFFFFF;
  for (const b of buf) {
    c = (crc ^ b) & 0xFF;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(2, 0); ihdr.writeUInt32BE(2, 4); ihdr[8] = 8; ihdr[9] = 6;
const raw = Buffer.from([0, 30, 60, 200, 255, 30, 60, 200, 255, 0, 10, 20, 120, 255, 10, 20, 120, 255]);
const png = Buffer.concat([Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]), chunk('IHDR', ihdr),
                           chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);

const got = [];
const hook = http.createServer((req, res) => {
  const parts = [];
  req.on('data', (d) => parts.push(d));
  req.on('end', () => { got.push({ type: req.headers['content-type'] || '', body: Buffer.concat(parts) }); res.end('{}'); });
});
await new Promise((r) => hook.listen(18799, '127.0.0.1', r));

async function call(path, body) {
  const r = await fetch(SERVER + path, { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                         body: JSON.stringify(body) });
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  return { status: r.status, data };
}

const a = await call('/announce', { name: 'LEGO Battles', host_name: 'TestHost', port: 28700, players: 1,
                                    max_players: 3, version: 'test' });
check('announce', a.status === 200, a);
const { code, token } = a.data;
await new Promise((r) => setTimeout(r, 500));
got.length = 0;   // the announce's own message

const bad = await call('/match_image', { code, token: 'nope', png: png.toString('base64') });
check('a wrong token is refused', bad.status === 404, bad);
const notpng = await call('/match_image', { code, token, png: Buffer.from('hello world, not a png at all, '.repeat(4)).toString('base64') });
check('a non-PNG is refused', notpng.status === 400, notpng);
const ok = await call('/match_image', { code, token, png: png.toString('base64') });
check('the picture is accepted', ok.status === 200, ok);
await new Promise((r) => setTimeout(r, 1500));
const post = got.find((g) => g.type.startsWith('multipart/form-data'));
check('the webhook got a multipart post', !!post, got.map((g) => g.type));
if (post) {
  check('it carries match.png', post.body.includes(Buffer.from('filename="match.png"')));
  check('with the PNG bytes', post.body.includes(png));
  check('and an embed of attachment://match.png', post.body.includes(Buffer.from('attachment://match.png')));
}
const again = await call('/match_image', { code, token, png: png.toString('base64') });
check('a second picture for the same match is refused', again.status === 409, again);

await call('/remove', { code, token });
hook.close();
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
