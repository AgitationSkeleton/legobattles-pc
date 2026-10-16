# 394 — The public master server, the first-run setup, and a playtest build

Status: **implemented and verified.** New: `services/master-server-cloudflare/`,
`port/runner/src/lb_setup.cpp`, `tools/lb_region_delta.py`, `tools/lb_dist.py`.
Changed: lb_net.cpp, lb_overlay.cpp/.h, lb_patches.cpp, lb_filedlg.cpp,
apply_hooks.py (main.cpp), build.sh, lb_deploy.py. 2026-10-16.

Three pieces of work: the master server moves to Cloudflare, with Discord
webhook logging of lobbies (named players hosting, starting, leaving and
disbanding, and each lobby's settings), and the client's default URL points at
it; first-time startup gets a proper ImGui prompt for the LEGO Battles ROM (US
or EU both accepted) and sets itself up afterwards; and a distributable test
build is produced for players.

## 1. The master server on Cloudflare

`services/master-server-cloudflare` is a Worker with one `Directory` Durable
Object, deployed at **lbserver.viosarcade.xyz**, modelled on the Crystalien
Conflict master server.

* **Same protocol.** It keeps the Python server's endpoints, field names, 90 s
  TTL and Crockford codes, so the client needed no new transport.
* **Plain HTTP still works.** The client is raw sockets over plain HTTP/1.1,
  and Cloudflare serves the custom domain on port 80 with `Content-Length`. A
  chunked reply is now decoded anyway, in case a proxy re-frames one.

The lobby's life is now reported, which the server could not see before. The
host withdrew its lobby with a bare `/remove` the moment Start was pressed, so
"started" and "disbanded" looked the same. Now:

| the cartridge's network state | the client sends |
| --- | --- |
| 2, the host's lobby is open | `/announce` with the host's settings and build, then `/heartbeat` |
| 3/4, Start | `heartbeat state=starting` (unlisted) |
| 11, the match, once its map has loaded | `heartbeat state=playing, map_name` |
| the lobby closes | `/remove reason=end` after a match, `disband` before |
| a joiner at 5/12 | `/join {code, name}`; leaving, `/leave` |

The map's name comes from a new observer on the level loader, `FUN_02035FAC`:
* r0 is the name as a String (char* at +8);
* "Maps/" (0x02144BB0) + name + ".map" (0x02144BB8);
* a stock stem is looked up in the mission table (0x02147DDC: stem, players,
  name id) and its name read from the LOC file;
* a mod map's name is lb_mods'.

The settings are sent by the panel's own labels, so the server prints them
without knowing what a number means.

The Discord messages are hosted, joined, left, started (players and map), ended
(duration), disbanded and expired. They go through the `DISCORD_WEBHOOK`
secret, never the repository.

The default master server is now `lbserver.viosarcade.xyz:80`. A settings file
still holding the old default (127.0.0.1:28555) moves to it; anything typed
stays.

Verified:
* `npm test`, 21 checks, locally and against the live server;
* the game's own self-test against the live server (announce, list);
* `tools/lb_mp.py`, two instances through a whole match: lobby YAXW hosted by
  the game, found by the joiner's Join screen, joined and reported, unlisted
  at Start, "playing" on "The Pond". The server's record held the roster, the
  build and the settings.

## 2. The first-run setup

A double-clicked `LegoBattles.exe` with an empty `rom\` used to print to
stderr and exit, and a double-clicked program has no console, so a new player
saw nothing happen. main.cpp's no-argument start now calls
`lb_first_run_setup` (lb_setup.cpp). It opens a small SDL + ImGui window of its
own that asks for the player's ROM, by file dialog or drag-and-drop.

It runs when rom\ is empty, or holds a ROM whose header is not the European
one (C5SP at 0x0C).

**Europe and USA.** The port is built on the European cartridge: its
recompiled code is registered for that image's SHA-1 alone. The USA cartridge
shares every game file byte for byte. It differs only in the first 0x126000
bytes (header, ARM9, overlay table, docs/04), and the packed ARM9 differs almost
throughout because compression moves everything. So a USA ROM is converted:

* the conversion data is the European bytes of 380 runs,
  `data/usa_to_eur.lbdelta` (894 KB, `tools/lb_region_delta.py`);
* the result must hash to the European SHA-1 before it is kept;
* the player's file is only read, and the image goes to `rom\` under the
  European name; any other .nds there is renamed `.unused`.

**The delta is never committed.** It carries the European ARM9, so it is made
locally from the two cartridge dumps (EU and USA) and shipped only in the build, which already
carries the recompiled code.

The no-argument start also gets `--network off --local-wireless on`, which
play.bat always passed and multiplayer needs.

**The console.** A double-click opened a console window for the whole session.
Now, when this process is the console's only one and stderr goes to the session
log, the console is HIDDEN. FreeConsole was tried first and lost every log line
after the first: 166 bytes against 11,896.

Verified, from clean folders made by lb_deploy:
* **A USA ROM in rom\.** It was converted, hashed to the European SHA-1
  (`da60469b...`) and booted. The log shows the title banks registered, local
  wireless configured and the overlay ready, and the window ran at 59.8 FPS.
* **An empty rom\.** The setup window appeared and waited.
* **A non-ROM .nds.** It was refused with "That is not a LEGO Battles ROM".

## 3. A playtest build

`tools/lb_dist.py` puts lb_deploy's layout into a fresh `dist/` folder and a
zip:
* nothing from the development machine: no settings, saves, logs or recordings;
* no self-hosted master server;
* the delta, and `version.txt`, which the game reports as its build.

## Gate

On the final binary, test mods parked: rep394a == rep394b == rep393a counts;
gate394 counts == gate370b, 8/8 pictures by pixels vs gate381, 59.86 fps;
van394 240/240 pixels vs van-hudmask.
