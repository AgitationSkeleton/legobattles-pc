#!/usr/bin/env python3
"""Assemble a self-contained build/ folder: exe, runtime DLLs, config, launcher.

    python tools/lb_deploy.py [--dest build]

Layout produced:

    build/
      LegoBattles.exe       the port
      *.dll                 the four MSYS2 runtime libraries it links against
      game.toml             the vanilla config
      mods/                 extra maps (mod .nds, map folders), not bootable ROMs
      saves/                save files, never your originals
      settings.json         written by the port, beside the executable
      imgui.ini             window layout, likewise
      play.bat              pick a ROM and run

## Only the cartridge boots

mods/ holds extra maps the port reads (a mod's .nds, map folders); those
files are not bootable. An earlier version of this launcher offered them as ROMs,
and booting one produced a black screen that looked exactly like a
broken port.
"""

import argparse
import os
import re
import shutil
import sys

DLLS = ["SDL2.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"]
DLL_DIRS = [r"C:\msys64\ucrt64\bin", r"C:\msys64\mingw64\bin"]

PLAY_BAT = r"""@echo off
setlocal EnableDelayedExpansion
REM ---------------------------------------------------------------------------
REM LEGO Battles - port launcher.
REM
REM Only the cartridge in rom\ is booted. mods\ is a DATA directory the port
REM reads extra maps out of (a mod's .nds, map folders); they are not bootable, and
REM launching one shows a black screen indistinguishable from a broken port.
REM That mistake is exactly why this no longer offers them.
REM
REM Saves live in saves\ (the retail cartridge's is saves\LEGO Battles.sav,
REM whatever the ROM is called). Import another save from the settings
REM panel: Settings > Saves.
REM ---------------------------------------------------------------------------
cd /d "%~dp0"
if not exist "mods"  mkdir "mods"
if not exist "saves" mkdir "saves"

set "ROM="
for %%F in ("rom\*.nds") do if not defined ROM (
  set "ROM=%%~fF"
  set "NAME=%%~nF"
)

if not defined ROM (
  echo No cartridge found.
  echo   Put your LEGO Battles dump in  "%~dp0rom\"
  echo.
  echo   mods\ is for custom map data, not for ROMs to run.
  pause
  exit /b 1
)

set "SAVE=saves\!NAME!.sav"
echo Cartridge  !NAME!
echo Save       !SAVE!
echo.
echo Starting - the game takes a few seconds to reach the title screen.
echo.
REM KEEP THE LOG. Every diagnostic this port has -- the ITCM dispatch probes,
REM the HUD decline counters, the placement footprint -- writes to stderr, and
REM a launcher that drops stderr means a play session produces no evidence at
REM all. A crash or a slow session is then unreportable except from memory.
REM
REM One file per session, named by start time, kept beside the demo recording
REM of the same session so the two can be read together.
REM wmic was tried here and is not resolvable on every Win10 install -- it left
REM STAMP unset and the log name literally "session-~0,14.log". %DATE%/%TIME%
REM are locale-dependent. PowerShell is neither, and this runs once at launch.
set "STAMP="
for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%T"
if not defined STAMP set "STAMP=%RANDOM%"
set "LOGFILE=logs\session-!STAMP!.log"
if not exist "logs" mkdir "logs"
echo Log        !LOGFILE!
echo.
REM MULTIPLAYER NEEDS --local-wireless on. It is the bridge that carries the
REM game's own DS wireless frames: between copies on this PC by itself, and to
REM another PC once the master server hands over an address. Without it the
REM Multiplayer tab has no port to advertise and the game's Join screen can
REM never see a host -- a 2026-09-24 test ran exactly that way. It
REM binds loopback only until a lobby is advertised or joined, and a second
REM copy started from here takes the next free instance index by itself.
LegoBattles.exe dummy 4000000000 --rom "!ROM!" --config "game.toml" ^
  --boot direct --save-path "!SAVE!" --interactive ^
  --generated-firmware --network off --local-wireless on --freebios --rtc-host %* 2>"!LOGFILE!"
if errorlevel 1 (
  echo.
  echo The game exited with an error. The log is at !LOGFILE!
  pause
)
endlocal
"""

README = """LEGO Battles - PC port
======================

Double-click LegoBattles.exe.

The first time, it asks for your LEGO Battles ROM -- your own dump of the
Nintendo DS cartridge (.nds). The Europe and the USA release both work: a
USA ROM is converted to the European image the port is built on. Your file
is only read; the image the port plays is kept in rom\\, so this is asked
once. (play.bat starts it the old way, with a console log.)

Multiplayer: the game's own Multiplayer menu. Hosting advertises your lobby
on the public lobby servers (lbserver.viosarcade.xyz and
king.legobattles.win: "Both", the default, in settings panel >
Network > Multiplayer); the Join screen lists the lobbies of both. The lobby server
introduces the two players so both routers let the game through: no port
forwarding is needed on most home networks. If a joiner's direct connection
carries nothing for five seconds (a VPN, a strict firewall), the two are
connected through lbserver.viosarcade.xyz's relay instead. "Fast online
link" (on by default; every player in a match needs it) keeps an online
match at full speed. The Multiplayer section shows whether anything has
arrived from the other player, and whether the relay is in use.

Balance: the community balance packs Rebalance, FactionWars and Funlite are
built in; pick one in settings panel > Balance > Presets. Single values are
changed under Balance > Units, Heroes and Attacks, and saved to or loaded
from a .json file with Presets' Export and Import.
Online, everyone plays on the host's balance.

  rom\\      your own cartridge dump. This is the only thing that boots.
  mods\\     extra MAPS, added to the map lists at the next start: a
            community mod's .nds (only its multiplayer maps are taken --
            its balance changes are NOT read; see Balance above), or map
            folders from the community archive in mods\\maps\\. A host's
            mod map you lack is fetched into mods\\maps\\_archive. These
            are NOT bootable ROMs -- pointing the emulator at one gives a
            black screen.
  saves\\    your save is saves\\LEGO Battles.sav, whatever the ROM file is
            called. To use a save from a cartridge or an emulator (any
            region), import it: settings panel > Settings > Saves > Browse,
            then Import; the game restarts to load it. A USA save's region
            tag (AFN) is rewritten to the European one (AFE), which is
            lossless, and a padded emulator file is trimmed; the original
            is kept beside it as .orig.

  settings.json  written beside the executable when a setting changes.
  imgui.ini      window layout, likewise.

F1 or Esc opens the settings menu; Ctrl+R restarts the game from its title.

Keys: mouse = the stylus; arrows or W/A/S/D move the camera; mouse wheel
zooms; Space or right-click deselects; Z = A, X = B, C = X, V = Y, Q = L,
E = R, Enter = Start, Backspace = Select (all rebindable). On the name and
cheat-code screens, just type.

The title screen takes a few seconds to appear; a black window at startup is
the game booting, not a hang.

How to
======

Use a mod's balance
  1. Press F1. Open Balance > Presets.
  2. Pick the mod's preset from the list; or press "Import from ROM..." and
     choose the mod's .nds. This replaces the values you changed.
  3. Start a battle. Online, the host's balance is the one everyone plays.

Share your balance
  1. In Balance > Presets, press "Export my balance (.json)..." and save the
     file. It holds the chosen preset and every value you changed.
  2. Send the file to the other player.
  3. They press "Import my balance (.json)..." and choose it.

Add mod maps
  1. Put a mod's .nds in the mods\\ folder, or a map folder from the community
     archive in mods\\maps\\.
  2. Start the game again. The maps are in the map lists.
  3. Only maps are taken. The mod's balance is not; use Import from ROM for that.

Play online
  1. Open the game's Multiplayer menu. One player picks Host; the others pick
     Join and choose the lobby.
  2. In F1 > Network > Multiplayer, leave the server on "Both", so the Join screen
     lists every lobby.
  3. Every player must have the same "Fast online link" setting.
  4. Nothing else is needed. If a direct link fails, the relay starts by itself.

Import a save
  1. Press F1. Open Settings > Saves.
  2. Press Browse and choose your .sav (a cartridge or an emulator, any region).
  3. Press Import. The game restarts and loads it.

Report a bug
  1. Open the Discord bug channel.
  2. Say what you were doing.
  3. Attach the newest file in logs\\ and the newest lb-session-*.lbdemo.
"""


def find_dll(name):
    for d in DLL_DIRS:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--dest", default=os.path.join(here, "build"))
    ap.add_argument("--exe", default=os.path.join(here, "port", "runner", "build",
                                                  "nds_runner.exe"))
    ap.add_argument("--config", default=os.path.join(here, "port", "game.toml"))
    args = ap.parse_args(argv)

    if not os.path.exists(args.exe):
        print("no executable at %s -- build first:\n  bash port/runner/build.sh"
              % args.exe, file=sys.stderr)
        return 2

    for sub in ("", "mods", "saves", "rom", "tools"):
        os.makedirs(os.path.join(args.dest, sub), exist_ok=True)

    shutil.copy2(args.exe, os.path.join(args.dest, "LegoBattles.exe"))
    shutil.copy2(args.config, os.path.join(args.dest, "game.toml"))
    shutil.copy2(os.path.join(here, "tools", "lb_save.py"),
                 os.path.join(args.dest, "tools", "lb_save.py"))
    print("  LegoBattles.exe, game.toml, tools/lb_save.py")

    # docs/394: the USA-to-Europe conversion the first-run setup applies. Made
    # locally from a USA and a Europe dump (tools/lb_region_delta.py) and never
    # committed; without it a USA ROM is refused with a message saying so.
    delta = os.path.join(here, "port", "runner", "build", "data", "usa_to_eur.lbdelta")
    if os.path.exists(delta):
        os.makedirs(os.path.join(args.dest, "data"), exist_ok=True)
        shutil.copy2(delta, os.path.join(args.dest, "data", "usa_to_eur.lbdelta"))
        print("  data/usa_to_eur.lbdelta")
    else:
        print("  NO data/usa_to_eur.lbdelta -- a USA ROM will be refused; run\n"
              "    py -3.13 tools/lb_region_delta.py port/runner/build/data/usa_to_eur.lbdelta",
              file=sys.stderr)

    # THE MASTER SERVER, beside the game it serves.
    #
    # The deployment ships the master server beside the game.
    # Multiplayer needs one running, and the deployment had the client without
    # it -- so testing a lobby meant finding the build in services/ first.
    #
    # Copied, not built. build_exe.sh needs PyInstaller and takes minutes; if
    # dist/ is empty that is the thing to run, and saying so beats a deploy
    # that silently ships nothing.
    ms_dir = os.path.join(here, "services", "master-server", "dist")
    ms_names = ("lb_master_server.exe", "lb_master_server_gui.exe")
    ms_done = []
    for name in ms_names:
        src = os.path.join(ms_dir, name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(args.dest, name))
            ms_done.append(name)
    if ms_done:
        print("  %s" % ", ".join(ms_done))
    else:
        print("  NO master server in services/master-server/dist -- run "
              "services/master-server/build_exe.sh", file=sys.stderr)

    missing = []
    for dll in DLLS:
        src = find_dll(dll)
        if src:
            shutil.copy2(src, os.path.join(args.dest, dll))
        else:
            missing.append(dll)
    print("  %d/%d runtime DLLs" % (len(DLLS) - len(missing), len(DLLS)))
    if missing:
        print("  MISSING: %s -- the exe will not start without these"
              % ", ".join(missing), file=sys.stderr)

    # NEVER SHIP a settings.json -- and NEVER DELETE the player's.
    #
    # The original rule: one written by a development run was copied into the
    # deployment with wide_field enabled, so the player got an unfinished
    # visualisation as their default view. That is prevented by NOT COPYING
    # one, and nothing above copies one.
    #
    # This used to enforce the rule by deleting build/settings.json on every
    # deploy. That cannot tell a stale development file from the player's own
    # -- which the port writes there on every setting change -- so it wiped
    # the player's settings (Enhanced mode, scales, player name, master host,
    # balance options, unlocks) on the 2026-09-22 deploy. Settings belong to
    # the player: leave them alone and say so.
    existing = os.path.join(args.dest, "settings.json")
    if os.path.exists(existing):
        print("  kept the player's settings.json")

    with open(os.path.join(args.dest, "play.bat"), "w", newline="\r\n") as fh:
        fh.write(PLAY_BAT)
    with open(os.path.join(args.dest, "README.txt"), "w", newline="\r\n") as fh:
        fh.write(README)
    print("  play.bat, README.txt")
    print("deployed to %s" % args.dest)
    return 0 if not missing else 1


if __name__ == "__main__":
    raise SystemExit(main())
