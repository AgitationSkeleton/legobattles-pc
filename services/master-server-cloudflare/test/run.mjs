// The directory's protocol, against a running server:
//   npm run dev            (in another shell; http://127.0.0.1:8787)
//   npm test               (or SERVER=https://lbserver.viosarcade.xyz npm test)
// It hosts, joins, starts, leaves and ends one lobby, and checks every reply
// the port's client (lb_net.cpp) reads. Against the live server it posts a
// handful of messages to the Discord channel, all from "TestHost".

const SERVER = process.env.SERVER || 'http://127.0.0.1:8787';
let failed = 0, passed = 0;

async function call(method, path, body) {
  const r = await fetch(SERVER + path, {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  return { status: r.status, data };
}

function check(what, ok, detail) {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail ? '  ' + JSON.stringify(detail) : '')); }
}

const health = await call('GET', '/health');
check('health', health.status === 200 && health.data.ok && health.data.service === 'legobattles-master', health);

check('announce refuses a bad port', (await call('POST', '/announce', { port: 0, max_players: 3 })).status === 400);
check('announce refuses max_players 0', (await call('POST', '/announce', { port: 5000, max_players: 0 })).status === 400);
check('a bad body is 400', (await call('POST', '/announce')).status === 400);

const opts = { 'Hotwire targets': 'Vanilla', 'Balance patch': 'Off (Vanilla)' };
const a = await call('POST', '/announce', { name: 'LEGO Battles', host_name: 'TestHost', port: 28700,
                                          players: 1, max_players: 3, version: 'test', options: opts });
check('announce', a.status === 200 && /^[0-9A-Z]{4}$/.test(a.data.code) && a.data.token && a.data.ttl === 90, a);
const { code, token } = a.data;

let list = await call('GET', '/list');
let mine = list.data.lobbies.find((l) => l.code === code);
check('listed', !!mine && mine.host_name === 'TestHost' && mine.port === 28700 && mine.max_players === 3 &&
      typeof mine.address === 'string' && mine.options['Hotwire targets'] === 'Vanilla', mine);
check('no token in the list', mine && mine.token === undefined, mine);

check('lobby by code', (await call('GET', '/lobby/' + code.toLowerCase())).data.lobby.code === code);
check('unknown lobby is 404', (await call('GET', '/lobby/ZZZZ')).status === 404);

check('join', (await call('POST', '/join', { code, name: 'TestJoiner' })).status === 200);
mine = (await call('GET', '/list')).data.lobbies.find((l) => l.code === code);
check('the joiner counts and is listed', mine.players === 2 && mine.players_list.some((p) => p.name === 'TestJoiner'), mine);

// docs/400: the introduction. Locally every request comes from one address,
// so the joiner is "behind the host's router" and its LAN pair is used.
check('punch refuses a bad port', (await call('POST', '/punch', { code, port: 0 })).status === 400);
check('punch to an unknown lobby is 404', (await call('POST', '/punch', { code: 'ZZZZ', port: 40000 })).status === 404);
check('punch', (await call('POST', '/punch', { code, port: 40000, lan_address: '192.168.5.7', lan_port: 26711 })).status === 200);
check('poll with a wrong token is 404', (await call('POST', '/poll', { code, token: 'x' })).status === 404);
let poll = await call('POST', '/poll', { code, token });
check('poll hands the host the joiner', poll.status === 200 && poll.data.punch.length === 1, poll);
const peerSeen = mine.address;   // the address this test is seen from
const same = peerSeen === '0.0.0.0' || peerSeen === mine.address;
check('poll gives the LAN pair to a host behind the same router',
      same ? (poll.data.punch[0].address === '192.168.5.7' && poll.data.punch[0].port === 26711)
           : poll.data.punch[0].port === 40000, poll);
check('a public lan_address is dropped',
      (await call('POST', '/punch', { code, port: 40001, lan_address: '8.8.8.8', lan_port: 26711 })).status === 200 &&
      (await call('POST', '/poll', { code, token })).data.punch.every((p) => p.address !== '8.8.8.8'));

check('heartbeat with a wrong token is 404', (await call('POST', '/heartbeat', { code, token: 'x', players: 1 })).status === 404);
check('heartbeat', (await call('POST', '/heartbeat', { code, token, players: 1 })).status === 200);

check('starting', (await call('POST', '/heartbeat', { code, token, players: 1, state: 'starting' })).status === 200);
list = await call('GET', '/list');
check('a starting lobby is unlisted', !list.data.lobbies.some((l) => l.code === code));
check('playing', (await call('POST', '/heartbeat', { code, token, players: 1, state: 'playing', map_name: 'The Pond' })).status === 200);

check('leave', (await call('POST', '/leave', { code, name: 'TestJoiner' })).status === 200);
check('remove with a wrong token is 404', (await call('POST', '/remove', { code, token: 'x' })).status === 404);
check('end', (await call('POST', '/remove', { code, token, reason: 'end' })).status === 200);
check('gone', (await call('POST', '/heartbeat', { code, token, players: 1 })).status === 404);

// A lobby that never plays: hosted and disbanded.
const b = await call('POST', '/announce', { host_name: 'TestHost', port: 28701, max_players: 2 });
check('disband', (await call('POST', '/remove', { code: b.data.code, token: b.data.token })).status === 200);

// docs/400: a host's LAN pair is shown only to players behind its router.
const c = await call('POST', '/announce', { host_name: 'TestHost', port: 28702, max_players: 2,
                                          lan_address: '192.168.5.9', lan_port: 26710 });
const seen = (await call('GET', '/list')).data.lobbies.find((l) => l.code === c.data.code);
check('a viewer behind the host\'s router sees its LAN pair',
      seen && seen.address === '192.168.5.9' && seen.port === 26710, seen);
check('disband the LAN lobby', (await call('POST', '/remove', { code: c.data.code, token: c.data.token })).status === 200);

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
