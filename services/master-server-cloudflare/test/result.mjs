// SG-0019: the "match ended" line names the winners and the losers, against `wrangler dev` whose
// webhook is this test's own receiver:
//   npx wrangler dev --port 8788 --var DISCORD_WEBHOOK:http://127.0.0.1:18798/hook
//   SERVER=http://127.0.0.1:8788 node test/result.mjs
// Local only: never point it at the live server.
import http from 'node:http';

const SERVER = process.env.SERVER || 'http://127.0.0.1:8788';
const HOOK_PORT = Number(process.env.HOOK_PORT || 18798);
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail) : '')); }
};

const got = [];
const hook = http.createServer((req, res) => {
  const parts = [];
  req.on('data', (d) => parts.push(d));
  req.on('end', () => { got.push(Buffer.concat(parts).toString('utf8')); res.end('{}'); });
});
await new Promise((r) => hook.listen(HOOK_PORT, '127.0.0.1', r));

async function call(path, body) {
  const r = await fetch(SERVER + path, { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                         body: JSON.stringify(body) });
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  return { status: r.status, data };
}
const settle = () => new Promise((r) => setTimeout(r, 1200));

async function match(extra, label) {
  const a = await call('/announce', { name: 'LEGO Battles', host_name: 'Tester1', port: 28700, players: 2,
                                      max_players: 3, version: 'test' });
  check(label + ': announce', a.status === 200, a);
  const { code, token } = a.data;
  await call('/heartbeat', { code, token, players: 2, state: 'playing', map_name: 'The Pond' });
  await settle();
  got.length = 0;
  const r = await call('/remove', { code, token, reason: 'end', ...extra });
  check(label + ': remove', r.status === 200, r);
  await settle();
  return got.join('\n');
}

let text = await match({ winners: ['Tester2'], losers: ['Tester1'] }, 'one winner');
check('names the winner', text.includes('**Tester2** won'), text);
check('names the loser', text.includes('**Tester1** lost'), text);
check('still says the match ended', text.includes('ended after'), text);

text = await match({ winners: ['Tester1', 'Tester3'], losers: ['Tester2'] }, 'two winners');
check('names both winners', text.includes('**Tester1**, **Tester3** won'), text);

text = await match({}, 'no result');
check('without a result the line is unchanged', text.includes('ended after') && !text.includes(' won') && !text.includes(' lost'), text);

text = await match({ winners: 'not a list', losers: [5, '<@everyone>'] }, 'junk');
check('junk is ignored or cleaned', text.includes('ended after') && !text.includes('@everyone'), text);

hook.close();
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
