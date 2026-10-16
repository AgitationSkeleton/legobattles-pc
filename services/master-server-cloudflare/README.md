# LEGO Battles master server (Cloudflare)

The public lobby directory the port uses by default:
**http://lbserver.viosarcade.xyz** (plain HTTP on port 80 and HTTPS both work;
the game's client speaks plain HTTP). It is a Cloudflare Worker with one Durable
Object, `Directory`, that holds every lobby.

It is the same service as `services/master-server` (the Python one, which still
works for a LAN or a private server): same endpoints, same field names, same
90-second TTL, same room-code alphabet. What it adds is the lobby's life after it
is advertised, so that it can be logged to Discord.

## What it logs to Discord

With the `DISCORD_WEBHOOK` secret set (`src/discord.js`), one message for each:

- a lobby hosted: the host's name, the room code, the build, and the host's
  simulation settings (balance patch, hotwire targets, transport occupants,
  attack boost, healing fix, same builders, unit caps, cheats, bans);
- a player joining or leaving the lobby (or the match), by name;
- the match starting, with the players and the map;
- the match ending, with how long it lasted;
- the lobby disbanded before a match, or expired (the host stopped answering).

The webhook is a secret, never in this repository: whoever has it can post to the
channel. Without it nothing is sent.

## Endpoints

| method | path | what |
| --- | --- | --- |
| GET | `/health` | `{"ok":true,"service":"legobattles-master","lobbies":N}` |
| GET | `/list` | every OPEN lobby, with its `players_list` |
| GET | `/lobby/<code>` | one open lobby |
| POST | `/announce` | `{name, host_name, port, players, max_players, map_name, mode, version, options}` → `{code, token, ttl}` |
| POST | `/heartbeat` | `{code, token, players, state?, map_name?}`; `state` is `open`, `starting` (unlisted) or `playing` |
| POST | `/remove` | `{code, token, reason?}`; `reason` is `disband` (default) or `end` |
| POST | `/join` | `{code, name}`: a player is in this lobby |
| POST | `/leave` | `{code, name}` |
| GET | `/admin/lobbies` | every lobby, any state, and the bans (`Authorization: Bearer ADMIN_KEY`) |
| POST | `/admin/close`, `/admin/ban`, `/admin/unban` | `{code}` / `{address, reason}` / `{address}` |

The host's address is the one the request came from (`CF-Connecting-IP`). The
client connects over IPv4, so that is the IPv4 address other players send to.

## How the port drives it

`port/runner/src/lb_net.cpp` reads the cartridge's own network state each frame:

- the game opens a Host lobby (state 2) → `/announce`, then `/heartbeat` every 30 s;
- Start (3, 4) → `state: starting`; the match on (11) with its map loaded →
  `state: playing` with `map_name` (the map's own name, from the level loader);
- the lobby closes → `/remove` with `end` after a match, `disband` before one;
- a joiner in the host's lobby (5, 12) → `/join`; leaving → `/leave`.

## Deploying

You need the Cloudflare account with the `viosarcade.xyz` zone and Node.js 20+.

```
npm install
npx wrangler login
npx wrangler deploy                       # the Worker, its Durable Object and the domain
npx wrangler secret put DISCORD_WEBHOOK   # the channel's webhook URL
npx wrangler secret put ADMIN_KEY         # any long random string
```

It is deployed (2026-10-16). Later changes are `npx wrangler deploy` again.

## Testing

```
npm run dev                               # http://127.0.0.1:8787, no Discord
npm test                                  # the protocol, against it
SERVER=http://lbserver.viosarcade.xyz npm test   # against the live one (posts to Discord)
```

From the game itself: `LB_NET_TEST=lbserver.viosarcade.xyz:80 LB_NET_HOST=1` with
`--local-wireless on` announces and lists once at startup; `tools/lb_mp.py` drives
two instances through a whole match against the server in the settings.
