// The LEGO Battles master server's FALLBACK: the Cloudflare Worker's own code
// (services/master-server-cloudflare/src, copied into ./worker by deploy.ps1),
// run by Miniflare -- the Workers runtime (workerd) that `wrangler dev` uses --
// as an ordinary process, so it behaves as the Worker does: the same
// endpoints, the Directory, relay rooms and score tables as Durable Objects,
// their SQLite storage kept on this machine's disk (./data).
//
// Clients turn to it when the Worker cannot be reached or answers with a
// Cloudflare limit error (port/runner/src/lb_net.cpp, docs/442). Lobbies,
// relay rooms and scores here are this server's own: a lobby lives on
// whichever server its host announced to.
//
// Run it with pm2 (ecosystem.config.cjs) or `node server.mjs`. Settings come
// from config.json beside this file (config.example.json shows them).

import { Miniflare, Log, LogLevel, convertV4MiniflareOptions } from 'miniflare';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const cfgPath = path.join(here, 'config.json');
const cfg = fs.existsSync(cfgPath) ? JSON.parse(fs.readFileSync(cfgPath, 'utf8')) : {};

const port = Number(process.env.LB_FALLBACK_PORT || cfg.port || 8787);
const host = process.env.LB_FALLBACK_HOST || cfg.host || '0.0.0.0';
const dataDir = path.resolve(here, process.env.LB_FALLBACK_DATA || cfg.data_dir || 'data');
fs.mkdirSync(dataDir, { recursive: true });

// The Worker's vars and secrets (wrangler.jsonc "vars", `wrangler secret put`).
// FALLBACK_MASTER is left unset here: this server has no fallback of its own.
const bindings = { MAX_PLAYERS: String(cfg.max_players || 8) };
for (const key of ['DISCORD_WEBHOOK', 'DISCORD_WEBHOOK_2', 'ADMIN_KEY', 'SCORE_MIN_SECONDS']) {
  if (cfg[key]) bindings[key] = String(cfg[key]);
}
// Any other var, as `wrangler dev --var` gives one (the tests' SCORE_MIN_SECONDS
// and SWEAR_TEST_WORDS): config.json "vars", or LB_FALLBACK_VARS=K=V,K=V.
for (const [k, v] of Object.entries(cfg.vars || {})) bindings[k] = String(v);
for (const kv of (process.env.LB_FALLBACK_VARS || '').split(',').filter(Boolean)) {
  const i = kv.indexOf('=');
  if (i > 0) bindings[kv.slice(0, i)] = kv.slice(i + 1);
}
const dataOverride = process.env.LB_FALLBACK_DATA;
// Discord lines from this server say so, so the channel can tell them apart.
bindings.SERVER_LABEL = String(cfg.server_label || 'fallback');

function workerModules() {
  const dir = path.join(here, 'worker');
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.js'));
  files.sort((a, b) => (a === 'index.js' ? -1 : b === 'index.js' ? 1 : a.localeCompare(b)));
  return files.map((f) => ({ type: 'ESModule', path: path.join(dir, f) }));
}

// Miniflare 5 takes its own config shape; the v4 options (the README's, one
// worker at the top level) go through its convertV4MiniflareOptions. Durable
// Object storage goes under resourcePersistencePath (unset, a temporary
// folder that is lost on every restart).
const mf = new Miniflare(convertV4MiniflareOptions({
  host: '127.0.0.1',   // the front door below is the public side
  port: 0,             // any free port
  resourcePersistencePath: dataDir,
  logRequests: !!cfg.debug,
  log: new Log(cfg.debug ? LogLevel.DEBUG : LogLevel.INFO),
  name: 'legobattles-master-fallback',
  // Every file of the Worker, the entry first (named relative to ./worker, so
  // its `import ... from './discord.js'` resolve as they do in the Worker).
  modules: workerModules(),
  modulesRoot: path.join(here, 'worker'),
  compatibilityDate: '2026-09-01',
  bindings,
  durableObjects: {
    DIRECTORY: { className: 'Directory', useSQLite: true },
    RELAY: { className: 'RelayRoom', useSQLite: true },
    SCORES: { className: 'Scores', useSQLite: true },
  },
}));

const url = await mf.ready;
console.log(`[lb-fallback] the Worker runs at ${url} (this machine only), data in ${dataDir}`);

// THE FRONT DOOR. Miniflare itself listens on 127.0.0.1 only: given 0.0.0.0 it
// also opens its internal loopback port (workerd's calls back into Node,
// guarded by a secret) to every interface. Public traffic comes in here and
// is passed through, with two changes:
//   * CF-Connecting-IP is set from the socket, never taken from the request,
//     as Cloudflare does (the Worker reads the asker's address from it);
//   * bodies over 64 KiB are refused (the Worker's largest request, a match
//     picture post, is far below that), and so are connections that send
//     nothing for 15 s.
// WebSocket upgrades (the relay) are spliced through as raw sockets.
const inner = new URL(url);
const MAX_BODY = 64 * 1024;
function clientIp(sock) {
  const a = sock.remoteAddress || '';
  return a.startsWith('::ffff:') ? a.slice(7) : a;
}
function forwardHeaders(req) {
  const h = { ...req.headers };
  for (const k of Object.keys(h)) {
    if (k.toLowerCase() === 'cf-connecting-ip' || k.toLowerCase() === 'x-forwarded-for') delete h[k];
  }
  h['cf-connecting-ip'] = clientIp(req.socket);
  return h;
}
const front = http.createServer((req, res) => {
  const len = Number(req.headers['content-length'] || 0);
  if (len > MAX_BODY) { res.writeHead(413).end(); req.destroy(); return; }
  let seen = 0;
  const up = http.request({ host: inner.hostname, port: inner.port, method: req.method,
                            path: req.url, headers: forwardHeaders(req) }, (r) => {
    res.writeHead(r.statusCode || 502, r.headers);
    r.pipe(res);
  });
  up.on('error', () => { if (!res.headersSent) res.writeHead(502); res.end(); });
  req.on('data', (c) => {
    seen += c.length;
    if (seen > MAX_BODY) { up.destroy(); res.writeHead(413).end(); req.destroy(); }
  });
  req.pipe(up);
});
front.on('upgrade', (req, sock, head) => {
  const up = net.connect(Number(inner.port), inner.hostname, () => {
    const h = forwardHeaders(req);
    let raw = `${req.method} ${req.url} HTTP/1.1\r\n`;
    for (const [k, v] of Object.entries(h)) {
      for (const one of Array.isArray(v) ? v : [v]) raw += `${k}: ${one}\r\n`;
    }
    up.write(raw + '\r\n');
    if (head && head.length) up.write(head);
    sock.pipe(up);
    up.pipe(sock);
  });
  const close = () => { sock.destroy(); up.destroy(); };
  up.on('error', close);
  sock.on('error', close);
  sock.on('close', close);
  up.on('close', close);
});
front.headersTimeout = 15000;
front.requestTimeout = 30000;
front.listen(port, host, () =>
  console.log(`[lb-fallback] serving on ${host}:${port}`));

async function shutdown(sig) {
  console.log(`[lb-fallback] ${sig}: stopping`);
  try { await mf.dispose(); } catch { /* already gone */ }
  process.exit(0);
}
process.on('SIGINT', () => shutdown('SIGINT'));
process.on('SIGTERM', () => shutdown('SIGTERM'));
