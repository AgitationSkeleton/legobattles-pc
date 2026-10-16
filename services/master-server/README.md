# LEGO Battles master server

> **The public server is now the Cloudflare one** (docs/394):
> `services/master-server-cloudflare`, at `lbserver.viosarcade.xyz`, which the
> port uses by default and which logs lobbies to Discord. This Python server is
> the same protocol for a LAN or a private server: point the settings panel's
> Multiplayer > Master server at it (127.0.0.1, 28555). It ignores the newer
> lobby-state fields (`state`, `reason`, `/join`, `/leave`).

Two flavours of the same service.

```
python master_server.py                 # cmd: shows logs and keeps them
python master_server_gui.py             # GUI: lobby sidepanel + console
python test_master_server.py            # 18 tests against the live service
```

Standard library only — no pip install, no build step. SQLite for storage so the
directory survives a restart.

## What it is

**A directory, not a relay.** Hosts advertise a lobby, clients ask for the list
and get addresses back. No game traffic passes through it, so it cannot make an
unreachable host reachable — that is what UPnP on the host is for.

Modelled deliberately on the LSW2 port's self-hosted master server
(`services/master-server-selfhost` in that project):
same TTL model, same endpoint shape, same Crockford room-code alphabet. A client
written against one is a small edit from the other, and both can run on one host
(this defaults to **28555**, two above LSW2's 28553).

## Endpoints

| method | path | purpose |
| --- | --- | --- |
| `GET` | `/health` | liveness and lobby count |
| `GET` | `/list` | every live lobby, each with its player list |
| `GET` | `/lobby/<code>` | one lobby by room code |
| `POST` | `/announce` | host advertises; returns `code` + `token` |
| `POST` | `/heartbeat` | refresh TTL, update players |
| `POST` | `/remove` | host closes its lobby |

Advertisements expire after **90 seconds** without a heartbeat. Room codes are
**4 characters** of Crockford Base32 minus I, L, O and U — readable aloud and off
a screenshot.

## On the player cap

**The server does not assume one.** LEGO Battles' own multiplayer maximum has not
been established from the ROM: `func_02086E5C` iterates `world->players_8[i]` for
`i < world->count_C`, so the count is **runtime state, not a compile-time
constant**. `SkirmishSetup` and `LobbySettings` both use 2 and 3 as immediates,
which is consistent with a maximum of 3, but that is not proof, and values
are read from the ROM, never guessed.

So the **host states `max_players`** in its advertisement and the server enforces
only an operator ceiling (`--max-players`, default 8) to stop a malicious host
advertising nonsense. When the real cap is read out of the ROM, tighten the
default; nothing else changes.

## Administration

The GUI's lobby tree expands to show connected players. Right-click a row to
close the lobby, ban its address, or copy the address. Bans persist in the
database and are enforced on every POST.

Logs go to `logs/master.log` by default in both flavours; the cmd flavour also
echoes to the console, and `--quiet` turns that off while keeping the file.

## Executables

The master server also ships as an **executable** in two
forms. `./build_exe.sh` runs the tests and produces both with PyInstaller:

```
dist/lb_master_server.exe        9.7 MB   console: shows logs and keeps them
dist/lb_master_server_gui.exe   12.8 MB   windowed: lobby sidepanel
```

`dist/` and `build/` are gitignored — they are build products.

Verified against the built console binary, not just the source:

```
$ ./dist/lb_master_server.exe --port 28601 --db m.sqlite3 --log logs/master.log
2026-08-29 20:15:05  START     LEGO Battles master server on 0.0.0.0:28601
2026-08-29 20:15:05  ANNOUNCE  BQHS  Agit's Game hosted by Player at 127.0.0.1:28700  (1/3)

POST /announce -> 200 {"ok": true, "code": "BQHS", "token": "a794585a...", "ttl": 90}
GET  /list     -> 200 {"ok": true, "lobbies": [{"code": "BQHS", ...}]}
```

## The `/announce` body — exact field names

**Unknown keys are ignored silently and the default is used**, so a client that
guesses a name half-works: the lobby appears but its map or host is blank. The
accepted keys are:

| key | type | default | notes |
| --- | --- | --- | --- |
| `port` | int | — | **required**, 1..65535; anything else is `400 port out of range` |
| `name` | str | `"LEGO Battles"` | lobby name |
| `host_name` | str | `"Player"` | **not** `host` |
| `map_name` | str | `""` | **not** `map` |
| `address` | str | the peer's IP | override only if behind a known NAT |
| `players` | int | 1 | |
| `max_players` | int | — | clamped to the operator ceiling (`--max-players`) |
| `mode` | str | `""` | |
| `version` | str | `""` | truncated to 32 chars |
| `options` | obj | `{}` | the lobby-carried rule set |
| `players_list` | list | `[]` | drives the GUI sidepanel's expansion |

`options` is where the host's simulation-affecting choices travel. Per
`docs/86`, anything that changes the simulation is
lobby-carried rather than client-local, so joining clients conform to the host.
