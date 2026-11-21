// docs/501 (security review): the sign-in that comes back to the game's own
// machine (RFC 8252), the account/play token split, and signing out -- against
// `wrangler dev` with a fake Discord (started here, port 8797):
//   npx wrangler dev --port 8796 --var REQUIRE_SIGNIN:1 --var MIN_CLIENT_VERSION:0 \
//     --var DISCORD_CLIENT_ID:x --var DISCORD_CLIENT_SECRET:y --var DISCORD_OAUTH_BASE:http://127.0.0.1:8797 \
//     --var AUTH_BASE:http://127.0.0.1:8796 --var AUTH_ALLOW_HTTP:1 --var "AUTH_SIGNING_KEY:$(cat DEVKEY.json)"
//   node test/signin_flow.mjs
// With GAME=<path to the runner exe> (and ROM, CONFIG), part B also signs the
// real game in: this script plays the browser against the game's listener.
import http from 'node:http';
import { spawn } from 'node:child_process';
import { existsSync, readFileSync, unlinkSync } from 'node:fs';
import path from 'node:path';

const SERVER = 'http://127.0.0.1:8796';
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail).slice(0, 300) : '')); }
};
const claims = (t) => { try { return JSON.parse(Buffer.from(String(t).split('.')[1], 'base64url').toString()); } catch { return {}; } };

// The fake Discord: authorize sends the browser straight back with a code
// naming the user; token and users/@me answer for that code.
const fake = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    if (u.pathname === '/oauth2/authorize') {
      const back = new URL(u.searchParams.get('redirect_uri'));
      back.searchParams.set('code', 'user-' + (globalThis.NEXT_USER || '880000000000000001'));
      back.searchParams.set('state', u.searchParams.get('state'));
      res.writeHead(302, { Location: back.toString() });
      return res.end();
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    if (u.pathname === '/api/v10/oauth2/token') {
      const code = new URLSearchParams(body).get('code') || '';
      return res.end(JSON.stringify({ access_token: 'at-' + code.slice(5) }));
    }
    if (u.pathname === '/api/v10/users/@me') {
      const id = String(req.headers.authorization || '').replace('Bearer at-', '');
      return res.end(JSON.stringify({ id, username: 'tester' + id.slice(-2), global_name: 'Flow Tester' }));
    }
    res.end(JSON.stringify({ id: '1' }));
  });
});
await new Promise((r) => fake.listen(8797, '127.0.0.1', r));

// One request, no redirects followed: [status, Location, body].
async function hop(url, opts = {}) {
  const r = await fetch(url, { redirect: 'manual', ...opts });
  return [r.status, r.headers.get('location') || '', await r.text()];
}
const post = (p, b) => fetch(SERVER + p, { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                            body: JSON.stringify(b) }).then(async (r) => [r.status, await r.json().catch(() => null)]);
const hex = (n) => [...crypto.getRandomValues(new Uint8Array(n))].map((x) => x.toString(16).padStart(2, '0')).join('');

// The browser's part: start -> Discord -> callback; the last Location.
async function browse(startUrl) {
  let [st, loc] = await hop(startUrl);
  if (st !== 302) return { st };
  [st, loc] = await hop(loc);                      // the fake authorize
  if (st !== 302) return { st };
  [st, loc] = await hop(loc);                      // the callback
  return { st, loc };
}

// ---------------------------------------------------------------- part A --
const state = hex(16);
const port = 40000 + Math.floor(Math.random() * 20000);
let r = await browse(`${SERVER}/auth/start?state=${state}&port=${port}`);
const back = r.loc ? new URL(r.loc) : null;
check('the callback sends the browser back to this machine, with a ticket',
      r.st === 302 && back && back.host === `127.0.0.1:${port}` && back.pathname === '/lb-signin' &&
      back.searchParams.get('state') === state && /^[0-9a-f]{32}$/.test(back.searchParams.get('ticket') || ''), r);
const ticket = back ? back.searchParams.get('ticket') : '';
let [s1, , d1] = await hop(`${SERVER}/auth/poll?state=${state}`);
check('a poll without the ticket is refused', s1 === 400, s1);
[s1, , d1] = await hop(`${SERVER}/auth/poll?state=${state}&ticket=${hex(16)}`);
check('a poll with a guessed ticket gets nothing', s1 === 200 && JSON.parse(d1).pending === true && !JSON.parse(d1).token, d1);
[s1, , d1] = await hop(`${SERVER}/auth/poll?state=${state}&ticket=${ticket}`);
const acct = JSON.parse(d1 || '{}').token || '';
check('with the ticket: the account token (aud main)', claims(acct).aud === 'main' && claims(acct).sub === '880000000000000001', d1.slice(0, 80));
[s1] = await hop(`${SERVER}/auth/poll?state=${state}&ticket=${ticket}`);
check('the ticket works once', s1 === 404, s1);
check('a start without a port is refused', (await hop(`${SERVER}/auth/start?state=${hex(16)}`))[0] === 400);

let [s2, d2] = await post('/auth/play', { auth: acct });
const play = d2 && d2.token;
check('a play token for the account token', s2 === 200 && claims(play).aud === 'play' && claims(play).exp - claims(play).iat <= 7200, d2);
check('no play token for a play token', (await post('/auth/play', { auth: play }))[0] === 401);
check('the lobby list takes the play token', (await fetch(`${SERVER}/list?v=1.0.0&auth=${play}`)).status === 200);
check('publishing does not take the play token', (await post('/levels/mine', { auth: play }))[0] === 401);
check('... and does take the account token', (await post('/levels/mine', { auth: acct }))[0] === 200);
check('hiding scores does not take the play token', (await post('/scores/hide', { auth: play, version: '1.0.0' }))[0] === 401);
[s2, d2] = await post('/auth/verify', { token: play });
check('King\'s check: the play token, with its audience', d2.ok && d2.aud === 'play', d2);

// The attack from the review: a sign-in link made by someone else.
const evil = hex(16);
await hop(`${SERVER}/auth/start?state=${evil}&port=50001`);      // the attacker registers a state
globalThis.NEXT_USER = '880000000000000002';                     // the victim signs in with it
r = await browse(`${SERVER}/auth/start?state=${evil}&port=50001`);
check('the victim\'s browser goes to the victim\'s own machine', r.loc && new URL(r.loc).host === '127.0.0.1:50001', r);
[s1, , d1] = await hop(`${SERVER}/auth/poll?state=${evil}&ticket=${hex(16)}`);
check('the attacker, polling with the state alone, gets no token', !JSON.parse(d1 || '{}').token, d1);

// Signing out voids both tokens.
check('sign out', (await post('/auth/signout', { auth: acct }))[0] === 200);
await new Promise((res) => setTimeout(res, 300));
check('after signing out the account token is refused', (await post('/levels/mine', { auth: acct }))[0] === 401);
[s2, d2] = await post('/auth/verify', { token: play });
check('... and the play token too', d2.ok === false, d2);

// ---------------------------------------------------------------- part B --
if (process.env.GAME) {
  globalThis.NEXT_USER = '880000000000000003';
  const dir = path.dirname(process.env.GAME);
  const authFile = path.join(dir, 'auth.json');
  if (existsSync(authFile)) unlinkSync(authFile);
  const env = { ...process.env, LB_SIGNIN_TEST: '1', LB_AUTH_TEST_HOST: '127.0.0.1:8796', LB_NOTOP: '1',
                LB_WINDOW: '640x480', LB_MASTER: '127.0.0.1:8796', LB_MASTER_FALLBACK: '0',
                LB_NET_TEST: '127.0.0.1:8796', LB_NET_HOST: '1' };
  const game = spawn(process.env.GAME, ['dummy', '4000000000', '--rom', process.env.ROM, '--config', process.env.CONFIG,
    '--boot', 'direct', '--save-path', process.env.SAVE, '--interactive', '--generated-firmware', '--network', 'off',
    '--freebios', '--local-wireless', 'on'], { env, windowsHide: true });
  let log = '';
  game.stderr.on('data', (c) => { log += c; });
  const until = async (re, ms) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { const m = re.exec(log); if (m) return m; await new Promise((x) => setTimeout(x, 250)); } return null; };
  const m = await until(/\[lb_auth\] test: sign in at (\S+)/, 30000);
  check('the game starts a sign-in on its loopback port', !!m, log.slice(-300));
  if (m) {
    r = await browse(m[1]);
    let landed = 0;
    if (r.loc) landed = (await hop(r.loc))[0];            // the browser's last stop: the game itself
    check('the game\'s listener answers the browser', landed === 200, r);
    check('the game signs in', !!(await until(/\[lb_auth\] signed in/, 15000)));
    check('auth.json holds an account token', existsSync(authFile) &&
          claims(JSON.parse(readFileSync(authFile, 'utf8')).token).aud === 'main');
    check('with it, the game hosts on the official server (play token)', !!(await until(/hosting as room [0-9A-Z]{4}/, 30000)),
          log.split('\n').filter((l) => /lb_net|lb_auth/.test(l)).slice(-6));
  }
  game.kill();
  if (existsSync(authFile)) unlinkSync(authFile);
}

fake.close();
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
