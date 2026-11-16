// GitHub issue #2: a flood of lobbies cannot flood the Discord channel. Against
// `wrangler dev` whose webhook is this test's own receiver, with the default
// limits (no TEST_LIMIT_SCALE):
//   npx wrangler dev --port 8792 --var DISCORD_WEBHOOK:http://127.0.0.1:18797/hook
//   SERVER=http://127.0.0.1:8792 node test/discord_flood.mjs
// Each request names its own CF-Connecting-IP, which `wrangler dev` passes
// through (Cloudflare and the fallback's front door set it themselves), to act
// as many addresses. Local only: never point it at the live server.
import http from 'node:http';

const SERVER = process.env.SERVER || 'http://127.0.0.1:8792';
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail) : '')); }
};

const lines = [];
const hook = http.createServer((req, res) => {
  let b = '';
  req.on('data', (c) => { b += c; });
  req.on('end', () => {
    try { lines.push(JSON.parse(b).content || ''); } catch { lines.push('?'); }
    res.writeHead(204).end();
  });
}).listen(18797);

async function post(path, body, ip) {
  const r = await fetch(SERVER + path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'CF-Connecting-IP': ip },
    body: JSON.stringify(body),
  });
  let data = null;
  try { data = await r.json(); } catch { /* none */ }
  return { status: r.status, data };
}
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

// 60 lobbies from 60 addresses at once: the channel gets its burst, no more.
const made = [];
for (let i = 0; i < 60; ++i) {
  const r = await post('/announce', { name: 'Flood', host_name: 'Flood' + i, port: 29000 + i, max_players: 2 },
                       '203.0.113.' + (i + 1));
  if (r.status === 200) made.push({ ...r.data, ip: '203.0.113.' + (i + 1) });
}
await wait(1500);
check('60 lobbies were listed', made.length === 60, made.length);
check('the channel got at most its burst of 30 lines', lines.length <= 30 && lines.length > 0, lines.length);

// One address joining and leaving over and over: 10 lines at most from it.
await wait(4000);   // two tokens back in the global bucket
const before = lines.length;
for (let i = 0; i < 20; ++i)
  await post(i % 2 ? '/leave' : '/join', { code: made[0].code, name: 'Spammer' }, '198.51.100.7');
await wait(1000);
check('one address makes at most 10 lines, however many joins',
      lines.length - before <= 10, lines.length - before);

for (const l of made) await post('/remove', { code: l.code, token: l.token }, l.ip);
hook.close();
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
