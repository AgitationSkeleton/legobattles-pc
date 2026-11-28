// Posts one announcement to a local `wrangler dev` (test aid for the game's
// news reader): SERVER=http://127.0.0.1:8797 KEY=DEVKEY.json node test/news_post.mjs
import { readFileSync } from 'node:fs';
import crypto from 'node:crypto';
const SERVER = process.env.SERVER || 'http://127.0.0.1:8797';
const priv = crypto.createPrivateKey({ key: JSON.parse(readFileSync(process.env.KEY, 'utf8')), format: 'jwk' });
const b64u = (b) => Buffer.from(b).toString('base64url');
const iat = Math.floor(Date.now() / 1000);
const body = 'v1.' + b64u(JSON.stringify({ sub: '333333333333333333', uid: 'LB-TEST0000', name: 'Moddy', aud: 'main', iat, exp: iat + 3600 }));
const tok = body + '.' + b64u(crypto.sign(null, Buffer.from(body), priv));
const r = await fetch(SERVER + '/news', { method: 'POST', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ auth: tok, version: process.env.V || '0.4.28', title: 'Test tournament', text: 'Saturday 18:00 UTC.\nBest of three.' }) });
console.log(r.status, await r.text());
