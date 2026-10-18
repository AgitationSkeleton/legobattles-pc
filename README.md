# LEGO Battles — PC port

A native Windows port of **LEGO Battles** (Nintendo DS, 2009, Warner Bros. /
Hellbent Games), built by recompiling the game's own ARM code to C. It plays in
a resizable widescreen window at 60 fps, with an enhanced presentation, a
settings panel, mod maps, and online multiplayer through the game's own lobby.

**You need your own copy of the game.** This repository and its releases contain
no ROM, no BIOS and no extracted assets. The first start asks for your dump of
the cartridge (the Europe or the USA release).

## Playing

1. Download the latest zip from [Releases](../../releases) and unzip it anywhere.
2. Double-click `LegoBattles.exe`.
3. The first time, choose your LEGO Battles ROM (`.nds`). A USA ROM is converted
   to the European image the port is built on; your file is only read.

F1 or Esc opens the settings menu (display, enhancements, cheats, Team Builder,
multiplayer); Ctrl+R restarts the game from its title. Settings are saved
beside the executable.

**Keys:** the mouse is the stylus; arrows or W/A/S/D move the camera and the
wheel zooms; Space or right-click deselects; Z/X/C/V are A/B/X/Y, Q/E are L/R,
Enter is Start and Backspace Select (all rebindable). On the profile-name and
cheat-code screens, just type.

**Multiplayer** uses the game's own Multiplayer menu. Hosting advertises your
lobby on the public lobby server (`lbserver.viosarcade.xyz`); the Join screen
lists the lobbies there. The host needs UDP port 26710 reachable from the
internet; players on one network need nothing.

**Reporting a problem:** open an issue on the
[issue tracker](../../issues/new/choose) -- there is a form for bugs and one for
suggestions. Attach the newest file in `logs\` and say what you were doing.
Every session also records its inputs as `lb-session-*.lbdemo`, which replays
the session exactly; attaching it usually makes a bug reproducible.

**This is a beta.** Online multiplayer is lightly tested, and the port has so
far been tested on a single PC. Reports from other hardware and networks are
the most useful thing a tester can send.

## What is in this repository

| path | what |
| --- | --- |
| [`docs/`](docs/) | how the game works, measured from the ROM: formats, the simulation, combat, the renderer, multiplayer, and every change the port makes, with its evidence. Start at [`docs/README.md`](docs/README.md). |
| [`port/`](port/) | the port: the recompiler configuration (`config/`), the recompiler patches (`recomp/`), and the runner (`runner/`: the port's own C++ sources, the hook script that applies them to the upstream runner, the build script) |
| [`src/`](src/) | hand-written reimplementations of game functions (`battles/`) |
| [`tools/`](tools/) | the Python tools: ROM extraction, format decoders, the test harness, deployment and packaging |
| [`services/`](services/) | the lobby server: `master-server-cloudflare` (the public one, a Cloudflare Worker) and `master-server` (the same protocol in Python, for a LAN or a private server) |
| [`config/`](config/) | function and symbol tables |

## Building

A build needs things this repository cannot ship:

- your **European ROM** (`C5SP`, SHA-1 `da60469bd143eb8f47ecf0d7951181b4783c91b4`), which
  every generated file is lifted from;
- **ndsrecomp**, the recompiler and runner framework the port builds on, at
  `$NDSRECOMP` (read-only: every change to the runner lives in `port/runner/`);
- **MSYS2** with the UCRT64 toolchain (`gcc`, `g++`, `cmake`, `make`, SDL2) and
  Python 3.

The pipeline, in short:

```
python tools/nds_extract.py "<your EU ROM>" out/eur      # extract
python tools/lb_ndsrecomp.py ...                          # program configs (port/config)
# lift with ndsrecomp into port/generated (port/recomp/apply_finder_patch.py)
bash port/runner/build.sh                                 # runner -> port/runner/build/nds_runner.exe
python tools/lb_deploy.py --dest build                    # a playable folder
```

`docs/` records each step and its checks; `port/runner/README.md` explains how
the runner fork is assembled.

A release zip is made by `tools/lb_dist.py`. It needs `tools/lb_region_delta.py`
first, which makes the USA-to-Europe conversion data from both dumps. The two
releases differ only in their code. The conversion rebuilds the European code
from the player's own USA code: it unpacks each module, applies bsdiff-style
address adjustments, and repacks it with the cartridge's own compression parse.
So the file carries the differences, not the European program. It is still
made from the dumps, so it is never committed and only goes into releases.

## Releases

`.github/workflows/release.yml` builds and publishes a release. Builds happen on a
**self-hosted Windows runner** (label `legobattles`) that has the ROMs, ndsrecomp,
MSYS2 and the generated code: GitHub's own runners cannot have the ROM. Run it
from the Actions tab with a version (it tags the commit `v<version>`), or push a
`v*` tag. `.github/workflows/ci.yml` runs on GitHub's runners: the lobby
servers' tests and a check of the Python tools.

## Credits and licences

The port stands on **[ndsrecomp](https://github.com/mstan/ndsrecomp)** by
Matthew Stanley, the DS static recompiler and runner it is built with, and on
**[melonDS](https://melonds.kuribo64.net/)**, whose 3D engine, rasterisers and
Wi-Fi model the runner vendors. Both credits matter for licensing:

- `LegoBattles.exe` links melonDS code, so the executable is a combined work
  under the **GNU GPL v3.0 or later**. Every release attaches
  `LegoBattles-PC-<version>-runner-source.zip`, the assembled runner source it
  was compiled from: ndsrecomp's runner with this port's hooks and sources
  applied.
- ndsrecomp as a whole is under the **PolyForm Noncommercial License 1.0.0**,
  with its runner sources under MIT and its melonDS-derived files under GPL.
  This port and its releases are noncommercial.

[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) lists every component
(FreeBIOS, libslirp, xxHash, glad, Dear ImGui, SDL2, the MinGW runtime) with
its licence, and [`LICENSES/`](LICENSES/) holds the full texts. Both also
ship inside every release zip.

## Licence

The port's own code and documentation -- everything in this repository not
listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) -- are licensed
under the **GNU General Public License v3.0 or later** ([`LICENSE`](LICENSE)).
GPL-3.0-or-later is the licence the port has to carry: its code is compiled
into the ndsrecomp runner together with melonDS's GPL-3.0-or-later sources, so
the executable as a whole is a GPL work. The third-party components keep their
own licences.

## Legal

LEGO Battles is © Warner Bros. Interactive Entertainment and the LEGO Group. This
is an unofficial fan project, not affiliated with or endorsed by them. It does
not include the game; you must own it.
