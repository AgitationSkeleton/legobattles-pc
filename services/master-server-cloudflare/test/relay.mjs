// The relay (docs/424), against a running server:
//   npm run dev                 (in another shell; http://127.0.0.1:8787)
//   node test/relay.mjs         (or SERVER=http://lbserver.viosarcade.xyz ...)
// Announces a lobby, puts a host and two joiners on its room, and checks
// that batches cross both ways, that 255 reaches every joiner, that a wrong
// token and an unknown lobby are refused, that a host that reconnects keeps
// its joiners, and that the host's leaving closes them after the grace period. The lobby is removed at the end (one Discord line against the
// live server).

import net from 'node:net';

const SERVER = process.env.SERVER || 'http://127.0.0.1:8787';
const WS = SERVER.replace(/^http/, 'ws');
let failed = 0, passed = 0;

function check(what, ok, detail) {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail) : '')); }
}

async function call(method, path, body) {
  const r = await fetch(SERVER + path, {
    method, headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  return { status: r.status, data };
}

// A socket whose messages queue up, so a test can wait for the next one.
function open(path) {
  return new Promise((resolve) => {
    const ws = new WebSocket(WS + path);
    ws.binaryType = 'arraybuffer';
    const box = { ws, queue: [], waiters: [], closed: null };
    ws.onmessage = (e) => {
      const m = typeof e.data === 'string' ? e.data : new Uint8Array(e.data);
      const w = box.waiters.shift();
      if (w) w(m); else box.queue.push(m);
    };
    ws.onclose = (e) => {
      box.closed = { code: e.code, reason: e.reason };
      for (const w of box.waiters.splice(0)) w(null);
    };
    ws.onopen = () => resolve(box);
    ws.onerror = () => resolve(box);
  });
}
function next(box, ms = 3000) {
  if (box.queue.length) return Promise.resolve(box.queue.shift());
  if (box.closed) return Promise.resolve(null);
  return new Promise((resolve) => {
    const w = (m) => { clearTimeout(t); resolve(m); };
    const t = setTimeout(() => {
      box.waiters.splice(box.waiters.indexOf(w), 1);
      resolve(undefined);
    }, ms);
    box.waiters.push(w);
  });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// The status line of a raw upgrade request: the game's own client reads the
// refusal from it (fetch will not send an Upgrade header).
function upgradeStatus(path) {
  const u = new URL(SERVER + path);
  return new Promise((resolve) => {
    const sock = net.connect(Number(u.port || 80), u.hostname, () => {
      const CRLF = String.fromCharCode(13, 10);
      sock.write(['GET ' + u.pathname + u.search + ' HTTP/1.1', 'Host: ' + u.host,
                  'Upgrade: websocket', 'Connection: Upgrade',
                  'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==', 'Sec-WebSocket-Version: 13',
                  '', ''].join(CRLF));
    });
    let got = '';
    sock.on('data', (d) => {
      got += d;
      const end = got.indexOf(String.fromCharCode(13));
      if (end >= 0) { sock.destroy(); resolve(got.slice(0, end)); }
    });
    sock.on('error', () => resolve(''));
  });
}
function batch(...datagrams) {
  const parts = [];
  for (const d of datagrams) parts.push(d.length & 255, d.length >> 8, ...d);
  return new Uint8Array(parts);
}
const same = (a, b) => a && b && a.length === b.length && a.every((v, i) => v === b[i]);

const health = await call('GET', '/health');
check('health says the relay is there', health.status === 200 && health.data.relay === true, health.data);

const a = await call('POST', '/announce', { name: 'LEGO Battles', host_name: 'TestHost', port: 28700,
                                          players: 1, max_players: 3, version: 'test' });
check('announce', a.status === 200, a);
const { code, token } = a.data;

const plain = await fetch(SERVER + '/relay/' + code + '?role=join');
check('a plain GET is 426', plain.status === 426, plain.status);

const bad = await open('/relay/' + code + '?role=host&token=nope');
check('a wrong token is refused', bad.ws.readyState !== WebSocket.OPEN);
const none = await open('/relay/ZZZZ?role=join');
check('an unknown lobby is refused', none.ws.readyState !== WebSocket.OPEN);

const early = await upgradeStatus('/relay/' + code + '?role=join');
check('a joiner before the host is refused (404)', / 404 /.test(early), early);

const host = await open('/relay/' + code + '?role=host&token=' + token);
check('the host connects', host.ws.readyState === WebSocket.OPEN);
await sleep(200);
const j1 = await open('/relay/' + code + '?role=join');
check('joiner 1 hears ready', (await next(j1)) === 'ready');
const hello1 = await next(host);
check('the host hears joiner 1 open', same(hello1, new Uint8Array([1, 1])), hello1);
const j2 = await open('/relay/' + code + '?role=join');
check('joiner 2 hears ready', (await next(j2)) === 'ready');
check('the host hears joiner 2 open', same(await next(host), new Uint8Array([1, 2])));

const up = batch([1, 2, 3], [4, 5]);
j1.ws.send(up);
const got = await next(host);
check('a joiner batch reaches the host, tagged', got && got[0] === 0 && got[1] === 1 && same(got.subarray(2), up), got);

const down = batch(new Array(1100).fill(7));
host.ws.send(new Uint8Array([0, 255, ...down]));
const d1 = await next(j1), d2 = await next(j2);
check('255 reaches joiner 1', same(d1, down), d1 && d1.length);
check('255 reaches joiner 2', same(d2, down), d2 && d2.length);
host.ws.send(new Uint8Array([0, 2, ...batch([9])]));
check('a tagged batch reaches only its joiner', same(await next(j2), batch([9])) &&
      (await next(j1, 500)) === undefined);

j1.ws.send('ping');
check('ping is answered', (await next(j1)) === 'pong');

j2.ws.close();
check('the host hears joiner 2 leave', same(await next(host), new Uint8Array([2, 2])));

// BR-0031/32: a host that drops and comes back keeps its joiners, and hears
// them as open again.
host.ws.close();
await sleep(1000);
check('a dropped host does not close the joiners at once', !j1.closed, j1.closed);
const host2 = await open('/relay/' + code + '?role=host&token=' + token);
check('the host reconnects', host2.ws.readyState === WebSocket.OPEN);
check('the new host hears joiner 1 open', same(await next(host2), new Uint8Array([1, 1])));
j1.ws.send(up);
const again = await next(host2);
check('a joiner batch reaches the reconnected host', again && again[1] === 1 && same(again.subarray(2), up), again);
host2.ws.send(new Uint8Array([0, 255, ...batch([5])]));
check('the reconnected host reaches the joiner', same(await next(j1), batch([5])));

host2.ws.close();
const t0 = Date.now();
await next(j1, 20000);
check('the host leaving closes the joiners after the grace period',
      j1.closed && j1.closed.code === 4004 && Date.now() - t0 >= 10000, { closed: j1.closed, ms: Date.now() - t0 });

await call('POST', '/remove', { code, token });
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
