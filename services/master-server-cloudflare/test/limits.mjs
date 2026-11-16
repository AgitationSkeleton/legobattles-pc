// GitHub issue #2: the directory's limits, against a fresh server with the
// default limits (no TEST_LIMIT_SCALE):
//   npx wrangler dev --port 8790      (in another shell)
//   SERVER=http://127.0.0.1:8790 node test/limits.mjs
// Never against the live server: it fills this address's lobby quota and
// rate windows on purpose.

const SERVER = process.env.SERVER || 'http://127.0.0.1:8790';
let failed = 0, passed = 0;

async function call(method, path, body) {
  const r = await fetch(SERVER + path, {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  return { status: r.status, data, headers: r.headers };
}

function check(what, ok, detail) {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail ? '  ' + JSON.stringify(detail) : '')); }
}

// Finding 3: a host cannot list itself at an arbitrary address.
const spoof = await call('POST', '/announce', { name: 'Spoof', host_name: 'TestHost', port: 28700, max_players: 2,
                                               address: 'ATTACKER-CONTROLLED-STRING-1234567890' });
check('announce', spoof.status === 200, spoof);
const seen = await call('GET', '/lobby/' + spoof.data.code);
check('a named address that is not the one seen is not listed',
      seen.data && seen.data.lobby && seen.data.lobby.address !== 'ATTACKER-CONTROLLED-STRING-1234567890',
      seen.data && seen.data.lobby && seen.data.lobby.address);
check('the announcing address is never in a view', seen.data && !('peer' in seen.data.lobby));

// Finding 2: lobbies per address.
const codes = [spoof.status];
const made = [spoof.data];
for (let i = 0; i < 3; ++i) {
  const r = await call('POST', '/announce', { name: 'Flood ' + i, host_name: 'TestHost', port: 28701 + i, max_players: 2 });
  codes.push(r.status);
  if (r.status === 200) made.push(r.data);
}
check('one address lists at most 3 lobbies', JSON.stringify(codes) === '[200,200,200,429]', codes);
const list = await call('GET', '/list');
check('/list says how many lobbies there are', list.status === 200 && list.data.total >= 3, list.data && list.data.total);
for (const l of made) await call('POST', '/remove', { code: l.code, token: l.token });

// The announce rate: 6 every 10 minutes from one address (4 used above).
const more = [];
for (let i = 0; i < 4; ++i) {
  const r = await call('POST', '/announce', { name: 'More ' + i, host_name: 'TestHost', port: 28710 + i, max_players: 2 });
  more.push(r.status);
  if (r.status === 200) await call('POST', '/remove', { code: r.data.code, token: r.data.token });
}
const last = await call('POST', '/announce', { name: 'Last', host_name: 'TestHost', port: 28720, max_players: 2 });
check('the announce rate is limited, with Retry-After',
      JSON.stringify(more) === '[200,200,429,429]' && last.status === 429 && last.headers.get('Retry-After') === '600',
      { more, last: last.status, retry: last.headers.get('Retry-After') });

// Finding 4: HEAD.
const head = await fetch(SERVER + '/list', { method: 'HEAD' });
const text = await head.text();
check('HEAD answers with headers only', head.status === 200 && text === '', { status: head.status, len: text.length });

// The GET rate: 120 a minute from one address.
let got429 = false;
for (let i = 0; i < 130 && !got429; ++i) got429 = (await call('GET', '/health')).status === 429;
check('GET is rate limited per address', got429);

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
