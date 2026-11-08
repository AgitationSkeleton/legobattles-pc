// The showcase relay (src/levels.js): an approved level is posted, without
// buttons, to SHOWCASE_CHANNEL, and its post is deleted when it is unpublished.
// Local only, against a FAKE Discord this script serves itself:
//   node test/showcase.mjs                       (starts the fake on 8812)
//   npx wrangler dev --port 8811 --var LEVELS_KEY:testkey \
//       --var LEVELS_ADMIN_USER:100000000000000001 --var LEVELS_BOT_TOKEN:fake \
//       --var SHOWCASE_CHANNEL:777 --var DISCORD_API_BASE:http://127.0.0.1:8812/api
// LEVELS_CHANNEL is left unset, so the moderation post is skipped and nothing
// is ever sent to discord.com.
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';

const SERVER = process.env.SERVER || 'http://127.0.0.1:8811';
const MAP = process.env.MAP || '../../out/eur/plain/Maps/mp01.map';
const TBP = process.env.TBP || '../../out/eur/plain/BP/DetailTiles_mp01.tbp';
const ADMIN = '100000000000000001';
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail).slice(0, 300) : '')); }
};

const seen = [];
let next = 9000;
const fake = createServer((req, res) => {
  let body = '';
  req.on('data', (c) => { body += c.toString('latin1'); });
  req.on('end', () => {
    seen.push({ method: req.method, url: req.url, body });
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify({ id: String(++next) }));
  });
}).listen(8812, '127.0.0.1');

async function call(path, body, headers = {}) {
  const r = await fetch(SERVER + path, { method: 'POST', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body) });
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  return { status: r.status, data };
}
const admin = (custom_id) => call('/levels/admin', { user_id: ADMIN, custom_id }, { 'X-Levels-Key': 'testkey' });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitFor(pred, ms = 15000) {
  const t = Date.now();
  while (Date.now() - t < ms) { if (pred()) return true; await sleep(250); }
  return false;
}

const map = readFileSync(MAP), tbp = readFileSync(TBP);
const up = await call('/levels/upload', { key: 'd'.repeat(32), name: 'Showcase Pond', author: 'Tester',
  map: Buffer.from(map).toString('base64'), tbp: Buffer.from(tbp).toString('base64') });
check('the level is uploaded as pending', up.status === 200 && up.data.status === 'pending', up);
await sleep(3000);
check('nothing is relayed while it is pending', !seen.some((s) => s.url.includes('/channels/777/')), seen.map((s) => s.url));

await admin(`lvl:approve:${up.data.id}`);
await fetch(SERVER + '/levels/list');   // any request arms the relay
const posted = await waitFor(() => seen.some((s) => s.method === 'POST' && s.url === '/api/channels/777/messages'));
const post = seen.find((s) => s.method === 'POST' && s.url === '/api/channels/777/messages');
check('the approved level is posted to the showcase channel', posted);
const payload = post ? JSON.parse(/payload_json"\r\n\r\n([^\r]*)\r\n/.exec(post.body)[1]) : {};
check('the post has no buttons', post && Array.isArray(payload.components) && payload.components.length === 0, payload.components);
check('the post names the level and has no uploader key', post && payload.embeds[0].title === 'Showcase Pond' &&
      !JSON.stringify(payload.embeds[0]).includes('Uploader'), payload.embeds && payload.embeds[0]);
check('the post carries the preview picture', post && post.body.includes('filename="preview.png"') && post.body.includes('\x89PNG'));
const postId = String(next);

await admin(`lvl:reject:${up.data.id}`);
await fetch(SERVER + '/levels/list');
const del = await waitFor(() => seen.some((s) => s.method === 'DELETE' && s.url === `/api/channels/777/messages/${postId}`));
check('unpublishing deletes the showcase post', del, seen.map((s) => s.method + ' ' + s.url));
const posts = seen.filter((s) => s.method === 'POST').length;
await sleep(4000);
check('nothing is posted twice', seen.filter((s) => s.method === 'POST').length === posts);
check('nothing went anywhere but the showcase channel', seen.every((s) => s.url.startsWith('/api/channels/777/')));

fake.close();
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
