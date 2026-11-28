// SG-0151: community announcements, against `wrangler dev` with a throwaway
// signing key (never the production one) and one test moderator:
//   node test/signin.mjs --make-key DEVKEY.json
//   npx wrangler dev --port 8797 --var REQUIRE_SIGNIN:1 --var MODERATORS:333333333333333333 \
//       --var "AUTH_SIGNING_KEY:$(cat DEVKEY.json)"
//   SERVER=http://127.0.0.1:8797 KEY=DEVKEY.json node test/news.mjs
// Local only.
import { readFileSync } from 'node:fs';
import crypto from 'node:crypto';

const SERVER = process.env.SERVER || 'http://127.0.0.1:8797';
const priv = crypto.createPrivateKey({ key: JSON.parse(readFileSync(process.env.KEY, 'utf8')), format: 'jwk' });
const V = process.env.V || '0.4.28';
let failed = 0, passed = 0;
const check = (what, ok, detail) => {
  if (ok) { ++passed; console.log('  ok   ' + what); }
  else { ++failed; console.log('  FAIL ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail).slice(0, 300) : '')); }
};
const b64u = (b) => Buffer.from(b).toString('base64url');
function token(sub, name, aud = 'main') {
  const iat = Math.floor(Date.now() / 1000);
  const body = 'v1.' + b64u(JSON.stringify({ sub, uid: 'LB-TEST0000', name, aud, iat, exp: iat + 86400 }));
  return body + '.' + b64u(crypto.sign(null, Buffer.from(body), priv));
}
async function call(method, path, body) {
  const r = await fetch(SERVER + path, {
    method, headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  let data = null;
  try { data = await r.json(); } catch { /* none */ }
  return { status: r.status, data };
}
const MOD = '333333333333333333', USER = '444444444444444444';
const tm = token(MOD, 'Moddy'), tu = token(USER, 'Usery'), tplay = token(MOD, 'Moddy', 'play');

let r = await call('GET', '/news');
check('anyone reads the news, no sign-in or version', r.status === 200 && Array.isArray(r.data.news), r);
const before = r.data.news.length;

r = await call('POST', '/news', { auth: tu, version: V, title: 'Hi', text: 'Not a moderator' });
check('a player who is not a moderator is refused', r.status === 403, r);
r = await call('POST', '/news', { auth: tplay, version: V, title: 'Hi', text: 'Play token' });
check('a play token is refused (account token only)', r.status === 403, r);
r = await call('POST', '/news', { version: V, title: 'Hi', text: 'No token' });
check('no token is refused', r.status === 401 || r.status === 403, r);
r = await call('POST', '/news', { auth: tm, version: V, title: '', text: 'x' });
check('a post needs a title', r.status === 400, r);

r = await call('POST', '/news', { auth: tm, version: V, title: 'Tournament on Saturday', text: 'Sign up in #events.\nBest of three.' });
check('a moderator posts', r.status === 200 && r.data.ok && r.data.id > 0, r);
const id = r.data && r.data.id;
r = await call('GET', '/news');
const top = r.data.news[0];
check('the post is listed first, with its text, time and poster', r.data.news.length === before + 1 && top.id === id &&
      top.title === 'Tournament on Saturday' && top.text.includes('\n') && top.time > 0 && top.by === 'Moddy', top);

r = await call('POST', '/news', { auth: tm, version: V, title: 'x'.repeat(200), text: 'y'.repeat(2000) });
check('long fields are cut', r.status === 200, r);
r = await call('GET', '/news');
check('to 80 and 600 characters', r.data.news[0].title.length === 80 && r.data.news[0].text.length === 600, r.data.news[0]);
const longId = r.data.news[0].id;

r = await call('POST', '/news/remove', { auth: tu, version: V, id });
check('a player cannot take a post down', r.status === 403, r);
r = await call('POST', '/news/remove', { auth: tm, version: V, id });
check('a moderator takes it down', r.status === 200, r);
r = await call('POST', '/news/remove', { auth: tm, version: V, id });
check('twice: 404', r.status === 404, r);
await call('POST', '/news/remove', { auth: tm, version: V, id: longId });
r = await call('GET', '/news');
check('both gone', r.data.news.length === before, r.data.news.length);

for (let i = 0; i < 12; ++i) await call('POST', '/news', { auth: tm, version: V, title: 'n' + i, text: 't' });
r = await call('GET', '/news');
check('at most ten are kept, newest first', r.data.news.length === 10 && r.data.news[0].title === 'n11', r.data.news.map((n) => n.title));
for (const n of r.data.news) await call('POST', '/news/remove', { auth: tm, version: V, id: n.id });

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
