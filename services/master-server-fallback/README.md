# LEGO Battles master server: the fallback

This is the Cloudflare Worker's own code (`worker/`, copied from `services/master-server-cloudflare/src`) run by Miniflare, the Workers runtime that `wrangler dev` uses, as an ordinary Node process under pm2. It serves the same endpoints the Worker does:

- the lobby list and lobbies;
- the UDP introduction (`/punch`, `/poll`);
- the relay (WebSockets on `/relay/<code>`);
- the score tables;
- the Discord lines.

Its storage (the Directory, relay rooms and scores, as SQLite) lives in `data/`.

Game clients switch to it on their own when the Worker cannot be reached or answers with a Cloudflare limit error (`port/runner/src/lb_net.cpp`, docs/442). The Worker names this server in its `/health` and `/list` replies (`FALLBACK_MASTER`), so the address can change without a new game build.

## One-time setup on the server PC

1. **Node.js 20 or later** and **pm2** (`npm install -g pm2`). This folder already holds its `node_modules`, so no `npm install` is needed here.
2. **Settings:** copy `config.example.json` to `config.json`, if `deploy.ps1` has not already, and fill in:
   - `DISCORD_WEBHOOK` and `DISCORD_WEBHOOK_2`: this server's own webhooks (the Worker's, or separate ones for a channel of its own). Lines from this server are posted as "LEGO Battles server (fallback)".
   - `ADMIN_KEY`: any long random string (the Worker's admin endpoint).
   - `port` stays 8787 unless you change the router forward too.
3. **Windows Firewall:** allow inbound TCP 8787 **by port, not by program**: `netsh advfirewall firewall add rule name="LEGO Battles fallback" dir=in action=allow protocol=TCP localport=8787`. If Windows asks whether to allow `node.exe` or `workerd.exe` on first start, say no: only port 8787 needs to be reachable, and the runtime's own ports stay on 127.0.0.1.
4. **Router:** forward TCP 8787 to the server PC's port 8787.
5. **DNS** (Cloudflare dashboard, zone viosarcade.xyz): an **A** record `lbfallback` pointing to your public IP, **DNS only (grey cloud)**. A proxied (orange) record would not carry port 8787 and would put the traffic back through Cloudflare.
6. **Start it:** run `start.bat`, or in this folder `pm2 start ecosystem.config.cjs` then `pm2 save`. To start pm2 itself with Windows, use `pm2-installer` or a scheduled task that runs `pm2 resurrect` at logon.

## Checking it

- On the server PC: `curl http://127.0.0.1:8787/health` gives `{"ok":true,"service":"legobattles-master","relay":true,"lobbies":0}`.
- From outside your network (a phone off Wi-Fi): `http://lbfallback.viosarcade.xyz:8787/health`.
- Logs: `pm2 logs legobattles-master-fallback`, or `logs/out.log` and `logs/error.log`.

## Updating it

On the development PC, from `services/master-server-fallback`: `powershell -ExecutionPolicy Bypass -File deploy.ps1 -Target <the server folder>` (or set `LB_FALLBACK_TARGET`). It copies the Worker's current code and a fresh `node_modules`, and leaves `config.json`, `data/` and `logs/` alone. Then, on the server PC: `pm2 restart legobattles-master-fallback`.

Deploy it whenever the Worker is deployed, so the two stay the same.

## What differs from the Worker

- **Its own lobbies and scores.** A lobby lives on the server its host announced it to. Scores recorded here stay here.
- **The front door.** `server.mjs` listens on port 8787 itself and passes requests to Miniflare, which listens on 127.0.0.1 only (with its internal loopback port). On the way it:
  - sets `CF-Connecting-IP` from the connection, as Cloudflare does, so a client cannot name another address for itself;
  - refuses bodies over 64 KiB, and connections that send no headers within 15 s.
- **No Cloudflare in front.** There is no DDoS protection, and the DNS record shows your home IP. Only port 8787 is open, and the server holds nothing but lobby listings and scores, but a flood would load your connection.
- **No HTTPS.** The game's client speaks plain HTTP here (it tries HTTPS first only on port 80/443).
