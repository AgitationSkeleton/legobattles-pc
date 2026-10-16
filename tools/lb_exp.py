#!/usr/bin/env python3
"""Run one controlled experiment against a VISIBLE window and record everything.

    python tools/lb_exp.py <label> [--width N] [--env K=V ...]

Launches the port with a real window (never SDL_VIDEODRIVER=dummy, which skips
rendering and therefore cannot show lag, banding, or a misplaced HUD), drives it
to Free Play gameplay, then records for that one run:

    stderr.log     the renderer's own branch diagnostics, [lb_wideA]/[lb_wide2d]
    fps.txt        frontend_stats over a real interval, taken IN gameplay
    <size>.png     the composited window at several sizes, via PrintWindow

Everything lands in screenshots/exp-<label>/ so two runs can be diffed. The
point is that a claim about striping or frame rate is only worth making when
the artefact that supports it is on disk next to the configuration that
produced it.
"""

import argparse
import binascii
import json
import os
import shutil
import re
import subprocess
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from dbg import Dbg, assert_single_runner  # noqa: E402

# The European ROM: LB_ROM_EU, else roms/ two levels above this checkout.
ROM = os.environ.get("LB_ROM_EU") or os.path.join(
    ROOT, "..", "..", "roms", "LEGO Battles (Europe) (En,Fr,De,Es,It,Da).nds")
# LB_EXE runs another binary through the same harness -- an A/B against the
# deployed build/LegoBattles.exe, for instance (docs/349).
EXE = os.environ.get("LB_EXE") or os.path.join(ROOT, "port", "runner", "build",
                                                "nds_runner.exe")

# Set once the output directory exists; render_band() reads the port's own
# [lb_band] from it.
LOGPATH = None
_BAND_WARNED = False


def render_band(gw):
    """Where the guest's 256 columns are DRAWN in a gw-wide surface.

    Every probe that aims at a target's native x used (gw - 256) // 2, the
    centred split. That was the same value the port's touch mapping subtracted,
    so the two cancelled and the probes passed whatever the renderer did -- the
    circularity docs/274 could not get out from under. The port now maps a
    click through lb_band_left(), which the map edges clamp, so the centred
    split is no longer the inverse of anything and a probe using it aims 252 px
    off (measured: band 132, centred split 384).

    Falls back to the centred split when the port has not said, which is right
    for a surface no wider than the screen and is what lb_band_left() itself
    returns in that case.
    """
    centred = (gw - 256) // 2
    band = None
    if LOGPATH:
        try:
            with open(LOGPATH, "rb") as fh:
                tail = fh.read()[-40000:].decode("latin-1", "replace")
        except OSError:
            tail = ""
        import re as _re
        for ln in reversed(tail.splitlines()):
            if not ln.startswith("[lb_band]"):
                continue
            m = _re.match(r"\[lb_band\] (\d+)x\d+\s+left=(-?\d+)", ln)
            # ONLY A LINE FOR THIS SURFACE. The run resizes the window, and a
            # band belonging to a 1024-wide surface says nothing about a
            # 1712-wide one.
            if m and int(m.group(1)) == gw:
                band = int(m.group(2))
                break
    if band is not None:
        return band
    # SAY SO. A SILENT FALLBACK IS HOW THIS KEEPS HAPPENING.
    #
    # The first run after this helper was written reported "band is 384 px each
    # side" -- the centred split -- because the probe that called it never
    # armed LB_BANDLOG, so there were no [lb_band] lines to read. It aimed 252
    # px off and printed four confident selection counts. port_camx() lost a
    # whole experiment to the identical fault and its docstring says so, which
    # is why this one refuses to be quiet about it.
    global _BAND_WARNED
    if not _BAND_WARNED:
        _BAND_WARNED = True
        print("  render_band: NO [lb_band] for a %d-wide surface -- falling "
              "back to the centred split %d. Aim taken from this is only "
              "right away from the map edges; arm LB_BANDLOG." % (gw, centred))
    return centred


def render_band_top(gh):
    """The same question vertically, and it is not the same answer.

    The vertical split is asymmetric too, and more visibly so: a 772x550
    surface reports `top=144 bot=214` where the centred split would be 179.
    Aiming a click with the centred value puts it 35 rows from where it was
    meant to go, which is most of a unit.
    """
    centred = (gh - 192) // 2
    if not LOGPATH:
        return centred
    try:
        with open(LOGPATH, "rb") as fh:
            tail = fh.read()[-40000:].decode("latin-1", "replace")
    except OSError:
        return centred
    import re as _re
    for ln in reversed(tail.splitlines()):
        if not ln.startswith("[lb_band]"):
            continue
        m = _re.match(r"\[lb_band\] \d+x(\d+)\s+.*?\btop=(-?\d+)", ln)
        if m and int(m.group(1)) == gh:
            return int(m.group(2))
    return centred

# NOT 19842. That port is the shared default across every ndsrecomp title,
# and a LEGO Star Wars II runner left over from another project
# was holding it -- so this harness connected to LSW2, drove LSW2, and reported
# "route did not reach gameplay" about a game it was never launching. A
# measurement tool that can silently attach to the wrong process is worse than
# no measurement, so this run gets a private port and proves the server it
# talks to is the child it started.
PORT = 19857

# 2560x1440 AND 3440x1440 ARE IN HERE ON PURPOSE.
#
# This list stopped at 1920x1080, but a target case is a fullscreen 3440x1440
# window that should show a wide view of the whole game field. So every defect
# that only appears past 1920 was invisible to every run this harness had made,
# which is how the adaptive path was repeatedly reported working while it was
# broken at that size.
#
# 2560x1440 and 3440x1440 also straddle the surface clamp: at pixel_scale 2 they
# ask for 1280x720 and 1720x720 guest pixels against a ceiling of 1024x512, so
# they are the first two sizes where the clamp actually bites and the picture
# starts magnifying again instead of showing more field.
SIZES = ["640x480", "800x600", "1024x768", "1280x720", "1600x900", "1920x1080",
         "2560x1440", "3440x1440"]

# Reaching Free Play. Coordinates are guest touch coordinates on the 256x192
# bottom screen; the route is the one verified by capture.
# Seeded into every run so the route never has to CREATE a profile. Creating
# one means driving the name keyboard, which is D-pad and A rather than touch,
# and four runs were spent discovering that and then failing to get a name
# committed anyway. A save that already has a profile removes the whole problem
# -- and this one is a real cartridge save, so the runs also get a
# profile with content rather than a blank one.
SEED_SAVE = os.path.join(ROOT, "saves", "lego_battles.sav")

# EVERY COORDINATE BELOW WAS DRIVEN BY HAND AND PHOTOGRAPHED FIRST.
#
# Six route runs were spent changing one guessed coordinate per boot. The
# coordinates here were instead established interactively against a single live
# instance with tools/lb_drive.py, capturing after every press, which settled
# the whole route against one instance.
#
# Two things that route cost six runs to learn:
#
#   * these menus need TWO taps -- the first selects, the second confirms. The
#     profile screen needs the slot ACTIVELY selected before "Use" does
#     anything, which is why tapping (128, 60), four pixels above the "Max"
#     bar, left the screen looking selected and the button inert.
#   * touch DOES work here. That was in doubt after the name keyboard ignored
#     it entirely; tapping the "Empty" slot and watching the highlight move
#     proved the mapping was right and the coordinates were not.
ROUTE = [
    (128, 96, 3.0),    # title: touch to start
    (127, 75, 2.5),    # profile: select "Max" (the seeded save's profile)
    (205, 169, 3.5),   # profile: Use
    (89, 52, 3.0),     # main menu: Single Player
    (171, 91, 2.5),    # single player: select Free Play
    (171, 91, 4.0),    # single player: enter Free Play
    (205, 169, 4.0),   # map select ("The Pond"): Continue
    (205, 169, 4.5),   # rules: Continue
    (205, 169, 5.0),   # army select: Continue
    (205, 169, 8.0),   # opponent: Continue -- drops into the battle
]

# Reaching STORY mode instead of Free Play.
#
# WHY: Free Play cannot answer two of the rendering checks. Modelled special
# units only exist in the campaign, and the shroud-removal cheat is only worth
# spending on a map that has something to reveal, so special unit rendering is
# tested on story mode missions in which specials exist, with the shroud
# removal cheat.
#
# The Single Player screen has two buttons and the route already tapped the
# right-hand one. screenshots/exp-cull2/route/03 and 04 show the highlight
# sitting on the LEFT button before the Free Play tap and on the RIGHT one
# after it, so the left button is Story and Free Play is the right. Measured
# back through the 448-wide surface (window x = 8 + (guest x + 96) * 1.99) the
# left button spans guest x 60..118, so its centre is x 89 -- which is also,
# conveniently, the same x as "Single Player" one screen earlier.
# Reaching MULTIPLAYER. The main menu's buttons sit where the Single Player
# screen's do -- the route taps the left one at (89, 52) for Single Player, so
# the right one is the same y. Everything past that is walked with --tap and
# photographed before it is written down here.
# Photographed: the main menu is five icons -- King (Single Player) top left,
# a pair of pirates (Multiplayer) top right, then a gift, a trophy and a set of
# gears along the bottom. Two taps, select then confirm, like every other menu
# here (screenshots/mp-smoke/i0-final.png shows the first tap landing with
# "Multiplayer" captioned and highlighted).
MP_ROUTE = ROUTE[:3] + [
    (171, 52, 3.0),    # main menu: select Multiplayer
    (171, 52, 4.0),    # main menu: enter Multiplayer
]

STORY_ROUTE = ROUTE[:4] + [
    (89, 91, 2.5),     # single player: select Story
    (89, 91, 4.0),     # single player: enter Story
    # "Select a Story": six campaign icons, three across. Measured off
    # screenshots/exp-story/route/04 the top row's centres come out at guest x
    # 52, 130, 206 -- i.e. the even 52/128/204 you would lay out across 256 --
    # and the row's centre is guest y 50. Knights is the top left one and is
    # the one already highlighted on entry.
    #
    # A and START were tried here first and do nothing: this screen is
    # touch-only, which is why the generic advance sat on it for fourteen
    # attempts. Two taps again, select then confirm.
    (52, 50, 2.5),     # select a story: select Knights
    (52, 50, 4.0),     # select a story: enter Knights
    # "Level Select" has Act 1 / Act 2 / Act 3 tabs across the top and a map of
    # mission nodes below. ACT 3 ON PURPOSE: Act 1 mission 1 is the campaign's
    # TUTORIAL, and a tutorial mission holds the camera still until its
    # multi-page box is finished -- which is why exp-story3 and exp-story4 both
    # reported camx = 256 for every pan step and looked like panning was
    # broken. A later mission also has the fuller roster the "modelled special
    # unit" rendering check needs. Tab centres measured off
    # screenshots/exp-story2/route/07: guest (210, 42) for Act 3.
    (210, 42, 2.0),    # level select: Act 3
    (210, 42, 2.5),    # level select: Act 3 (confirm, if it needs two)
]


# What comes after "enter Story" is not photographed yet, so it is not
# hard-coded here. --advance N drives it generically: campaign select, chapter
# select, briefing and cutscene are all either the same bottom-right Continue
# button the Free Play route already uses, or an A press. Every attempt is
# captured and in_gameplay() is checked after each, so one boot produces the
# evidence to write a fixed route with -- rather than six boots each changing
# one guessed coordinate, which is what the Free Play route cost.
def port_camx(logpath):
    """The camera the PORT says it is using, from its own log.

    0x02152034 is a hardcoded view pointer and the view object MOVES between
    runs -- lb_field_camera() locates it by scanning for the scroll callback's
    record rather than trusting an address, and the note beside --nudge-cam
    records that the hardcoded read once reported camx -7889, tile -987 on a
    192-tile map, while the port said cam_tx=17 in the same frames.

    The pan path kept using the hardcoded pointer, which is why it read
    00000000 on four consecutive runs and garbage on others. The port prints
    the camera it actually uses on every [lb_band] line under LB_BANDLOG, so
    read that instead of guessing at an address.

    Returns None when the log has no camera yet.
    """
    try:
        with open(logpath, "rb") as fh:
            fh.seek(0, 2)
            back = min(fh.tell(), 300000)
            fh.seek(-back, 2)
            tail = fh.read().decode("latin-1", "replace")
    except Exception:                                      # noqa: BLE001
        return None
    # "[lb_band] ... cam=136,144 ..." and the older "cam=(136, 144)".
    m = re.findall(r"cam=\(?(-?\d+),\s*(-?\d+)\)?", tail)
    return int(m[-1][0]) if m else None


def struct_ptr(d, addr):
    """Read a pointer out of main RAM, or 0 if it does not point into it.

    The view and map globals are pointers to heap objects that only exist
    during a battle, so every reader has to range-check them; doing it in one
    place stops a menu-time read of 0x00000000 from turning into a read_mem at
    address 0x5E that the debug server answers with something meaningless.
    """
    # RETRY, because one failed read is not proof the battle is not running.
    #
    # Two consecutive 28-minute runs reported "view pointer at 02152034 was not
    # readable, so no camera was sampled" -- and the same runs show 89 fog scans
    # and a live camera at tile 17,18 in the port's own log, so the BATTLE was
    # fine and this read was not. The debug channel is shared with the port's
    # own samplers and gets busier the more per-frame logging is on; a single
    # timeout here used to disable the pan for the whole run.
    for attempt in range(4):
        try:
            v = int.from_bytes(bytes.fromhex(
                d.cmd("read_mem", cpu=9, addr=addr, len=4)["hex"]), "little")
        except Exception:                                  # noqa: BLE001
            time.sleep(0.15)
            continue
        if 0x02000000 <= v < 0x02400000:
            return v
        # A genuinely out-of-range value means "not in a battle", which is a
        # real answer rather than a failure -- do not burn retries on it. SAY
        # WHICH, though: "not readable" conflated a failed read with a read
        # that succeeded and returned something out of range, and three runs
        # were spent not knowing which of those was happening while the port's
        # own log showed the battle plainly running.
        print("  struct_ptr(%08X) read %08X, outside main RAM" % (addr, v),
              flush=True)
        return 0
    print("  struct_ptr(%08X) could not be read in 4 attempts" % addr,
          flush=True)
    return 0


def hero_pos(logpath):
    """The PLAYER'S hero, as (screen x, screen y), or None.

    Both coordinates, and owner=0. The aim loop only ever read sx, and its
    caller then tapped a hard-coded (128, 100) hoping the hero was near it --
    which is why every "select a hero" run came back with the in-field HUD
    absent and only the always-present quads in the polygon list. The port
    prints sy and owner too; there is no reason to guess at either, and a unit
    the player does not own has no HUD at all.
    """
    try:
        with open(logpath, "rb") as fh:
            tail = fh.read()[-20000:].decode("latin-1", "replace")
    except OSError:
        return None
    for ln in reversed([l for l in tail.splitlines()
                        if l.startswith("[lb_hero]")]):
        if "owner=0" not in ln:
            continue
        try:
            return (int(ln.split("sx=")[1].split()[0]),
                    int(ln.split("sy=")[1].split()[0]))
        except (IndexError, ValueError):
            return None
    return None


def aim_hero(d, logpath, stepdir, capture_fn, tries=14, lo=None, hi=None):
    """Pan until a Hero stands inside a widened column, then photograph it.

    The check is "modelled special unit rendering". Panning a fixed
    number of steps and hoping cannot answer that -- exp-ent panned eight times
    and the only Hero on the map stayed 381 px off the left edge the whole way.
    This closes the loop instead: the port prints `[lb_hero] sx=...` once a
    second (LB_ENTLOG=1), so the harness can read where the hero actually is and
    press the key that moves it where it needs to be.

    sx is the hero's x MINUS the camera's, so the hero moves left on screen as
    the camera moves right. Target the left widened column, guest x -88..-8:
    far enough in to be unambiguous, and not so far that the sprite is half cut
    off by the surface edge.
    """
    # Default targets the LEFT widened column, which is what docs/210
    # needed. Callers that want the hero somewhere touchable pass their
    # own band -- a tap outside the native 256 is still dropped.
    LO = -88 if lo is None else lo
    HI = -8 if hi is None else hi

    def hero_sx():
        p = hero_pos(logpath)
        return None if p is None else p[0]

    for n in range(tries):
        sx = hero_sx()
        if sx is None:
            print("aim: no hero reported yet")
            time.sleep(1.2)
            continue
        if LO <= sx <= HI:
            capture_fn(os.path.join(stepdir, "hero-in-ext.png"))
            pos = hero_pos(logpath)
            print("aim: HERO IN BAND at sx=%d sy=%s"
                  % (sx, pos[1] if pos else "?"))
            return sx
        # sx too high means the hero is right of where it should be, so the
        # camera has to move right, which is KEYINPUT bit 4. Active low.
        key = 0x10 if sx > HI else 0x20
        # Scale the hold to the distance left to cover. A 0.12 s hold moved the
        # camera about 32 px in exp-story8, so aim for the remaining gap and
        # keep the last approach short enough not to overshoot the 80-px band.
        gap = abs(sx - (LO + HI) // 2)
        hold = max(0.05, min(0.40, gap / 32.0 * 0.12))
        d.cmd("keys", mask=0x3FF & ~key)
        time.sleep(hold)
        d.cmd("keys", mask=0x3FF)
        time.sleep(1.3)          # let the port emit a fresh [lb_hero]
        capture_fn(os.path.join(stepdir, "aim-%02d.png" % n))
        print("aim: sx=%d -> held %s %.2fs" % (sx, "RIGHT" if key == 0x10 else "LEFT", hold))
    print("aim: gave up; last sx=%s" % hero_sx())
    return None


def advance_to_gameplay(d, stepdir, tries, capture_fn):
    for n in range(tries):
        if in_gameplay(d):
            print("advance: in gameplay after %d attempts" % n)
            return True
        before = screen_sig(d)
        tap(d, 205, 169, 1.0)          # the Continue button, if there is one
        if not in_gameplay(d) and screen_sig(d) == before:
            # Continue did nothing, so this screen has no Continue. Try the
            # centre, which is where a chapter node or a "tap to continue"
            # prompt sits.
            tap(d, 128, 96, 1.0)
        if not in_gameplay(d):
            # START/A/B every attempt, not only when the screen is static.
            # The first version gated these on "nothing changed", which meant
            # they were never pressed during the mission intro CUTSCENE -- a
            # cutscene changes every frame, so it looked like progress and the
            # run simply expired inside it. A cutscene is exactly the thing
            # these buttons exist to skip.
            for mask in (0x008, 0x001, 0x002):        # START, A, B
                d.cmd("keys", mask=0x3FF & ~mask)
                time.sleep(0.12)
                d.cmd("keys", mask=0x3FF)
                time.sleep(0.5)
        time.sleep(1.0)
        capture_fn(os.path.join(stepdir, "adv-%02d.png" % n))
    return in_gameplay(d)


def tap(d, x, y, wait):
    """Tap a GUEST coordinate on the 256x192 touch screen.

    Uses the raw `touch` command deliberately. `frontend_input action=touch`
    was tried and is wrong for this: it takes WINDOW coordinates and runs them
    through the enhanced mapping, which converts to the 448-wide surface and
    then REJECTS anything outside the centred 256-column band. Window (128, 96)
    becomes guest x = 64 - 96, i.e. negative, so every tap was silently
    discarded and the route sat on the title screen.

    An earlier note here blamed a race with the frontend for raw touches being
    dropped. That was wrong -- the real cause was two emulator instances
    sharing the debug port (docs/198), so the taps were landing in the process
    that was NOT being photographed. Raw touch works.
    """
    d.cmd("touch", x=x, y=y, down=True)
    time.sleep(0.15)
    d.cmd("touch", x=x, y=y, down=False)
    time.sleep(wait)


def lb_probe_cull(d, width):
    """Rewrite the entity-cull view width in func_02038F28.

    The gather rect is (camx, camy)..(camx + 0x100, camy + 0xC0) and the width
    is a single ARM immediate at 0x02038FD0, stock E2822C01.
    """
    imm = None
    for rot in range(16):
        r = ((width << (2 * rot)) | (width >> (32 - 2 * rot))) & 0xFFFFFFFF
        if r <= 0xFF:
            imm = (rot << 8) | r
            break
    if imm is None:
        raise RuntimeError("%d is not an ARM immediate" % width)
    cur = int.from_bytes(bytes.fromhex(
        d.cmd("read_mem", cpu=9, addr=0x02038FD0, len=4)["hex"]), "little")
    if (cur & 0xFFFFF000) != 0xE2822000:
        raise RuntimeError("%08X is not `add r2, r2, #imm`" % cur)
    d.cmd("write_mem", cpu=9, addr=0x02038FD0, value=0xE2822000 | imm, width=32)


def screen_sig(d):
    """A cheap fingerprint of the field screen."""
    r = d.cmd("framebuffer", engine="B")
    raw = binascii.unhexlify(r["rgb"])
    return tuple(raw[i] for i in range(0, len(raw), 61))


def tap_verified(d, x, y, wait, tries=4):
    """Tap, and confirm the screen actually changed.

    Every step of this route was a fixed sleep, which couples it to how fast the
    game happens to be running. When one tap lands during a transition it does
    nothing, the route carries on pressing coordinates of a screen it never
    reached, and the run ends somewhere plausible -- the main menu, in the case
    that prompted this. Retrying on "the screen did not move" turns a silent
    wrong-screen result into either a success or an honest failure.
    """
    # TAP ONCE. NEVER RETRY.
    #
    # Retrying was worse than useless. Half this route's steps are SELECTION
    # taps that only move a highlight -- far below any sane change threshold --
    # so they were read as "no response" and tapped up to four times. The second
    # tap confirms and moves to the next screen, and the third and fourth then
    # land on THAT screen, where (205,169) is a Back button on several. That is
    # why a route with verified coordinates walked forward and then came back
    # out to the title.
    #
    # lb_drive.py drove this exact sequence with single taps and reached the
    # battle every time. So: tap once, wait for the screen to stop moving, and
    # report whether it changed as INFORMATION rather than acting on it.
    THRESH = 0.90
    before = screen_sig(d)
    tap(d, x, y, 0.4)
    settle(d, timeout=max(wait, 6.0))
    now = screen_sig(d)
    same = sum(1 for p, q in zip(now, before) if p == q)
    return same < len(now) * THRESH


def settle(d, timeout=8.0, quiet=0.6):
    """Wait until the screen stops changing.

    Animated screens never freeze completely, so "settled" means two samples
    that mostly agree, held for `quiet` seconds -- not two identical ones.
    """
    end = time.time() + timeout
    prev = None
    stable_since = None
    while time.time() < end:
        sig = screen_sig(d)
        if prev is not None:
            same = sum(1 for a, b in zip(sig, prev) if a == b)
            if same >= len(sig) * 0.97:
                if stable_since is None:
                    stable_since = time.time()
                elif time.time() - stable_since >= quiet:
                    return True
            else:
                stable_since = None
        prev = sig
        time.sleep(0.2)
    return False


def in_gameplay(d):
    """True when the field screen is mostly terrain.

    Guards every measurement below. Timing a menu against a battle has already
    produced one nonsense result in this project (docs/191), and the route is
    timed in real seconds so it genuinely can end somewhere else.
    """
    # Both screens, best of. The debug server's `engine` argument selects a
    # SCREEN, not a 2D engine -- and in this title engine A drives the bottom
    # screen, so "engine=A" returns the top one. Asking for the wrong screen
    # made a real battle look like a menu and aborted three runs.
    green = total = 0
    for eng in ("A", "B"):
        r = d.cmd("framebuffer", engine=eng)
        raw = binascii.unhexlify(r["rgb"])
        g = t = 0
        for i in range(0, r["w"] * r["h"] * 3, 21):
            rr, gg, bb = raw[i], raw[i + 1], raw[i + 2]
            t += 1
            if gg > rr + 24 and gg > bb + 24 and gg > 60:
                g += 1
        if t and g / t > (green / total if total else 0):
            green, total = g, t
    # COLOUR IS NOT THE TEST ANY MORE. It is kept only as a reported hint.
    #
    # "15% green" passed on the ARMY-SELECT MENU, which has a green unit
    # preview and, in this port, the Top Screen panel showing the live field
    # behind it. Two runs were measured on that menu and reported success, and
    # conclusions about the fog were drawn from them before the captures were
    # looked at (docs/203).
    #
    # The field scene is structurally identifiable, so identify it structurally:
    # display mode 1 with BG0 as the 3D layer and BG1..BG3 enabled (measured
    # DISPCNT 00010F08, docs/193), plus a live view object and a map whose
    # metatile dimensions are sane. That is the port's OWN gate for widening,
    # so the harness and the feature now agree about when a battle is a battle,
    # and no menu can satisfy it.
    hint = (green / total) if total else 0.0
    try:
        dispcnt = d.cmd("read_io", cpu=9, addr=0x04000000, width=32)["value"]
        if ((dispcnt >> 16) & 3) != 1:
            return False
        # BG0 AND BG1, WHICH IS WHAT THE PORT ACTUALLY REQUIRES.
        #
        # This read `(dispcnt & 0xF00) != 0xF00` -- all four backgrounds on --
        # directly under a comment claiming it is "the port's OWN gate for
        # widening". It is not: lb_widemap_fill tests bit 3 (BG0 is the 3D
        # layer) with bits 8 and 9 only, and says nothing about BG2 or BG3.
        # Being stricter than the feature means the harness can call a frame a
        # menu while the port is happily widening it, and this check ABORTS THE
        # RUN -- four consecutive 20-minute runs were thrown away that way,
        # each one reporting "route did not reach gameplay" about a battle it
        # then lingered 240 seconds inside, with the port's own fog scan
        # running throughout. Matching the feature is the point of the comment;
        # now it does.
        if not (dispcnt & 0x8) or (dispcnt & 0x300) != 0x300:
            return False
        view = int.from_bytes(bytes.fromhex(
            d.cmd("read_mem", cpu=9, addr=0x02152034, len=4)["hex"]), "little")
        mapp = int.from_bytes(bytes.fromhex(
            d.cmd("read_mem", cpu=9, addr=0x02156CB0, len=4)["hex"]), "little")
        for p in (view, mapp):
            if not (0x02000000 <= p < 0x02400000):
                return False
        dims = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=mapp + 0x14,
                                   len=2)["hex"])
        if not (8 <= dims[0] <= 200 and 8 <= dims[1] <= 200):
            return False
    except Exception:                              # noqa: BLE001
        # A structural check that cannot run is a failed check, not a pass.
        return False
    del hint
    return True


def _write_png(path, w, h, rgb):
    """Minimal RGB PNG, so a framebuffer can be saved without Pillow."""
    import struct as _s
    import zlib as _z
    raw = b"".join(bytes(1) + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        body = _s.pack(">I", len(data)) + tag + data
        return body + _s.pack(">I", _z.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as fh:
        fh.write(bytes([137, 80, 78, 71, 13, 10, 26, 10]))
        fh.write(chunk(b"IHDR", _s.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", _z.compress(raw, 6)))
        fh.write(chunk(b"IEND", b""))


def peek_dump(d, specs):
    """Read guest memory for each "HEXADDR:HEXLEN" and format it as words.

    Called before AND after the linger so a pair brackets the wait; "is this
    counter moving?" is the question a single dump cannot answer.
    """
    lines = []
    for spec in specs:
        try:
            a_s, n_s = spec.split(":")
            addr, n = int(a_s, 16), int(n_s, 16)
        except ValueError:
            lines.append("peek: bad spec %r (want HEXADDR:HEXLEN)" % spec)
            continue
        try:
            raw = bytes.fromhex(
                d.cmd("read_mem", cpu=9, addr=addr, len=n)["hex"])
        except Exception as e:                         # noqa: BLE001
            lines.append("peek %08X: %s" % (addr, e))
            continue
        for off in range(0, len(raw), 16):
            chunk = raw[off:off + 16]
            words = " ".join(
                "%08X" % int.from_bytes(chunk[i:i + 4], "little")
                for i in range(0, len(chunk) - 3, 4))
            lines.append("%08X  %s  %s" % (
                addr + off, words,
                "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    return lines


def peekptr_dump(d, specs):
    """Follow a pointer, then dump: "PTRADDR:OFFSET:LEN" (all hex).

    The objects that matter most here are reached through a pointer and
    MOVE BETWEEN RUNS -- the view is `*(u32*)0x02152034` and lb_widemap
    reads it that way for exactly that reason. A static --peek cannot
    name view+0x164, and hardcoding an address observed in one run is
    how a probe ends up reporting a stale object's bytes as fact.

    Prints the pointer it followed, so a null or nonsense deref is
    visible as itself rather than as an empty dump.
    """
    lines = []
    for spec in specs:
        try:
            p_s, o_s, n_s = spec.split(":")
            # "O1,O2,..." -- every offset but the LAST is another deref, so a
            # buffer hanging off a field of a moving object can be reached:
            # 02152034:174,0:180 is view -> view+0x174 -> that buffer.
            offs = [int(x, 16) for x in o_s.split(",")]
            pa, n = int(p_s, 16), int(n_s, 16)
        except ValueError:
            lines.append("peekptr: bad spec %r (want PTR:OFF[,OFF...]:LEN)"
                         % spec)
            continue
        try:
            raw = bytes.fromhex(
                d.cmd("read_mem", cpu=9, addr=pa, len=4)["hex"])
            base = int.from_bytes(raw, "little")
        except Exception as e:                         # noqa: BLE001
            lines.append("peekptr %08X: %s" % (pa, e))
            continue
        lines.append("peekptr [%08X] -> %08X, +%s:"
                     % (pa, base, ",".join("%X" % o for o in offs)))
        if not (0x02000000 <= base < 0x02400000):
            lines.append("    not a main-RAM pointer; not dereferenced")
            continue
        bad = False
        for step in offs[:-1]:
            try:
                raw = bytes.fromhex(
                    d.cmd("read_mem", cpu=9, addr=base + step,
                          len=4)["hex"])
            except Exception as e:                     # noqa: BLE001
                lines.append("    deref +%X: %s" % (step, e))
                bad = True
                break
            base = int.from_bytes(raw, "little")
            lines.append("    +%X -> %08X" % (step, base))
            if not (0x02000000 <= base < 0x02400000):
                lines.append("    not a main-RAM pointer; stopping")
                bad = True
                break
        if bad:
            continue
        addr = base + offs[-1]
        try:
            raw = bytes.fromhex(
                d.cmd("read_mem", cpu=9, addr=addr, len=n)["hex"])
        except Exception as e:                         # noqa: BLE001
            lines.append("    %08X: %s" % (addr, e))
            continue
        # POPCOUNT FIRST. For a fog/shroud bitmask the question is never what
        # the bytes are, it is how many bits are set and whether that number
        # GROWS -- an explored record accumulates and a currently-visible one
        # does not. Printing the hex without it invites reading four zero
        # words as "the mask is empty" when the set bits are further in.
        bits = sum(bin(b).count("1") for b in raw)
        lines.append("    %d of %d bits set (%.1f%%), %d nonzero bytes"
                     % (bits, len(raw) * 8, 100.0 * bits / max(len(raw) * 8, 1),
                        sum(1 for b in raw if b)))
        for o in range(0, len(raw), 16):
            chunk = raw[o:o + 16]
            words = " ".join(
                "%08X" % int.from_bytes(chunk[i:i + 4], "little")
                for i in range(0, len(chunk) - 3, 4))
            lines.append("    %08X  %s  %s" % (
                addr + o, words,
                "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    return lines


def title_blue(d):
    """How much of the bottom screen is the title's blue gradient, 0..1.

    "Touch the Screen to Start" is a full-screen blue gradient and nothing
    earlier in the boot is blue, so a blue census identifies it exactly --
    where "the screen stopped being black" passes on the copyright page and
    costs the route its first four taps.

    Module scope because tools/lb_mp.py has to wait for TWO title screens.
    """
    r = d.cmd("framebuffer", engine="B")
    raw = binascii.unhexlify(r["rgb"])
    blue = total = 0
    for i in range(0, r["w"] * r["h"] * 3, 3 * 7):
        rr, gg, bb = raw[i], raw[i + 1], raw[i + 2]
        total += 1
        if bb > rr + 20 and bb > 40:
            blue += 1
    return (blue / total) if total else 0.0


# The pid of the runner THIS harness launched. lb_capture.ps1 matches windows
# by title, and every instance of the port carries the same one, so without
# this a run photographs -- and RESIZES -- whichever instance it finds first.
# With another instance of the game open, that one was moved to 60,60 and
# stretched to whatever size the sweep wanted. It also meant the measurement
# came from a window someone was playing in rather than the configuration
# under test.
CAPTURE_PID = 0


def _windows_path(p):
    """/d/x/y (MSYS python's spelling) -> d:/x/y, for Windows programs."""
    m = re.match(r"^/([a-zA-Z])/(.*)$", p)
    return "%s:/%s" % (m.group(1), m.group(2)) if m else p


_capture_warned = False


def capture(out, width=0, height=0):
    # THE PATH GOES TO WINDOWS POWERSHELL, so it must be a Windows path: under
    # MSYS python it arrives as /d/..., GDI+ cannot save there, and the capture
    # was silently empty (lb_capture.ps1 now converts it too, and fails loudly).
    cmd = ["powershell", "-ExecutionPolicy", "Bypass", "-File",
           os.path.join(HERE, "lb_capture.ps1"), "-Out",
           _windows_path(os.path.abspath(out)) if out != os.devnull else out]
    if width and height:
        cmd += ["-Width", str(width), "-Height", str(height)]
    if CAPTURE_PID:
        cmd += ["-OwnerPid", str(CAPTURE_PID)]
    r = subprocess.run(cmd, capture_output=True, timeout=90)
    global _capture_warned
    if r.returncode != 0 and not _capture_warned:
        # Say it once rather than never: a missing capture used to look exactly
        # like a capture nobody opened.
        _capture_warned = True
        err = (r.stderr or b"").decode("utf-8", "replace").strip()
        print("capture FAILED (exit %d) for %s: %s"
              % (r.returncode, out, err.splitlines()[0] if err else "no stderr"))


def capture_info():
    """Where the client area sits inside a capture, measured not assumed.

    capture() photographs the outer window rect and frontend_input takes
    client pixels, so comparing "where I clicked" with "where the game drew
    it" needs the offset between the two. Returns (chrome_x, chrome_y,
    client_w, client_h), or None if the window could not be read.
    """
    cmd = ["powershell", "-ExecutionPolicy", "Bypass", "-File",
           os.path.join(HERE, "lb_capture.ps1"), "-Out", os.devnull, "-Info"]
    if CAPTURE_PID:
        cmd += ["-OwnerPid", str(CAPTURE_PID)]
    r = subprocess.run(cmd, capture_output=True, timeout=60)
    txt = (r.stdout or b"").decode("utf-8", "replace")
    import re as _re
    m = _re.search(r"client=(\d+)x(\d+) origin=(-?\d+),(-?\d+) "
                   r"chrome=(-?\d+),(-?\d+)", txt)
    if not m:
        return None
    cw, ch, _ox, _oy, kx, ky = (int(v) for v in m.groups())
    return (kx, ky, cw, ch)


def resize_window(width, height):
    """Resize the window WITHOUT photographing it.

    The end-of-run sweep walks eight window sizes to measure magnification
    and field of view, and it used to screenshot every one -- eight
    stretched PNGs per run that nothing reads. The resize is what the
    measurement needs; the
    picture is not.
    """
    cmd = ["powershell", "-ExecutionPolicy", "Bypass", "-File",
           os.path.join(HERE, "lb_capture.ps1"), "-Out", os.devnull,
           "-Width", str(width), "-Height", str(height), "-NoShot"]
    if CAPTURE_PID:
        cmd += ["-OwnerPid", str(CAPTURE_PID)]
    r = subprocess.run(cmd, capture_output=True, timeout=90)
    # PRINT WHERE IT LANDED. The sweep used to drag the window to screen
    # (60, 60) -- the main display's own corner -- on every resize, which is
    # what put the test windows on the main display. It now resizes about
    # the window's current top-left, and this is how a regression would be
    # noticed: if test windows are placed on a secondary display, an x on the
    # main display's range means the window has moved back there.
    out = (r.stdout or b"").decode("utf-8", "replace").strip()
    if out:
        print("  %s" % out.replace(chr(10), " | "))


class _NudgeRefused(Exception):
    """The hardcoded view pointer disagrees with the port's own camera."""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--env", action="append", default=[])
    # EXTRA RUNNER FLAGS.
    #
    # The Popen line below is hardcoded, so a runner flag that only exists on
    # the command line was unreachable from the harness. (This was added under
    # a wrong diagnosis -- that --coverage-manifest was the only safe way to
    # read tier-3 coverage because the debug commands raced. They do not; the
    # server is single-client and --keep was holding the only slot, see
    # docs/254. Kept because passing runner flags is useful on its own.)
    ap.add_argument("--exe-arg", action="append", default=[],
                    help="extra argument passed straight to nds_runner")
    ap.add_argument("--config", default=os.path.join(ROOT, "port", "game.toml"))
    ap.add_argument("--pan", action="store_true")
    ap.add_argument("--drag-boundary", action="store_true",
                    help="item 5: does a drag box that CROSSES the "
                         "native boundary behave differently from one that "
                         "does not? Paired trials on the same target with the "
                         "same box size, differing only in whether the box "
                         "spans x=0. --drag-unit cannot answer this: it needs "
                         "the unit parked in x 60..190 so its +/-40 box stays "
                         "native, and on this route the hero sits at sx=18, so "
                         "both of its trials land in the band and the control "
                         "is not a control.")
    ap.add_argument("--drag-native", action="store_true",
                    help="item 8: drag entirely inside the native 256x192 via "
                         "RAW TOUCH and measure camera drift. Raw touch needs "
                         "no [lb_geom], which is why this works in Vanilla "
                         "where --drag-boundary stops at geom=None -- so this "
                         "is the arm that can compare the port against the "
                         "cartridge. Two paths per rep: one hugging the left "
                         "edge, one across the middle, so a run also carries "
                         "its own control for edge-scrolling.")
    ap.add_argument("--pause-nocap", action="store_true",
                    help="with --pause-cycle, skip the capture burst after "
                         "the unpause, so the frame stats there are the "
                         "game's and not the harness's (docs/349)")
    ap.add_argument("--pause-cycle", action="store_true",
                    help="item 10: pause with START, linger, unpause, "
                         "linger, and report the port's terrain/fog counters "
                         "either side. The port has NO pause handling at all -- "
                         "lb_widemap_scan runs every frame regardless -- so if "
                         "a pause menu changes the BG setup the model is being "
                         "rebuilt from a menu's tilemap, and model_bad_frames' "
                         "after-first-ok term (docs/310) is the detector: it is "
                         "0 across a whole --pan run, so any non-zero here is "
                         "the pause and nothing else.")
    # Which way to pan. The cull rect runs from the camera RIGHTWARD
    # (camx .. camx + width), so widening it can only be proved by pushing
    # units off the RIGHT edge, which means panning LEFT (KEYINPUT bit 5).
    ap.add_argument("--pan-key", type=lambda v: int(v, 0), default=0x10)
    # How far to pan. Six holds walks the camera most of the way across the
    # map, which is right for asking whether an artefact is screen- or
    # map-anchored, and wrong for asking whether a building still draws once
    # it crosses out of the native columns -- that needs it to move a little.
    ap.add_argument("--pan-steps", type=int, default=6)
    # Record the RENDER POLYGON LIST mid-hold, per pan step. See the note at
    # the sampling site: every earlier drift instrument reported a union over
    # the whole 3D layer, which the screen-fixed in-field HUD dominates.
    ap.add_argument("--polysample", action="store_true",
                    help="dump gx_polygons during the pan to polysample.jsonl")
    # How long each pan step holds the key. The default 0.35s travels about
    # 27 tiles, which is far too coarse to place a unit in a widened column.
    ap.add_argument("--pan-hold", type=float, default=0.35)
    ap.add_argument("--pan-return", action="store_true",
                    help="pan back to the start and capture, for a same-camera diff")
    # Size the window BEFORE panning, so the run samples the configuration
    # under study. The band cross-check only records cells the surface actually
    # asks for: a surface 192 rows tall has no row above or below the guest's
    # window, so a vertical pan against a default-sized window records nothing
    # at all and reports a clean ABOVE/BELOW 0/0 -- which reads exactly like a
    # pass. That is measurement bug 4 (make sure the trace samples the
    # configuration under study), and this is the flag that prevents it.
    # SIZE THE WINDOW BEFORE THE ROUTE RUNS, so the level LOADS at the size
    # under test. --pan-size resizes after the route, which means every
    # measurement of the load itself was taken at the default small window --
    # and the band is far bigger at 1600x900, so "does the extended view lag
    # the native one on load" could not be asked at a realistic play size.
    ap.add_argument("--window-size", default=None, metavar="WxH",
                    help="resize before the route, so the level loads at WxH")
    ap.add_argument("--pan-size", default=None, metavar="WxH",
                    help="resize the window to WxH before panning")
    # WHICH FREE PLAY MAP. The route has always taken the default, "The Pond",
    # so every terrain measurement so far described one map's tile data;
    # terrain needs testing across various Free Play maps.
    #
    # Map select is a carousel -- one preview, the name between a left and a
    # right arrow (screenshots/exp-mapsel/1280x720.png). The d-pad does NOT
    # drive it: two Right presses left the name on "The Pond"
    # (screenshots/exp-mapsel3/route/06-ok-key10.png). The right arrow is a
    # touch target at native (204, 134), measured off that screenshot through
    # the one native point the route already knows -- Continue at (205, 169) --
    # assuming the native 256x192 content is centred and uniformly scaled,
    # which both axes agreed on at s = 2.11. Two taps there turn The Pond into
    # Avast (screenshots/exp-mapsel4/route/07-ok-204-134.png).
    ap.add_argument("--map-index", type=int, default=0, metavar="N",
                    help="advance the Free Play map carousel N times")
    # Order a unit somewhere new and watch the visibility grid.
    #
    # The one test still owed for docs/206: an explored record's revealed count
    # can only RISE, and every run so far has left it constant because the
    # harness never moves anything. "Never shrank" already disqualified a wrong
    # candidate; "never grew" is not confirmation of the right one.
    # Move the camera by a PRECISE number of pixels, which --pan cannot do:
    # one pan step holds the key long enough to travel ~27 tiles. Needed to put
    # a known unit inside a widened column and see whether it still draws.
    ap.add_argument("--nudge-cam", type=int, default=0)
    ap.add_argument("--explore", action="store_true")
    ap.add_argument("--overlay", action="store_true")
    # Seed a DIFFERENT save, so a modified one can be tested without
    # touching the reference copy.
    ap.add_argument("--seed-save", default=None)
    ap.add_argument("--click-x", type=int, default=410)
    ap.add_argument("--click-y", type=int, default=86)
    # Tap a guest coordinate to SELECT something. The in-field HUD -- the
    # unit-action, build and deselect buttons -- does not exist until a unit is
    # selected, so every capture taken so far has been of a screen that has no
    # HUD on it. "The HUD does not move to the corners" cannot be investigated
    # against a frame where the HUD was never drawn.
    ap.add_argument("--select", nargs=2, type=int, metavar=("X", "Y"))
    # Stop the route early and photograph a MENU instead of a battle. The cover
    # fit crops the widened columns, and the menus anchor buttons to the edges
    # of the widened surface -- so "the window is full" and "the Main Menu
    # button is still on screen" are separate questions.
    ap.add_argument("--stop-after", type=int, default=0)
    # Widen the ENTITY CULL rectangle at runtime and see whether more units are
    # submitted. func_02038F28 builds the query rect as
    # (camx, camy)..(camx + 0x100, camy + 0xC0); the width is a single ARM
    # immediate at 0x02038FD0. Patching it in place is the cheapest possible
    # test of whether that really is the bound, before any of it is built.
    ap.add_argument("--cull-width", type=lambda v: int(v, 0), default=0)
    ap.add_argument("--cull-ab", action="store_true")
    # Reach gameplay, then STAY UP so probes can be run against a live battle
    # without paying for the route again.
    #
    # The obvious alternative is a savestate, and this runner cannot cheaply
    # have one: it is a static recompiler, so the guest CPU state lives in the
    # recompiled register file rather than in melonDS's cores, and the vendored
    # Savestate.cpp is unused. A restore that quietly missed a region would not
    # fail -- it would produce measurements from a subtly wrong machine, which
    # is the failure this project has already paid for four times. Holding one
    # correct instance open costs a boot and is exact by construction.
    ap.add_argument("--keep", action="store_true")
    # Boot and stop. The route is worth fixing but not worth paying for while
    # working out WHAT the route should be: one long-lived instance that can be
    # driven interactively answers in one boot what one-run-per-hypothesis
    # was answering in six.
    ap.add_argument("--no-route", action="store_true")
    # REPLAY A RECORDED ROUTE INSTEAD OF RE-WALKING IT WITH SLEEPS.
    #
    # Testing with simulated timed inputs that reproduce a menu navigation is
    # inefficient. The route tables above are exactly that -- measured tap
    # coordinates separated by
    # fixed waits, guessed once per menu screen and re-tuned whenever one
    # changed. lb_demo already records at the guest boundary and replays
    # frame-indexed, so a route the player walked once is reproducible without
    # any of it, and reaches levels no route table has coordinates for (the
    # first King level tutorial among them).
    #
    # Implies --no-route: the demo IS the route, and driving taps on top of a
    # replay would fight it.
    ap.add_argument("--demo", default=None, metavar="PATH",
                    help="replay a recorded .lbdemo instead of walking menus")
    # Which menu path to walk. "story" is the one that can show modelled
    # special units; "free" is the Free Play route every earlier run used.
    ap.add_argument("--route", choices=("free", "story", "mp"),
                    default="free")
    # Append taps to the route, "X,Y[,SECONDS]", repeatable. Menu
    # reconnaissance is a loop of "tap here, look at the picture, tap there",
    # and every turn of it used to mean editing the route table in this file.
    ap.add_argument("--tap", action="append", default=[], metavar="X,Y[,S]")
    # Taps made IN GAMEPLAY, after --advance and --dismiss, "X,Y[,SECONDS]"
    # in guest coordinates, repeatable, each photographed. --tap cannot do
    # this: it joins the route, which runs before the battle exists, and the
    # campaign tutorial's first box waits for a unit to be touched.
    ap.add_argument("--game-tap", action="append", default=[],
                    metavar="X,Y[,S]")
    # How many generic Continue/A attempts to make after the route runs out of
    # known coordinates. 0 disables it.
    ap.add_argument("--advance", type=int, default=0)
    # Tap the tutorial box away so the camera unlocks (see below).
    ap.add_argument("--dismiss", action="store_true")
    # Pan until a Hero (blueprint category 0) stands in a widened column.
    ap.add_argument("--aim-hero", action="store_true")
    # Sit in gameplay for N seconds. Effects only exist while something is
    # HAPPENING, and a run that reaches a battle and immediately starts
    # probing sees an idle field. Combat is what spawns them.
    ap.add_argument("--linger", type=int, default=0)
    # After a --demo replay finishes, wait N seconds WITHOUT ANY INPUT before
    # the probes (which hold keys and move the camera) start. The game plays
    # on unattended -- the AI keeps building and attacking -- and, input-free,
    # it does so identically run to run, so a later moment can be recorded by
    # LB_DUMP_FRAME. docs/350: the fog-finder loop that began ~22,000 frames
    # after a recorded replay ended was found this way by accident.
    ap.add_argument("--demo-linger", type=int, default=0)
    # Capture the first N seconds of gameplay at 1s spacing, for the cold
    # start of the widened area. --linger samples every 10s and cannot see it.
    ap.add_argument("--coldshots", type=int, default=0, metavar="N")
    # Order a unit out to this surface x (a band coordinate, e.g. 400) and
    # watch for a NAMED effect classified there, photographing the widened
    # surface with the shroud state printed beside it.
    ap.add_argument("--fxband", type=int, default=0, metavar="SURFACE_X")
    # The VERTICAL twin. Same probe, same route, same click delivery -- the
    # difference is only which coordinate of the second click is moved off the
    # native screen, and which axis of the result is scored.
    #
    # It has to be a separate flag rather than a second value on --fxband
    # because the two ask different questions and a run that moved both at once
    # could not say which axis carried the order. docs/238's whole retraction
    # came from a probe that could not separate its own arithmetic from the
    # game's.
    ap.add_argument("--fyband", type=int, default=0, metavar="SURFACE_Y",
                    help="order a unit to a ROW outside the native 0..191 and "
                         "report where it ends up vertically")
    # Dump the 3D polygon list before and after a camera pan. Whatever does
    # not move is screen-locked, which is the HUD.
    ap.add_argument("--polydump", action="store_true")
    # Capture the SIZES sweep from inside gameplay, not from a menu. The
    # field of view question is about the battlefield.
    ap.add_argument("--sizes-in-game", action="store_true")
    # Keep the eight stretched window shots the magnification sweep used to
    # leave in every run directory. Off by default; the sweep's NUMBERS are
    # printed either way.
    ap.add_argument("--size-shots", action="store_true")
    # Dump arbitrary guest memory once gameplay is reached.
    #
    # Every probe so far has had to be written into this file as its own flag,
    # which is fine for a question asked twice and absurd for one asked once.
    # "What is at 0x02155494 during a match?" needs no code.
    #
    #     --peek 2155494:40 --peek 2155878:40
    ap.add_argument("--peek", action="append", default=[],
                    metavar="HEXADDR:HEXLEN")
    # Follow a pointer first: --peekptr 02152034:164:20 reads the view
    # pointer, then dumps view+0x164. The view MOVES BETWEEN RUNS, so a
    # static --peek cannot name anything inside it.
    ap.add_argument("--peekptr", action="append", default=[],
                    metavar="HEXPTR:HEXOFF:HEXLEN")
    # Find a literal string in main RAM once gameplay is reached.
    #
    #     --findstr Tileset
    #
    # The map's world (castle / pirate / space) is its TILESET, and the tileset
    # is loaded by name, so the name is the honest way to read the world rather
    # than inferring it from which factions happen to be playing.
    ap.add_argument("--findstr", action="append", default=[], metavar="TEXT")
    # Is the game making any SOUND at all? Reports how many samples the SPU
    # has produced and the peak amplitude of a recent window, so "the music
    # setting does nothing" can be told apart from "nothing is audible".
    # Drain the watch ring as the LAST thing the run does, so whatever the
    # probes above provoked is still in it. Pairs with LB_WATCH.
    ap.add_argument("--touchwho", action="store_true",
                    help="watch reads of the cartridge's touch x/y during an "
                         "in-band click and name every consumer by PC")
    ap.add_argument("--drainwatch", action="store_true")
    ap.add_argument("--audio", action="store_true")
    # Dump the last N seconds of SPU output to audio.wav in the run directory.
    #
    # Every other claim in this project is settled by a measurement, and for
    # music the measurement is a listener. The port cannot listen, so it hands
    # over the sound instead: two runs that differ only in --env LB_MUSIC_MODE
    # produce two files that can be compared by ear or by any waveform tool.
    ap.add_argument("--audio-wav", type=float, default=0.0, metavar="SECONDS")
    # Locate the game's own touch position in screen pixels, by injecting
    # known touches and looking for them. Needed to make the widened
    # columns clickable: the DS touch panel is 256 px and the ADC field
    # cannot carry a coordinate outside it, so the extension has to be
    # applied ABOVE the hardware.
    ap.add_argument("--findtouch", action="store_true")
    # Does a write to the game's touch state SURVIVE, or is it rewritten
    # every frame? external-poke-loses-the-race says read it back before
    # believing anything.
    ap.add_argument("--pokemouse", action="store_true")
    # Who WRITES the touch X? A watchpoint answers directly; the static
    # scan could not, because the sampler forms the address as base+offset
    # so no literal names it.
    ap.add_argument("--whowrites", action="store_true")
    # Which guest addresses does the game NEVER write? The pool-repoint
    # needs one to use as scratch, and "the top of RAM is probably free"
    # is exactly the kind of assumption this project does not make.
    ap.add_argument("--scratch", type=int, default=0)
    # Select a unit before probing the HUD. The build/magic bricks and the
    # deselect icon DO NOT EXIST until something is selected -- so a probe
    # run with nothing selected sees one lonely quest button and
    # concludes the HUD is one element.
    ap.add_argument("--select-hero", action="store_true")
    # Click a unit that is standing OUTSIDE the native 256 columns, through
    # the real frontend mapping, and prove it got selected.
    ap.add_argument("--bandpark", type=int, default=None,
                    help="with --bandclick: park the target at this screen x")
    ap.add_argument("--bandclick", type=int, default=None,
                    help="with --bandpark: click at this screen x instead of "
                         "at the target")
    ap.add_argument("--bandland", action="store_true",
                    help="park the target at a series of screen x and click "
                         "the same negative x at each, to bracket where a "
                         "clamped click lands")
    ap.add_argument("--bandsize", default=None, metavar="WxH",
                    help="resize the window before the band trials, so the "
                         "band is wide enough to aim into")
    ap.add_argument("--bandwrap", type=int, default=None,
                    help="park the target at N and click at native N-256, "
                         "testing whether the coordinate WRAPS to eight bits "
                         "rather than being refused")
    ap.add_argument("--bandforce", type=int, default=None,
                    help="park the target at this screen x but click at native 128; pair with LB_TOUCH_FORCE=<same> to test the VALUE without the delivery")
    ap.add_argument("--banddrag", action="store_true",
                    help="with --bandselect, drag a box round the target instead of tapping it")
    ap.add_argument("--bandvalues", action="store_true",
                    help="with --bandwatch, group by the accessed VALUE instead of the address")
    ap.add_argument("--bandwatch", action="store_true",
                    help="with --bandselect, drain the LB_WATCH ring after each click and print the PC sets for comparison")
    ap.add_argument("--bandsweep", action="store_true",
                    help="with --bandselect, also walk the target left a step at a time and report where selection stops")
    ap.add_argument("--bandselect", action="store_true",
                    help="park the base in band and in the widened band and "
                         "click it, using the selection COUNT at 0215802C as "
                         "the detector instead of the HUD quad census")
    ap.add_argument("--selwrite", action="store_true",
                    help="watch the selection list while clicking empty ground "
                         "and then the base; the PCs that appear only in the "
                         "second are the selection writers")
    ap.add_argument("--touchmap", action="store_true",
                    help="sweep window x across every window size and check "
                         "the cartridge's own touch X receives the right "
                         "guest coordinate; needs no unit and no selection")
    ap.add_argument("--click-ext", action="store_true")
    # Click EVERY entity the port reports in a widened column, deselecting
    # between each. A structure is the target that matters: it does not
    # walk, so the click lands where the entity actually is.
    ap.add_argument("--click-ents", action="store_true")
    # Pan N steps, clicking the player's BASE at each one, to find the
    # exact screen x at which out-of-band selection stops working.
    # DRAG BETWEEN TWO CHOSEN NATIVE POINTS, capturing mid-drag.
    #
    # Every other drag probe aims at a DISCOVERED target and judges by what got
    # selected. None can answer "is the selection rectangle drawn, and where",
    # and no capture in this repository can either: a marquee exists only while
    # the button is held, and every capture is taken after the release.
    # Endpoints are given rather than found so the same box can be run inside
    # the native columns and across the boundary and the pictures compared.
    ap.add_argument("--dragbox", default=None, metavar="X0,Y0,X1,Y1",
                    help="drag between two NATIVE points, capturing mid-drag")
    # docs/349: DRAG-SELECT IN THE VERTICAL BAND. Steer the camera with the
    # D-pad until the player's hero stands in the widened rows above (or
    # below) the native field, then drag boxes that contain it, cross into
    # the native rows over it, and miss it -- and compare the game's selected
    # SET, per drag, with what each box contains.
    ap.add_argument("--vsel", action="store_true",
                    help="drag-select against a hero parked in the widened "
                         "rows; per-drag expected vs selected")
    ap.add_argument("--keep-settings", action="store_true",
                    help="do NOT reset the port's settings.json to "
                         "tools/lb_harness_settings.json before the run")
    ap.add_argument("--hudpan", action="store_true",
                    help="select the hero, then log camera and the native "
                         "HUD icons while holding still, panning, and "
                         "stopping (docs/349); works in vanilla too")
    ap.add_argument("--vsel-axis", choices=("y", "x"), default="y",
                    help="with --vsel, park the hero in the widened ROWS (y, "
                         "above/below the field) or COLUMNS (x, left/right)")
    ap.add_argument("--vsel-order", default="cross,contains,cross-rev",
                    help="with --vsel, the order of the three boxes that "
                         "contain the hero; only the FIRST starts from a "
                         "known-empty selection, so rotate it across runs")
    ap.add_argument("--vsel-walk", type=int, default=0, metavar="DX",
                    help="with --vsel, first order the hero DX px east (and "
                         "deselect it), so the right widened columns are "
                         "reachable from a base at the map's west edge")
    ap.add_argument("--vsel-target", default=None, metavar="LO,HI",
                    help="with --vsel, the native row range to park the hero "
                         "in above the field (default: the deepest 60 rows "
                         "the band allows, down to -30)")
    # THE TOUCH QUESTION ASKED WITHOUT A BAND ON EITHER SIDE.
    #
    # Every other click probe converts a target's native x to a window pixel
    # with a band, and the port converts that window pixel back with a band.
    # The two cancel, so the probe passes whichever band is used and cannot
    # see a wrong one -- which is exactly what happened: --bandselect reported
    # in-band selection working while clicks did not land on what was drawn.
    #
    # This asks the only version of the question a shared wrong assumption
    # cannot satisfy: press at a CHOSEN CLIENT PIXEL, and measure where the
    # game draws the selection box. The box is drawn by the cartridge from the
    # coordinate it received, so it sits outside the round trip, and a correct
    # mapping puts its anchor under the press.
    # DOES HOLDING OUT OF BAND SCROLL THE MAP, AND WHERE DOES IT STOP?
    #
    # Vanilla scrolls when the stylus is held near the screen edge, which is
    # correct and wanted. The suspicion is that the port has widened the
    # PICTURE without widening that test: nds_set_touch still receives the
    # CLAMPED coordinate, 0 or 255, so every press outside the native band may
    # read as hard against an edge and scroll when the player is pointing at
    # the middle of what they can see.
    #
    # Hold still at a series of x positions across the whole window and record
    # the camera's drift at each. A scroll zone that is only the window's own
    # edges looks like drift at the two ends and none in between; the fault
    # looks like drift everywhere outside the band.
    ap.add_argument("--edgescroll", action="store_true",
                    help="hold at points across the window, measure camera drift")
    ap.add_argument("--touchtruth", action="store_true",
                    help="press at raw client pixels, measure where the box lands")
    ap.add_argument("--drag-select", action="store_true")
    ap.add_argument("--drag-unit", action="store_true")
    ap.add_argument("--build-flow", action="store_true")
    ap.add_argument("--click-watch", action="store_true")
    ap.add_argument("--cw-trace", action="store_true")
    ap.add_argument("--cw-attempts", type=int, default=10)
    ap.add_argument("--cw-quick", action="store_true")
    ap.add_argument("--cw-base", action="store_true")
    ap.add_argument("--sel-trace", action="store_true")
    ap.add_argument("--sel-find", action="store_true")
    ap.add_argument("--drag-unit-target", default="builder", choices=("builder", "hero"))
    ap.add_argument("--build-flow-unit", default="builder", choices=("builder", "hero"))
    # Where to park the base before boxing it. Negative puts the whole box
    # outside the native band; a positive value is the in-band CONTROL that
    # says whether box-select exists at all.
    ap.add_argument("--drag-select-at", type=int, default=-60)
    ap.add_argument("--click-halves", action="store_true")
    # Where to park the base, in screen x.
    #
    # A CLAMPED click always lands at native x = 0, so a target whose hit
    # area covers 0 "selects" from an out-of-band click whether or not the
    # extended coordinate is doing anything -- measured, and it is exactly
    # what made the first reading look positive with the extension OFF.
    # Parking the base well left of 0 puts its whole hit area out of band
    # and away from where a clamp can reach.
    ap.add_argument("--click-halves-at", type=int, default=-20)
    ap.add_argument("--click-sweep", type=int, default=0)
    ap.add_argument("--click-sweep-hold", type=float, default=0.055)
    # Aim the hero OUT of the native band, then tap in-band with LB_TOUCH_FORCE
    # set to his real screen x. Tests whether an extended coordinate reaches
    # the game's selection logic, independent of the frontend mapping.
    ap.add_argument("--force-click", action="store_true")
    args = ap.parse_args()

    # --pan-return IMPLIES --pan, because its code lives inside the `if
    # args.pan:` block.
    #
    # Without this, `--pan-return` on its own is a silent no-op: the run
    # completes, reports its fps and its captures, and simply never performs
    # the return leg or writes pan-before/pan-return.png. Nothing says so. That
    # is the worst shape a measurement bug can have -- it reads exactly like
    # "the test ran and found nothing", which for a drift hunt means "no
    # drift". It cost a run here before it was noticed, and only because the
    # output directory was checked for the files rather than the exit code
    # being trusted.
    if args.pan_return:
        args.pan = True

    # Refuse to start while another instance is alive: two of them share
    # the debug port, and commands then reach one process while captures
    # come from the other (docs/198).
    assert_single_runner(PORT)

    out = os.path.join(ROOT, "screenshots", "exp-" + args.label)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    global LOGPATH
    LOGPATH = os.path.join(out, "stderr.log")

    # A private COPY of the seed save. The run must not write back to the one
    # in saves/ -- the cartridge save is treated as a read-only source
    # -- and each run needs the same starting state to be reproducible.
    run_save = os.path.join(out, "lb.sav")
    seed = args.seed_save or SEED_SAVE
    if os.path.exists(seed):
        shutil.copyfile(seed, run_save)
        print("seeded save: %s" % seed)

    env = dict(os.environ)
    env["NDS_DEBUG_ALLOW_WRITES"] = "1"
    # The runner links SDL2 and the MSYS2 C++ runtime dynamically. A shell
    # launch finds them because the MSYS2 bin directory is on that shell's
    # PATH; a launch from Python does not, and Windows reports it only as exit
    # 0xC0000135 with an empty log -- which reads exactly like "the emulator
    # started and produced nothing".
    env["PATH"] = (os.path.join(ROOT, "build") + os.pathsep +
                   "C:\\devkitPro\\msys2\\usr\\bin" + os.pathsep +
                   env.get("PATH", ""))
    env["LB_DUMP_ADAPTIVE"] = out
    # THE DIAGNOSTICS THE SELECTION PROBES PARSE, armed automatically.
    #
    # --click-sweep reads the player's base position out of the port's own
    # "[lb_base]" line, the field rect out of "[lb_geom]", and the selection
    # state out of "[lb_hud] ... hudquads=N". All three are env-gated
    # (LB_ENTLOG, LB_VERBOSE, LB_HUDLOG) and NOTHING set them, so every step
    # printed "has no geom/base yet" and the run ended "SELECTED at sx none".
    # That is indistinguishable from "clicking does not select", and it was
    # read that way: four runs across --select-hero, --select, --drag-select
    # and a plain route all reported hudquads=0, and the conclusion drawn was
    # that the harness cannot select a unit. It can; it could not SEE that it
    # had. With these set the same sweep reports "hud 0 -> 6 SELECTED".
    #
    # Armed here rather than at each probe because the failure is silent and
    # the cost is a few log lines. A measurement tool that cannot observe its
    # own subject is a broken prerequisite, not a lower priority.
    # --bandselect BELONGS HERE AND WAS MISSING, which made it unrunnable: it
    # calls geom(), geom() parses [lb_geom], [lb_geom] only exists under
    # LB_VERBOSE, and nothing set it -- so every invocation printed
    # "bandselect: geometry None" and stopped before clicking anything. The
    # probe written to answer "does a click in the widened band select?" had
    # never actually asked it.
    if args.click_sweep or args.select or args.select_hero or \
            args.drag_select or args.drag_unit or args.build_flow or \
            args.drag_boundary or args.drag_native or \
            args.click_watch or args.click_ents or args.bandselect or \
            args.dragbox or args.touchtruth or args.edgescroll or \
            args.fxband or args.fyband or args.nudge_cam or args.vsel or             args.hudpan:
        # --fxband/--fyband JOINED THIS LIST LATE, and the same way --bandselect
        # did: both call fx_geom(), fx_geom() parses [lb_geom], [lb_geom] only
        # exists under LB_VERBOSE, and the branch's own error message told the
        # caller to set it by hand. A probe that needs an environment variable
        # nobody sets is a probe that reports "no geometry" instead of an
        # answer, which is the failure this whole block exists to prevent.
        env.setdefault("LB_ENTLOG", "1")
        env.setdefault("LB_VERBOSE", "1")
        env.setdefault("LB_HUDLOG", "1")
        # AND THE BAND, because every one of these probes now AIMS with it.
        #
        # render_band() converts a target's native x to a window pixel the way
        # the port converts it back, and it reads the value from [lb_band].
        # Without this the first --bandselect run after the change fell back to
        # the centred split, aimed 252 px off, and printed four selection
        # counts that meant nothing. The probes below do not merely report the
        # band any more; they depend on it.
        env.setdefault("LB_BANDLOG", "1")
    # AND THE ARITHMETIC THE WHOLE TOUCH QUESTION TURNS ON.
    #
    # frontend.cpp prints every term of the window-to-guest mapping -- the
    # window pixel, the field rect, the surface coordinate, what the HUD
    # unmapper did to it, the band it subtracts and the coordinate it
    # publishes -- and has done all along, behind LB_TOUCHLOG. Nothing set it.
    # Many runs were spent inferring the band from where clicks appeared to
    # land while the port was willing to print it, which is the same fault
    # the LB_BANDLOG comment below records and cost the same kind of time.
    if args.demo:
        # An ABSOLUTE path: the runner's working directory is not this one, and
        # a relative path would silently record/replay somewhere else.
        env["LB_DEMO_PLAY"] = os.path.abspath(args.demo)
        if not os.path.isfile(args.demo):
            print("demo: %s does not exist" % args.demo, file=sys.stderr)
    if args.touchtruth:
        env.setdefault("LB_TOUCHLOG", "1")
        env.setdefault("LB_BANDLOG", "1")
        env.setdefault("LB_CLICKDISPATCH", "1")
    # AND THE CAMERA READBACK THE PAN DEPENDS ON.
    #
    # port_camx() parses the port's own [lb_band] line precisely because the
    # hardcoded view pointer at 0x02152034 is unreliable -- its own docstring
    # records that read returning camx -7889, tile -987 on a 192-tile map,
    # while the port said cam_tx=17 in the same frames. But [lb_band] only
    # exists under LB_BANDLOG, and nothing set it, so port_camx returned None
    # and the pan SILENTLY FELL BACK to the very pointer it was written to
    # avoid.
    #
    # That cost a whole experiment. The pan-return null test read "camx 455 ->
    # 456, residual 1" from the bad pointer and was declared closed, while the
    # port's own cam_tx went 49 -> 57: eight tiles, 64 map px, exactly the
    # 128 window px the frames differed by at magnification 2. The renderer was
    # correct and the ruler was broken.
    if args.pan or args.pan_return:
        env.setdefault("LB_BANDLOG", "1")
    # --window-size NOW SETS THE SIZE AT WINDOW CREATION.
    #
    # It used to only drive a post-launch PowerShell resize, which FAILS
    # SILENTLY: a run asking for 1600x900 reported "ASKED 1600x900, GOT
    # 1024x384" and the resize itself printed nothing. Every frame-rate and
    # geometry number this harness produced was therefore measured at 1024x384
    # whatever was requested, and the camera clamp's failing case -- a view
    # wider than the 1536 px world -- could not be reached at all.
    #
    # LB_WINDOW is applied by the port in SDL_CreateWindow's wake, before the
    # renderer exists, so there is no resize to fail. The PowerShell resize
    # below is left in place: it still moves an already-open window, and its
    # [lb_geom] confirmation is what catches this if it ever regresses.
    if args.window_size:
        env.setdefault("LB_WINDOW", args.window_size)
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    # TEE STDOUT INTO THE RUN DIRECTORY.
    #
    # Every finding this harness produces is printed, and a run takes twenty
    # minutes. Twice now a result has been lost because the caller piped the
    # output through `head` and the interesting line was past the cut, and the
    # only way to get it back was to run the whole thing again.
    class _Tee(object):
        def __init__(self, stream, path):
            self.stream = stream
            self.fh = open(path, "w", encoding="utf-8")

        def write(self, s):
            self.stream.write(s)
            self.fh.write(s)
            self.fh.flush()

        def flush(self):
            self.stream.flush()
            self.fh.flush()

    sys.stdout = _Tee(sys.stdout, os.path.join(out, "stdout.log"))

    # THE TOP-SCREEN OVERLAY MUST BE OPEN, OR THE CAPTURES LIE.
    #
    # imgui.ini beside the exe is gitignored and persists whatever state the
    # window was last left in. The harness copy had `Collapsed=1` at Pos
    # 336,149, so every PrintWindow capture showed the overlay as a bare title
    # bar and the minimap was absent from the shot -- which reads exactly like
    # "the minimap is missing" while the DEPLOYED build has `Collapsed=0` at
    # 1348,632 and shows it correctly. That is the deployed-vs-harness trap in
    # its purest form, and a capture nobody can trust is worse than none.
    #
    # So force it open before launch. Position is left alone if already set;
    # only the collapse flag is asserted.
    try:
        ini = os.path.join(ROOT, "port", "runner", "build", "imgui.ini")
        if os.path.exists(ini):
            with open(ini, encoding="utf-8") as fh:
                txt = fh.read()
            blocks, cur, name, changed = [], [], None, False
            for ln in txt.splitlines(True):
                if ln.startswith("[Window]"):
                    if cur:
                        blocks.append((name, "".join(cur)))
                    cur, name = [ln], ln.strip()
                else:
                    cur.append(ln)
            if cur:
                blocks.append((name, "".join(cur)))
            outb = []
            for nm, body in blocks:
                if nm == "[Window][Top Screen]" and "Collapsed=1" in body:
                    body = body.replace("Collapsed=1", "Collapsed=0")
                    changed = True
                outb.append(body)
            if changed:
                with open(ini, "w", encoding="utf-8") as fh:
                    fh.write("".join(outb))
                print("note: expanded the Top Screen overlay in imgui.ini "
                      "(it was collapsed; captures would not have shown it)")
    except OSError:
        pass

    # PIN THE PORT'S SETTINGS FOR EVERY RUN (docs/349).
    #
    # The port autosaves settings.json beside the executable whenever any
    # setting changes, and on 2026-09-23 the harness's copy was rewritten in
    # the middle of a run: pixel scale 1.75 -> 1.0, unit caps zeroed, two
    # gameplay fixes switched on. Every later run then simulated a different
    # game on a different surface, and the exactness gate "failed" for a
    # reason that had nothing to do with the build under test. So each run
    # starts from tools/lb_harness_settings.json -- a copy of the deployed
    # settings, which also makes a replay of a recorded play session run its
    # configuration -- and reports afterwards if the port changed it.
    #
    # ONLY the harness's own build folder is ever written. An LB_EXE pointing
    # elsewhere (the deployed build/LegoBattles.exe, say) sits beside the
    # player's own settings.json, which this must never touch.
    _pinned = os.path.join(HERE, "lb_harness_settings.json")
    _live = os.path.join(os.path.dirname(os.path.abspath(EXE)), "settings.json")
    _harness_dir = os.path.normcase(os.path.abspath(
        os.path.join(ROOT, "port", "runner", "build")))
    _pin_ok = (os.path.exists(_pinned) and not args.keep_settings and
               os.path.normcase(os.path.dirname(os.path.abspath(EXE))) ==
               _harness_dir)
    if _pin_ok:
        shutil.copyfile(_pinned, _live)
    elif not args.keep_settings and os.path.exists(_pinned):
        print("note: %s is not in the harness build folder; its settings.json "
              "is left exactly as it is" % EXE)
    log = open(os.path.join(out, "stderr.log"), "wb")
    proc = subprocess.Popen(
        [EXE, "dummy", "4000000000", "--rom", ROM,
         "--config", args.config,
         # A save private to this run. The route creates a profile, so sharing
         # one save between runs means the second run meets a profile-select
         # screen where the first met "Create" and every later tap lands on the
         # wrong screen -- the route is only reproducible from a clean save.
         "--boot", "direct", "--save-path", os.path.join(out, "lb.sav"),
         "--interactive", "--serve", "--port", str(PORT),
         "--generated-firmware", "--network", "off", "--freebios"]
        + list(args.exe_arg),
        cwd=ROOT, env=env, stdout=log, stderr=log)
    # Every capture from here on is restricted to the window this run owns.
    #
    # BY ITS WINDOWS PID. Under MSYS python (devkitPro's, if first on PATH)
    # proc.pid is the MSYS pid, which no Windows window carries, so every
    # -OwnerPid capture found "no visible window" and -- until lb_capture.ps1
    # was made to fail loudly -- wrote nothing and said nothing (2026-09-24).
    # MSYS publishes the real one at /proc/<pid>/winpid.
    global CAPTURE_PID
    CAPTURE_PID = proc.pid
    try:
        with open("/proc/%d/winpid" % proc.pid) as fh:
            CAPTURE_PID = int(fh.read().strip())
    except (OSError, ValueError):
        pass

    try:
        d = None
        for _ in range(60):
            if proc.poll() is not None:
                print("%s: runner exited %s before serving; see stderr.log"
                      % (args.label, proc.returncode), file=sys.stderr)
                return 2
            try:
                d = Dbg(port=PORT)
                break
            except Exception:                      # noqa: BLE001
                time.sleep(1)
        if d is None:
            print("no debug server", file=sys.stderr)
            return 2

        # Wait for the title rather than sleeping a fixed 22 seconds. The boot
        # is the single largest fixed cost in every experiment, and it finishes
        # when the screen stops being black, which is observable.
        # Wait for the TITLE, identified by colour, not by "the screen stopped
        # being black". Boot goes black -> publisher logos -> a copyright page
        # of grey text on black -> the title, and the old test passed on the
        # copyright page: the route then spent its first four taps on boot
        # screens and arrived at the title around step 5 with nothing left.
        #
        # "Touch the Screen to Start" is a full-screen blue gradient and nothing
        # earlier in the boot is blue, so a blue census identifies it exactly.
        def title_visible():
            return title_blue(d) > 0.40

        boot_deadline = time.time() + 90
        while time.time() < boot_deadline:
            time.sleep(1.0)
            try:
                if title_visible():
                    time.sleep(2.0)                # let the title settle
                    break
            except Exception:                      # noqa: BLE001
                pass
        else:
            print("%s: never saw the title screen" % args.label,
                  file=sys.stderr)

        if args.window_size:
            ww, wh = (int(v) for v in args.window_size.split("x"))
            # SAY WHERE IT LANDED, AND CHECK THAT IT DID.
            #
            # This called capture(), which runs lb_capture.ps1 with
            # capture_output=True and discards both streams -- so a resize that
            # silently failed was indistinguishable from one that worked. A
            # whole performance run was spent that way: --window-size 3440x1440
            # was asked for, every one of the run's 100 [lb_geom] lines said
            # window=1024x384, and the run cheerfully reported 58 fps for a
            # window nobody plays at. resize_window() prints the rect it got.
            resize_window(ww, wh)
            time.sleep(1.0)
            capture(os.path.join(out, "window-size.png"))
            got = None
            try:
                with open(os.path.join(out, "stderr.log"), "rb") as fh:
                    tail = fh.read()[-20000:].decode("latin-1", "replace")
                import re as _re
                ls = [l for l in tail.splitlines() if l.startswith("[lb_geom]")]
                if ls:
                    m = _re.search(r"window=(\d+)x(\d+)", ls[-1])
                    if m:
                        got = (int(m.group(1)), int(m.group(2)))
            except OSError:
                pass
            if got and abs(got[0] - ww) > 32:
                print("  window-size: ASKED %dx%d, GOT %dx%d -- every timing "
                      "below describes the size it GOT" % (ww, wh, got[0],
                                                           got[1]))
            elif got:
                print("  window-size: %dx%d confirmed by [lb_geom]" % got)
            else:
                print("  window-size: no [lb_geom] to confirm against; arm "
                      "LB_VERBOSE if this run's timings matter")
        chosen = (MP_ROUTE if args.route == "mp"
                  else STORY_ROUTE if args.route == "story" else ROUTE)
        # Advance the map carousel before the map-select Continue. ROUTE[6] is
        # that Continue, so the presses go in ahead of it -- and ahead of the
        # --stop-after truncation, or stopping AT the map select cuts the very
        # step that chooses the map.
        if args.map_index and args.route == "free" and len(chosen) > 6:
            chosen = (list(chosen[:6]) +
                      [(204, 134, 1.2)] * args.map_index +
                      list(chosen[6:]))
        route = [] if (args.no_route or args.demo) else (
            chosen[:args.stop_after] if args.stop_after else chosen)
        if args.demo:
            # WAIT FOR THE REPLAY, and say so if it never finishes.
            #
            # The port prints "[lb_demo] finished at frame N" when the last
            # recorded event has been delivered. Polling that beats any fixed
            # wait: a demo is frame-indexed, so how long it takes in seconds
            # depends entirely on how fast this host emulates, which is the
            # whole reason for replaying it rather than sleeping through it.
            demo_log = os.path.join(out, "stderr.log")
            # THE CEILING WAS 300.0 AND THE ROUTE BRUSHES IT.
            #
            # Measured completions of demos/user-combat-2026-09-14.lbdemo at the
            # recorded geometry: 262, 267, 268, 269, 273, 287 and 297 seconds --
            # the last one three seconds inside the budget. Anything that slows a
            # frame (a diagnostic env var, a slower fill path, a busier host)
            # pushes it over, and the run then reports "NEVER FINISHED" and
            # "frontend busy or frozen", which reads exactly like the build under
            # test froze. It cost a wrong attribution: a correct change was
            # suspected of hanging the emulator when the route had simply run
            # long.
            #
            # This loop already exits the instant the demo says it is finished,
            # so a larger ceiling costs a healthy run nothing at all. It only
            # changes how long a genuinely stuck run takes to be declared stuck.
            # EVERYTHING WRITTEN SINCE THE LAST LOOK, not the last 20 KB.
            #
            # A verbose diagnostic (LB_FXLOG=1 with LB_FXPERIOD=6 prints a few
            # hundred lines a second) pushes the "finished" line out of a
            # fixed 20 KB tail between two one-second polls. The run then
            # waited out the whole 900 s ceiling and reported NEVER FINISHED
            # for a replay that had finished 12 minutes earlier -- and the
            # game, left alone that long, played on into the AI's attack on
            # the player's base (docs/350). Reading from the previous offset, with a line's worth
            # of overlap, cannot miss it however fast the log grows.
            waited = 0.0
            seen_to = 0
            while waited < 900.0:
                try:
                    with open(demo_log, "rb") as fh:
                        fh.seek(max(0, seen_to - 256))
                        chunk = fh.read()
                        seen_to = fh.tell()
                    tail = chunk.decode("latin-1", "replace")
                except OSError:
                    tail = ""
                if "[lb_demo] finished" in tail:
                    print("demo: replay finished after %.0fs" % waited)
                    break
                time.sleep(1.0)
                waited += 1.0
            else:
                print("demo: NEVER FINISHED after %.0fs -- the probe below is "
                      "running against whatever state the replay reached, "
                      "which is not the state it was recorded for" % waited)
            # Let the last delivered input settle into the simulation before a
            # probe reads anything.
            time.sleep(1.5)
            if args.demo_linger > 0:
                print("demo: lingering %ds with no input" % args.demo_linger)
                time.sleep(float(args.demo_linger))
        # Extra taps, appended to whatever route was chosen. Menu
        # reconnaissance is a loop of "tap here, look at the picture, tap
        # there", and every turn of it used to mean editing a table above.
        for spec in args.tap:
            q = spec.split(",")
            route = list(route) + [(int(q[0]), int(q[1]),
                                    float(q[2]) if len(q) > 2 else 2.5)]
        # Photograph EVERY step. Three runs were spent inferring where the route
        # went from a single end-state screenshot, and the inference was wrong
        # each time -- the route reaches a submenu and comes back out, which no
        # end state can show. One capture per step is a few seconds and settles
        # it by evidence.
        stepdir = os.path.join(out, "route")
        os.makedirs(stepdir, exist_ok=True)
        for n, step in enumerate(route):
            if step[0] == "key":
                _, mask, presses, wait = step
                for _ in range(presses):
                    d.cmd("keys", mask=0x3FF & ~mask)
                    time.sleep(0.12)
                    d.cmd("keys", mask=0x3FF)
                    time.sleep(0.35)
                time.sleep(wait)
                ok, label = True, "key%02X" % mask
            else:
                x, y, wait = step
                ok = tap_verified(d, x, y, wait)
                label = "%d-%d" % (x, y)
            capture(os.path.join(stepdir, "%02d-%s-%s.png"
                                 % (n, "ok" if ok else "NORESP", label)))
            if not ok:
                # Informational only. A SELECTION tap legitimately changes very
                # little, so this is not a failure -- the run is judged by
                # in_gameplay() at the end, not by these.
                print("%s: step %d (%s) changed little" % (args.label, n, label))
        time.sleep(5)

        if args.advance:
            advance_to_gameplay(d, stepdir, args.advance, capture)

        if args.dismiss:
            # A story mission opens with a tutorial box across the bottom of
            # the field, and WHILE IT IS UP THE CAMERA IS LOCKED. That is why
            # the first story run reported camx = 256 for all ten pan steps and
            # BG1HOFS flat at 0: the pan was not failing to reach the game, it
            # was being refused. The box's accept control is the red tick at
            # its right-hand end, guest (224, 182) measured off
            # screenshots/exp-story3/pan-00.png.
            for i in range(3):
                tap(d, 224, 182, 1.0)
                capture(os.path.join(stepdir, "dismiss-%02d.png" % i))

        for i, spec in enumerate(args.game_tap):
            q = spec.split(",")
            tap(d, int(q[0]), int(q[1]), float(q[2]) if len(q) > 2 else 2.0)
            capture(os.path.join(stepdir, "gametap-%02d-%s-%s.png"
                                 % (i, q[0], q[1])))

        if args.aim_hero:
            aim_hero(d, os.path.join(out, "stderr.log"), stepdir, capture)

        if args.sizes_in_game:
            for size in SIZES:
                w, h = (int(v) for v in size.split("x"))
                capture(os.path.join(out, "game-" + size + ".png"), w, h)
                time.sleep(0.6)

        if args.findtouch:
            # THE WHOLE OF MAIN RAM by default. The first version scanned
            # only 0x02150000..0x02160000 because that is where the other
            # globals live, found one pair, and that pair turned out NOT
            # to feed unit selection (docs/214). Other copies of the
            # coordinate are exactly what is being looked for now.
            LO = 0x02000000
            HI = 0x02400000
            def snap(tx, ty):
                d.cmd("touch", x=tx, y=ty, down=True)
                time.sleep(0.35)
                out = bytearray()
                for off in range(LO, HI, 0x8000):
                    out += bytes.fromhex(
                        d.cmd("read_mem", cpu=9, addr=off, len=0x8000)["hex"])
                return bytes(out)
            probes = [(200, 100), (40, 150), (200, 100), (40, 150)]
            snaps = [snap(x, y) for x, y in probes]
            d.cmd("touch", x=0, y=0, down=False)
            hits = []
            for off in range(0, len(snaps[0]) - 1, 2):
                vals = [int.from_bytes(s[off:off+2], "little") for s in snaps]
                # Must track the injected X on BOTH repeats, so a counter that
                # happens to pass through the value once is not mistaken for it.
                if (abs(vals[0] - 200) <= 2 and abs(vals[1] - 40) <= 2 and
                        abs(vals[2] - 200) <= 2 and abs(vals[3] - 40) <= 2):
                    hits.append(("X", LO + off, vals))
                if (abs(vals[0] - 100) <= 2 and abs(vals[1] - 150) <= 2 and
                        abs(vals[2] - 100) <= 2 and abs(vals[3] - 150) <= 2):
                    hits.append(("Y", LO + off, vals))
            with open(os.path.join(out, "touch.txt"), "w") as fh:
                for k, a2, v in hits:
                    fh.write("%s @%08X %s\n" % (k, a2, v))
            print("findtouch: %d candidates" % len(hits))
            for k, a2, v in hits[:20]:
                print("   %s @%08X %s" % (k, a2, v))

        if args.pokemouse:
            X0, Y0 = 0x0215376A, 0x02153768
            def rd(addr, n=2):
                return int.from_bytes(bytes.fromhex(
                    d.cmd("read_mem", cpu=9, addr=addr, len=n)["hex"]), "little")
            def wr16(addr, v):
                # value= and width=, NOT hex=. Passing hex= left value
                # defaulting to 0, so every "poke" wrote a zero and the
                # first run of this probe measured nothing at all.
                d.cmd("write_mem", cpu=9, addr=addr, value=v, width=16)
            rep = []
            d.cmd("touch", x=200, y=100, down=True)
            time.sleep(0.3)
            rep.append("held (200,100): X=%d Y=%d" % (rd(X0), rd(Y0)))
            # 300 is past the panel; -40 is left of it. Both are what a click in
            # a widened column would need, and both are representable in the
            # s16 the game reads with LDRSH.
            for want in (300, 0x10000 - 40):
                wr16(X0, want)
                back = rd(X0)
                time.sleep(0.05)
                after1 = rd(X0)
                time.sleep(0.35)
                after2 = rd(X0)
                rep.append("wrote %5d -> immediate %5d, +50ms %5d, +400ms %5d"
                           % (want if want < 32768 else want - 65536,
                              back, after1, after2))
            d.cmd("touch", x=0, y=0, down=False)
            with open(os.path.join(out, "pokemouse.txt"), "w") as fh:
                fh.write(chr(10).join(rep) + chr(10))
            for line in rep:
                print("  pokemouse: " + line)

        if args.findstr:
            LO, HI = 0x02000000, 0x02400000
            blob = bytearray()
            for off in range(LO, HI, 0x8000):
                blob += bytes.fromhex(
                    d.cmd("read_mem", cpu=9, addr=off, len=0x8000)["hex"])
            lines = []
            for needle in args.findstr:
                pat = needle.encode("latin-1")
                at = 0
                hits = 0
                while hits < 40:
                    at = blob.find(pat, at)
                    if at < 0:
                        break
                    lo = max(0, at - 24)
                    ctx = bytes(blob[lo:at + len(pat) + 24])
                    lines.append("%08X  %s" % (
                        LO + at,
                        "".join(chr(b) if 32 <= b < 127 else "."
                                for b in ctx)))
                    at += 1
                    hits += 1
                lines.append("findstr %r: %d hit(s)" % (needle, hits))
            txt = "\n".join(lines)
            with open(os.path.join(out, "findstr.txt"), "w") as fh:
                fh.write(txt + "\n")
            print(txt)

        if args.audio:
            try:
                # A start of 0 is refused once the ring has wrapped ("no
                # longer retained") and the refusal carries no counters, so
                # ask with a start beyond the end instead: that is always
                # >= oldest, copies nothing, and still reports both.
                r = d.cmd("audio_samples", start=1 << 40, count=1)
                produced = int(r.get("produced", 0))
                oldest = int(r.get("oldest", 0))
                # Well behind the write head: asking for the newest 2048
                # samples returns nothing, because the ring only retains what
                # has been fully written and `produced` is already past it.
                start = max(int(oldest), int(produced) - 16384)
                r = d.cmd("audio_samples", start=start, count=2048)
                raw = bytes.fromhex(r.get("pcm_s16le", ""))
                peak = 0
                nz = 0
                for i in range(0, len(raw) - 1, 2):
                    v = int.from_bytes(raw[i:i + 2], "little", signed=True)
                    if v:
                        nz += 1
                    if abs(v) > peak:
                        peak = abs(v)
                print("audio: produced=%s oldest=%s window=%d samples "
                      "peak=%d nonzero=%d"
                      % (produced, oldest, len(raw) // 4, peak, nz))
            except Exception as e:                       # noqa: BLE001
                print("audio: %s" % e)

        if args.peek:
            args._peek_pre = peek_dump(d, args.peek)
            print(chr(10).join(args._peek_pre))

        if args.peekptr:
            print(chr(10).join(peekptr_dump(d, args.peekptr)))

        if args.audio_wav > 0.0:
            RATE = 32768                    # the DS mixer's output rate
            want = int(args.audio_wav * RATE)
            try:
                r = d.cmd("audio_samples", start=1 << 40, count=1)
                produced = int(r.get("produced", 0))
                oldest = int(r.get("oldest", 0))
            except Exception as e:                     # noqa: BLE001
                produced = oldest = 0
                print("audio-wav: %s" % e)
            # Stay a little behind the write head: the newest frames are not
            # retained yet and asking for them returns nothing.
            end = max(oldest, produced - 8192)
            start = max(oldest, end - want)
            pcm = bytearray()
            pos = start
            while pos < end:
                try:
                    rr = d.cmd("audio_samples", start=pos, count=4096)
                except Exception:                      # noqa: BLE001
                    break
                got = bytes.fromhex(rr.get("pcm_s16le", ""))
                if not got:
                    break
                pcm += got
                pos += len(got) // 4
            path = os.path.join(out, "audio.wav")
            n = len(pcm)
            with open(path, "wb") as fh:
                fh.write(b"RIFF" + (36 + n).to_bytes(4, "little") + b"WAVE")
                fh.write(b"fmt " + (16).to_bytes(4, "little"))
                fh.write((1).to_bytes(2, "little") + (2).to_bytes(2, "little"))
                fh.write(RATE.to_bytes(4, "little"))
                fh.write((RATE * 4).to_bytes(4, "little"))
                fh.write((4).to_bytes(2, "little") + (16).to_bytes(2, "little"))
                fh.write(b"data" + n.to_bytes(4, "little") + bytes(pcm))
            peak = 0
            for i in range(0, n - 1, 2):
                v = int.from_bytes(pcm[i:i + 2], "little", signed=True)
                if abs(v) > peak:
                    peak = abs(v)
            print("audio-wav: %.2fs, peak %d -> %s"
                  % (n / 4.0 / RATE, peak, path))

        if args.whowrites:
            # The watch range is armed by LB_WATCH in the environment; this just
            # holds a touch so the sampler runs, then drains the ring.
            # Deep trace makes main-RAM accesses take the SLOW path, which is
            # the one that records watch events. It defaults on, but say so
            # explicitly -- the first run of this probe returned zero events and
            # the fast path was blamed without checking.
            try:
                print("whowrites: deep_trace ->", d.cmd("deep_trace", on=True))
            except Exception as e:                       # noqa: BLE001
                print("whowrites: deep_trace failed: %s" % e)
            d.cmd("touch", x=200, y=100, down=True)
            time.sleep(0.5)
            d.cmd("touch", x=40, y=150, down=True)
            time.sleep(0.5)
            try:
                ev = d.cmd("watch", max=8192)["events"]
            except Exception as e:                       # noqa: BLE001
                ev = []
                print("whowrites: watch failed: %s" % e)
            d.cmd("touch", x=0, y=0, down=False)
            wr = {}
            rd = {}
            for e in ev:
                tgt = wr if e["write"] else rd
                tgt.setdefault(e["pc"], []).append((e["addr"], e["value"]))
            with open(os.path.join(out, "whowrites.txt"), "w") as fh:
                fh.write("events=%d%s" % (len(ev), chr(10)))
                for tag, tbl in (("WRITE", wr), ("READ", rd)):
                    for pc, lst in sorted(tbl.items(), key=lambda kv: -len(kv[1])):
                        fh.write("%s pc=%08X n=%d sample=%s%s"
                                 % (tag, pc, len(lst), lst[:4], chr(10)))
            print("whowrites: %d events, %d writing PCs, %d reading PCs"
                  % (len(ev), len(wr), len(rd)))
            for pc, lst in sorted(wr.items(), key=lambda kv: -len(kv[1]))[:6]:
                print("   WRITE pc=%08X n=%-4d sample=%s" % (pc, len(lst), lst[:3]))

        if args.scratch:
            MAGIC = 0x5A3C
            # 0x02153770 is the touch struct's +0x10, the slot docs/214's plan
            # publishes the extended X into. It has to be genuinely unwritten by
            # the game or the writer patch reads garbage; the neighbours are
            # included so a "free" reading that is really just a quiet frame
            # stands out against the fields that DO change.
            cands = [0x02153770, 0x02153772, 0x02153774, 0x02153778,
                     0x0215376A, 0x0215376C,
                     0x021503F0, 0x0215FFF8, 0x02160000, 0x023FFFF8]
            def rd16(a2):
                return int.from_bytes(bytes.fromhex(
                    d.cmd("read_mem", cpu=9, addr=a2, len=2)["hex"]), "little")
            def wr16(a2, v):
                d.cmd("write_mem", cpu=9, addr=a2, value=v, width=16)
            before = {}
            for a2 in cands:
                before[a2] = rd16(a2)
                wr16(a2, MAGIC)
            landed = {a2: rd16(a2) for a2 in cands}
            # EXERCISE THE CODE THAT WOULD WRITE IT. A quiet linger proves
            # nothing: the first version of this sat still for 40 s and duly
            # reported 0215376A and 0215376C -- the touch X and Y the watchpoint
            # caught the game writing every frame -- as FREE, because with no
            # touch the touch code never runs. Those two are kept in the
            # candidate list as a CONTROL: any run that calls them free is
            # measuring nothing.
            print("scratch: %ds of gameplay, with the screen being driven..."
                  % args.scratch)
            t_end = time.time() + args.scratch
            n = 0
            while time.time() < t_end:
                n += 1
                d.cmd("touch", x=60 + (n * 37) % 130, y=40 + (n * 23) % 110,
                      down=True)
                time.sleep(0.12)
                d.cmd("touch", x=60 + (n * 53) % 130, y=40 + (n * 31) % 110,
                      down=True)
                time.sleep(0.12)
                d.cmd("touch", x=0, y=0, down=False)
                time.sleep(0.10)
            after = {a2: rd16(a2) for a2 in cands}
            lines = []
            for a2 in cands:
                ok = landed[a2] == MAGIC and after[a2] == MAGIC
                lines.append("%08X was=%04X landed=%04X after=%04X  %s"
                             % (a2, before[a2], landed[a2], after[a2],
                                "FREE" if ok else "used/unwritable"))
            with open(os.path.join(out, "scratch.txt"), "w") as fh:
                fh.write(chr(10).join(lines) + chr(10))
            for ln in lines:
                print("  scratch: " + ln)

        if args.select_hero:
            # Put the hero in the NATIVE band first: the whole point of this
            # probe is the HUD, and a tap outside the native band is still
            # dropped by the touch mapping, so it could not select anything.
            logp0 = os.path.join(out, "stderr.log")
            aim_hero(d, logp0, stepdir, capture, tries=14, lo=40, hi=200)
            pos = hero_pos(logp0)
            # TAP WHERE THE HERO ACTUALLY IS, and RE-READ IT EACH TIME: a hero
            # walks, so a position measured three taps ago is stale. Each
            # attempt re-reads [lb_hero] and aims at the current one; the fixed
            # (128, 100) this replaced hit empty ground in every run, which is
            # why the in-field HUD never appeared and hudquads stayed at its
            # nothing-selected value.
            for attempt in range(4):
                aim = hero_pos(logp0) or pos
                if not aim or not (0 <= aim[0] < 256 and 0 <= aim[1] < 192):
                    print("select-hero: hero at %s is off the touch panel" % (aim,))
                    time.sleep(1.0)
                    continue
                tap(d, aim[0], aim[1], 1.0)
                capture(os.path.join(stepdir, "select-%02d.png" % attempt))
                time.sleep(0.8)

        if args.bandselect:
            # DOES A CLICK IN THE WIDENED BAND SELECT?
            #
            # Every previous attempt at this question failed on its DETECTOR,
            # not on the experiment. docs/228 lists four instrument faults and
            # ends by saying the synthetic click path cannot support the
            # measurement, because "selected" was inferred from the in-field
            # HUD's quad count -- which is zero whenever anchoring is off or
            # the window is narrow, and which needs the HUD to have been built
            # and drawn before the sample is taken.
            #
            # There is now a direct one. --selwrite established, by comparing a
            # click on bare ground against a click on the player's base, that
            # the selection count lives at 0x0215802C: bare ground leaves it 0,
            # and clicking the base has 020EC8F0 write 1 while 020ECADC writes
            # the list pointer beside it. So "did that click select something"
            # is a two-byte read, with no HUD, no quad census, no watch ring to
            # drain and nothing to infer.
            #
            # That makes the band question answerable the way it should always
            # have been: park the base at a known screen x, click it, read the
            # count. In band and out of band, same target, same detector.
            SEL_N = 0x0215802C
            logp = os.path.join(out, "stderr.log")

            def sel_count():
                raw = d.cmd("read_mem", cpu=9, addr=SEL_N, len=4)["hex"]
                return int.from_bytes(bytes.fromhex(raw), "little")

            def sel_handle():
                # WHICH entity, not just how many.
                #
                # The count alone said the wrap trial and its control BOTH
                # selected, which would have read as "negatives select
                # something regardless" -- and that is only true if they
                # selected the SAME thing. 0x02158028 holds the list pointer
                # and the entries are two-byte handles, so one more read
                # separates "it selected the target" from "it selected
                # whatever happened to be under the wrapped position".
                try:
                    p_raw = d.cmd("read_mem", cpu=9, addr=SEL_N - 4,
                                  len=4)["hex"]
                    ptr = int.from_bytes(bytes.fromhex(p_raw), "little")
                    if ptr < 0x02000000 or ptr >= 0x02400000:
                        return None
                    h_raw = d.cmd("read_mem", cpu=9, addr=ptr, len=2)["hex"]
                    return int.from_bytes(bytes.fromhex(h_raw), "little")
                except Exception:                        # noqa: BLE001
                    return None

            def geom_bs():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-200000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in tail.splitlines() if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                import re as _re
                m = _re.search(r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) "
                               r"(\d+)x(\d+)", ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            def ent_pos(tag="[lb_base]", owner=0):
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                for ln in reversed([l for l in tail.splitlines()
                                    if l.startswith(tag)]):
                    if "owner=%d" % owner not in ln:
                        continue
                    try:
                        return (int(ln.split("sx=")[1].split()[0]),
                                int(ln.split("sy=")[1].split()[0]))
                    except (IndexError, ValueError):
                        return None
                return None

            def deselect():
                # CLEAR IT, AND CHECK, AND TRY SOMEWHERE ELSE.
                #
                # The first version clicked one fixed spot and read the count
                # once. Three of four attempts then began with something still
                # selected and were correctly scored as unusable, which left
                # the left-band trial reporting "0 of 0" -- honest, and no
                # measurement at all.
                #
                # --selwrite showed a click on bare ground DOES clear the count
                # (020EC7E0/808/810 write 0), so a deselect that does not take
                # means that spot was not bare -- the target may have walked
                # under it, or it is the base's own footprint. Several spots,
                # each polled until the count falls, turns that from a lost
                # trial into a retry.
                # AWAY FROM THE TARGET, wherever it happens to be parked.
                #
                # The fixed corners worked while the target sat mid-panel and
                # stopped working the moment a trial parked it at x 216: the
                # (248, y) spots are within the base's own footprint, so the
                # "deselect" re-selected it and every attempt was thrown away
                # as a dirty start. The whole wrap experiment then reported
                # "0 of 0" twice -- honest, and no measurement.
                here = ent_pos()
                spots = [(8, 8), (248, 8), (8, 184), (248, 184),
                         (128, 8), (8, 96)]
                if here is not None:
                    spots = [s for s in spots if abs(s[0] - here[0]) >= 60
                             or abs(s[1] - here[1]) >= 60] or spots
                for gx, gy in spots:
                    d.cmd("touch", x=gx, y=gy, down=True)
                    time.sleep(0.20)
                    d.cmd("touch", x=0, y=0, down=False)
                    for _ in range(8):
                        time.sleep(0.08)
                        if sel_count() == 0:
                            return 0
                return sel_count()

            def drag_native(nx, ny, half=44):
                # A BOX AROUND THE TARGET, through the same mapping a click
                # uses, so the two probes differ only in the gesture.
                #
                # The old drag probe inferred success from the in-field HUD's
                # quad count -- the detector docs/231 replaced everywhere else.
                # On the selection count a drag is as measurable as a tap, and
                # the count distinguishes them: a box round one unit still
                # selects one, and a box collapsed to a line selects none.
                g = geom_bs()
                if not g:
                    return None
                gw, gh, fx, fy, fw, fh_ = g
                bx = render_band(gw)
                by = render_band_top(gh)

                def to_win(ax, ay):
                    return (int(round(fx + (ax + bx) * fw / float(gw))),
                            int(round(fy + (ay + by) * fh_ / float(gh))))

                x0, y0 = to_win(nx - half, ny - half)
                x1, y1 = to_win(nx + half, ny + half)
                d.cmd("frontend_input", action="click", x=x0, y=y0, down=True)
                time.sleep(0.12)
                for i in range(1, 6):
                    d.cmd("frontend_input", action="moveto",
                          x=int(x0 + (x1 - x0) * i / 5.0),
                          y=int(y0 + (y1 - y0) * i / 5.0))
                    time.sleep(0.06)
                d.cmd("frontend_input", action="click", x=x1, y=y1, down=False)
                time.sleep(0.30)
                return x0, y0, x1, y1

            def click_native(nx, ny):
                # Native (possibly negative) -> window, through the field rect
                # docs/230 verified column by column.
                g = geom_bs()
                if not g:
                    return None
                gw, gh, fx, fy, fw, fh_ = g
                bx = render_band(gw)
                by = render_band_top(gh)
                wx = int(round(fx + (nx + bx) * fw / float(gw)))
                wy = int(round(fy + (ny + by) * fh_ / float(gh)))
                d.cmd("frontend_input", action="click", x=wx, y=wy, down=True)
                time.sleep(0.30)
                d.cmd("frontend_input", action="click", x=wx, y=wy, down=False)
                time.sleep(0.30)
                return wx, wy

            def park_to(lo, hi, tries=25, tag="[lb_base]", owner=0):
                # Pan until the target's screen x is within [lo, hi] and its y
                # is on the panel. Same shape as click-watch's park.
                for _ in range(tries):
                    p = ent_pos(tag, owner)
                    if p is None:
                        time.sleep(1.0)
                        continue
                    if not (20 <= p[1] <= 170):
                        ykey = 0x80 if p[1] > 170 else 0x40
                        d.cmd("keys", mask=0x3FF & ~ykey)
                        time.sleep(max(0.05, min(0.40,
                                                 abs(p[1] - 95) / 32.0 * 0.12)))
                        d.cmd("keys", mask=0x3FF)
                        time.sleep(1.0)
                        continue
                    if lo <= p[0] <= hi:
                        return p
                    key = 0x10 if p[0] > hi else 0x20
                    d.cmd("keys", mask=0x3FF & ~key)
                    time.sleep(max(0.05, min(0.40,
                                             abs(p[0] - (lo + hi) // 2)
                                             / 32.0 * 0.12)))
                    d.cmd("keys", mask=0x3FF)
                    time.sleep(1.0)
                return ent_pos(tag, owner)

            def trial(label, lo, hi, attempts=4, clickx=None):
                p = park_to(lo, hi)
                if p is None:
                    print("bandselect: %-10s no target" % label)
                    return
                if not (lo <= p[0] <= hi):
                    print("bandselect: %-10s COULD NOT PARK in x %d..%d "
                          "(stuck at %d,%d) -- not counted"
                          % (label, lo, hi, p[0], p[1]))
                    return
                hits = clean = 0
                pcs = {}
                handles = {}
                counts = {}
                for _ in range(attempts):
                    before = deselect()
                    if before != 0:
                        continue          # dirty start; do not score it
                    clean += 1
                    q = ent_pos() or p
                    if clickx is not None:
                        # CLICK SOMEWHERE ELSE ON PURPOSE.
                        #
                        # FUN_020af040, the gesture recogniser whose return
                        # value gates the field-click path, stores the touch
                        # coordinate as a CHAR and then compares it as an
                        # unsigned byte. That predicts a WRAP rather than a
                        # rejection: native -40 should be seen as 216. So park
                        # the target at 216 and click at -40. If the wrap is
                        # real the click selects it; if the game simply refuses
                        # negatives, nothing happens and the theory is dead.
                        q = (clickx, q[1])
                    if args.bandforce is not None:
                        # SEPARATE THE VALUE FROM THE PATH.
                        #
                        # A band click fails, and there are two quite different
                        # reasons it could: the game refuses the COORDINATE
                        # (some test on screen x), or the band click never
                        # arrives as a proper pen-down at all (a flag, a
                        # threshold, a clamp in the delivery). Those want
                        # opposite fixes and the selection count cannot tell
                        # them apart.
                        #
                        # LB_TOUCH_FORCE=<n> makes the port publish a fixed
                        # extended X while the pen is down. So park the target
                        # at screen x = n, then click INSIDE the native panel,
                        # where the delivery is known good, while the value the
                        # game sees is the out-of-band one. If it selects, the
                        # value is fine and the delivery is the fault; if it
                        # does not, the game genuinely refuses the coordinate.
                        q = (128, q[1])
                    if args.bandwatch:
                        try:
                            d.cmd("watch_clear")
                            d.cmd("watch", max=8192)
                        except Exception:            # noqa: BLE001
                            pass
                    if args.banddrag:
                        drag_native(q[0], q[1])
                    else:
                        click_native(q[0], q[1])
                    # THE COUNT, not just whether it is non-zero.
                    #
                    # A tap selects exactly one thing. A DRAG selects every
                    # owned unit inside the swept rectangle, capped at eight.
                    # So the count separates the two, and that matters here:
                    # the gesture recogniser measures its drag threshold with
                    # UNSIGNED BYTE arithmetic on the touch coordinate, so a
                    # negative x gives a huge delta and would be classified as
                    # a drag. If band clicks report counts above one while
                    # in-band clicks report exactly one, that is the mechanism.
                    n_sel = sel_count()
                    if n_sel >= 1:
                        hits += 1
                        counts.setdefault(n_sel, 0)
                        counts[n_sel] += 1
                        h = sel_handle()
                        if h is not None:
                            handles.setdefault(h, 0)
                            handles[h] += 1
                    if args.bandwatch:
                        # WHICH CODE SAW THE CLICK, in each case.
                        #
                        # The cutoff is exactly screen x 0, so something tests
                        # the coordinate rather than the target. Comparing the
                        # PC sets for a click that selects against one that
                        # does not says whether the divergence is at the READ
                        # of the coordinate -- a reader that only runs in one
                        # case -- or downstream of it, where every reader fires
                        # in both and a later branch differs. Those want
                        # opposite next steps, and there is no way to tell them
                        # apart from the selection count alone.
                        try:
                            for e in d.cmd("watch", max=8192)["events"]:
                                # LR, not just PC. 020EC8F0 is inside a
                                # generic vector push with thirty-one static
                                # callers, so the accessing PC cannot name the
                                # code that decided; the return address can.
                                # THE ADDRESS TOO. Without it a value trace
                                # says "0200AA2C stored 300" and leaves the
                                # struct layout to be inferred from the
                                # disassembly -- which is exactly where two
                                # readings of the same event object came apart
                                # (is +0x1C a 16-bit x, or a byte x and a byte
                                # flag?). The address answers it directly.
                                k = (e["pc"], e.get("lr", 0),
                                     e.get("addr", 0), e.get("value", 0),
                                     "w" if e["write"] else "r")
                                pcs.setdefault(k, 0)
                                pcs[k] += 1
                        except Exception:            # noqa: BLE001
                            pass
                print("bandselect: %-10s target at x %d..%d -> SELECTED %d of "
                      "%d clean attempts" % (label, lo, hi, hits, clean))
                if counts:
                    print("      selection counts: %s"
                          % " ".join("%d units x%d" % (c, n)
                                      for c, n in sorted(counts.items())))
                if handles:
                    print("      selected handles: %s"
                          % " ".join("%04X x%d" % (h, n)
                                      for h, n in sorted(handles.items())))
                if args.bandwatch and pcs:
                    for (pc, lr, ad, va, rw), n in sorted(
                            pcs.items(), key=lambda kv: -kv[1])[:40]:
                        print("      %s pc=%08X lr=%08X addr=%08X val=%08X n=%d"
                              % (rw, pc, lr, ad, va, n))

            if args.bandsize:
                # A WIDER BAND, because some questions only come apart there.
                #
                # Clamp-to-255 and wrap-to-8-bits predict the same thing for
                # every click the default window allows: its band is 128 px, so
                # every reachable negative x wraps into the right half of the
                # panel, which is also where a clamp would put it. At 1920x1080
                # the band is 345, and a click at -196 wraps to 60 while a clamp
                # still sends it to 255 -- opposite predictions, one trial.
                bw, bh = (int(v) for v in args.bandsize.split("x"))
                capture(os.path.join(out, "bandsize.png"), bw, bh)
                time.sleep(2.0)
            if args.bandwatch:
                # Main-RAM accesses only record watch events on the SLOW path.
                # It defaults on, but --whowrites learned the hard way that a
                # probe which does not say so gets zero events and blames the
                # watch.
                try:
                    print("bandselect: deep_trace ->",
                          d.cmd("deep_trace", on=True))
                except Exception as e:                   # noqa: BLE001
                    print("bandselect: deep_trace failed: %s" % e)
            g0 = geom_bs()
            print("bandselect: geometry %s" % (g0,))
            if g0:
                band = render_band(g0[0])
                print("bandselect: band is %d px each side" % band)
                trial("in band", 60, 190)
                if band >= 40:
                    trial("left band", -(band - 16), -16)
                    # THE RIGHT BAND IS LIMITED BY THE MAP, not by the code.
                    #
                    # Pushing the player's base out to 272+ needs the camera at
                    # a negative x, and it clamps at the map's left edge before
                    # it gets there -- every run reported "COULD NOT PARK in x
                    # 272..585 (stuck at 259)". A window the camera can actually
                    # reach still tests what matters: at screen x 259 the byte
                    # the game used to store is 3, so the positive wrap is
                    # exercised just as the negative one is at -16.
                    trial("right band", 253, 265)
                else:
                    print("bandselect: band too narrow to aim into; use a "
                          "bigger window")
                if args.bandpark is not None and args.bandclick is not None:
                    # THE GENERAL PRIMITIVE: park HERE, click THERE.
                    #
                    # --bandwrap and --bandland are both special cases of it,
                    # and the question that needs it now is not a special case
                    # of either: does a POSITIVE out-of-range x clamp the same
                    # way a negative one does? The band is 345 px wide, so
                    # native +300 is reachable, and if a click there also
                    # selects a base parked at 240 then the fault is a plain
                    # saturate-to-the-panel-width on both sides -- which is a
                    # different fix from sign-extension, and the sixth LDRH
                    # site not moving the landing point already suggests
                    # sign-extension is not the whole story.
                    m, n = args.bandpark, args.bandclick
                    trial("park %d click %d" % (m, n), m - 6, m + 6,
                          attempts=4, clickx=n)
                if args.bandland:
                    # WHERE DOES A NEGATIVE CLICK ACTUALLY LAND?
                    #
                    # The wrap is refuted, so the coordinate is clamped -- but
                    # to what? Park the base at a series of positions and click
                    # the SAME negative x at each. The base is never hit at 128
                    # and usually is at 216, so the position where hits begin,
                    # plus the base's own half-width (about 40 px, from the -40
                    # and -80 trials at 216), locates the landing point to a
                    # few pixels. A landing point of exactly 255 and one of
                    # exactly 251 point at different instructions.
                    for park_x in (140, 160, 180, 200, 220, 240):
                        trial("land @%d" % park_x, park_x - 6, park_x + 6,
                              attempts=3, clickx=-150)
                if args.bandwrap is not None:
                    n = args.bandwrap
                    trial("wrap %d<-%d" % (n, n - 256), n - 6, n + 6,
                          attempts=4, clickx=n - 256)
                    # The control matters as much as the trial, and the first
                    # one was too close. It wrapped to forty pixels left of the
                    # target and selected 4 of 4 -- which does not refute the
                    # wrap, it just means the base's clickable footprint is
                    # wider than forty pixels, as a building's would be. A
                    # control has to land somewhere the target demonstrably is
                    # NOT, so this one wraps a hundred pixels away.
                    trial("ctrl %d<-%d" % (n, n - 356), n - 6, n + 6,
                          attempts=4, clickx=n - 356)
                if args.bandforce is not None:
                    n = args.bandforce
                    trial("forced %d" % n, n - 6, n + 6, attempts=4)
                if args.bandsweep and band >= 40:
                    # WHERE EXACTLY DOES IT STOP?
                    #
                    # "In band selects, out of band does not" names a
                    # behaviour, not a cause. The cutoff position names a
                    # cause, or at least excludes most of them: a stop at
                    # exactly screen x 0 is a sign or bounds test on the
                    # coordinate, while a stop part-way into the band is the
                    # target's own footprint leaving something, and a gradual
                    # decline is the control degrading and nothing else.
                    #
                    # So walk the base leftwards a step at a time and click it
                    # at each stop, with the same detector and the same
                    # clean-start rule as the trials above.
                    print("bandselect: cutoff sweep (base walked left, "
                          "clicked at each stop)")
                    for cx in (120, 80, 48, 24, 12, 4, -4, -12, -24, -48,
                               -80):
                        trial("x~%d" % cx, cx - 6, cx + 6, attempts=2)

        if args.selwrite:
            # WHO WRITES THE SELECTION, and does a click on a unit write it at
            # all?
            #
            # docs/230 excluded the coordinate: a click anywhere in the widened
            # field reaches the cartridge's touch struct correctly. What is
            # still unknown is whether the game ACTS on it, and the honest way
            # to find that is to identify the code that records a selection and
            # then ask what it tests.
            #
            # FUN_020e2a04 iterates a list at DAT_020e2a88+0x64 with its count
            # at +0x68, filters by ownership and caps at 8. DAT_020e2a88 is
            # 0x02157FC4 in the EUR build -- read out of the static image, not
            # guessed -- so the count is 0x0215802C. Nothing in that routine is
            # a coordinate test, so the test is in whoever FILLS the list.
            #
            # THE EXPERIMENT IS A DIFFERENCE, not an absolute. A watch on that
            # count during a click on empty ground returned zero events, which
            # on its own says nothing -- the watch might not be armed. So the
            # same watch is drained twice: once around a click on bare ground
            # and once around a click on the player's BASE, which is stationary
            # and large and whose screen position the port already prints. The
            # PCs that appear only in the second run are the selection writers.
            SEL_LO, SEL_HI = 0x02158028, 0x02158030
            logp = os.path.join(out, "stderr.log")

            def base_pos():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                for ln in reversed([l for l in tail.splitlines()
                                    if l.startswith("[lb_base]")]):
                    if "owner=0" not in ln:
                        continue
                    try:
                        return (int(ln.split("sx=")[1].split()[0]),
                                int(ln.split("sy=")[1].split()[0]))
                    except (IndexError, ValueError):
                        return None
                return None

            try:
                d.cmd("deep_trace", on=True)
            except Exception as e:                       # noqa: BLE001
                print("selwrite: deep_trace failed: %s" % e)

            def drain():
                try:
                    return d.cmd("watch", max=8192)["events"]
                except Exception as e:                   # noqa: BLE001
                    print("selwrite: watch failed: %s" % e)
                    return []

            def click_and_drain(gx, gy, label):
                try:
                    d.cmd("watch_clear")
                except Exception:                        # noqa: BLE001
                    pass
                drain()                                  # belt and braces
                d.cmd("touch", x=gx, y=gy, down=True)
                time.sleep(0.35)
                d.cmd("touch", x=0, y=0, down=False)
                time.sleep(0.35)
                ev = drain()
                pcs = {}
                for e in ev:
                    if not e["write"]:
                        continue
                    pcs.setdefault(e["pc"], []).append((e["addr"], e["value"]))
                print("selwrite: %-14s (%3d,%3d) -> %d write events, %d PCs"
                      % (label, gx, gy, sum(len(v) for v in pcs.values()),
                         len(pcs)))
                for pc, lst in sorted(pcs.items(), key=lambda kv: -len(kv[1])):
                    print("      pc=%08X n=%-3d sample=%s"
                          % (pc, len(lst), lst[:3]))
                return pcs

            bp = base_pos()
            print("selwrite: base at %s (needs LB_ENTLOG=1 to be found)" % (bp,))
            empty = click_and_drain(20, 20, "empty ground")
            hit = {}
            if bp and 0 <= bp[0] < 256 and 0 <= bp[1] < 192:
                # Three attempts: docs/218 showed an in-band click on the base
                # is not certain, and one miss would read as "no writer".
                for attempt in range(3):
                    bp2 = base_pos() or bp
                    hit = click_and_drain(bp2[0], bp2[1],
                                          "base try %d" % attempt)
                    if hit:
                        break
            else:
                print("selwrite: base is off the native panel at %s; pan first"
                      % (bp,))
            only = sorted(set(hit) - set(empty))
            if only:
                print("selwrite: PCs that write the selection ONLY when the "
                      "base is clicked: %s"
                      % " ".join("%08X" % p for p in only))
            elif hit:
                print("selwrite: the same PCs wrote in both cases -- this "
                      "range is not selection-specific")
            else:
                print("selwrite: no write to %08X..%08X in either case; either "
                      "the click did not select or the object is elsewhere"
                      % (SEL_LO, SEL_HI))

        if args.touchmap:
            # IS THE WIDENED FIELD ADDRESSABLE, END TO END?
            #
            # The requirement is that the expanded field of view on the touch
            # screen is just as interactable as the original aspect. Every
            # previous attempt to answer that went through SELECTION -- click a
            # unit, see whether it highlights -- and docs/228 records why that
            # cannot settle it: the synthetic click path was never a reliable
            # enough control, so a band failure and an instrument failure look
            # identical.
            #
            # This measures the half that CAN be measured cleanly. A click at a
            # window position must reach the cartridge's own touch struct as
            # the right guest coordinate. That is arithmetic with a definite
            # answer, it needs no unit to be present, no camera to settle and
            # no HUD to appear, and it covers the whole surface rather than one
            # target. If it is wrong, selection cannot work and there is a
            # concrete bug; if it is right, the coordinate is not the problem
            # and the search moves downstream -- which is exactly the fork
            # docs/228 could not resolve.
            #
            # 0215376A is the cartridge's touch X, the field docs/224 traced
            # the port's write to and docs/225 enumerated every reader of.
            TX = 0x0215376A
            logp = os.path.join(out, "stderr.log")

            def geom_tm():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-200000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in tail.splitlines() if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                import re as _re
                m = _re.search(r"window=(\d+)x(\d+) guest=(\d+)x(\d+) "
                               r"field=(-?\d+),(-?\d+) (\d+)x(\d+)", ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            def rd16(addr):
                raw = d.cmd("read_mem", cpu=9, addr=addr, len=2)["hex"]
                v = int.from_bytes(bytes.fromhex(raw), "little")
                return v - 65536 if v >= 32768 else v

            for size in SIZES:
                w, h = (int(v) for v in size.split("x"))
                # Resize by taking a capture at that size, which is how every
                # other sweep here changes the window.
                capture(os.path.join(out, "tm-" + size + ".png"), w, h)
                time.sleep(1.5)
                g = geom_tm()
                if not g:
                    print("touchmap: %s -- no geometry line" % size)
                    continue
                ww, wh, gw, gh, fx, fy, fw, fh_ = g
                band = render_band(gw)
                rows = []
                bad = 0
                # WAIT FOR THE VALUE TO CHANGE, rather than sleeping a fixed
                # time and reading whatever is there.
                #
                # The first version slept 120 ms and read once. At the two
                # smallest window sizes that produced three "MISMATCH" rows per
                # sweep whose measured value was EXACTLY the previous row's --
                # 5, 5, 5 and 205, 205, 205 -- and one row at 800x600 that
                # returned 280, which was the last value of the 640x480 sweep
                # before it. Those are stale reads, not mapping faults, and
                # reporting them as mapping faults is precisely the error this
                # project has already made four times with this instrument.
                #
                # Polling until the value moves off the previous reading is
                # self-correcting: it cannot return a stale value, and when two
                # adjacent columns genuinely map to the same coordinate it
                # times out and returns that same value, which is the right
                # answer anyway.
                last = rd16(TX)
                # Eleven columns across the FULL window, so the two extremes
                # are the outermost band pixels rather than the native edges.
                for i in range(11):
                    wx = int(round(fx + (fw - 1) * i / 10.0))
                    wy = fy + fh_ // 2
                    if wx < 0 or wx >= ww:
                        continue
                    # Expected: window -> guest is a plain scale about the
                    # field rect; guest -> native subtracts the band.
                    guest_x = (wx - fx) * gw / float(fw)
                    want = int(guest_x) - band
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=True)
                    got = last
                    polls = 0
                    for polls in range(1, 21):
                        time.sleep(0.05)
                        got = rd16(TX)
                        if got != last:
                            break
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=False)
                    last = got
                    time.sleep(0.05)
                    # Two pixels of slack: the mapping rounds, and the guest
                    # writes the value through a s16 store whose low bit the
                    # port does not control.
                    ok = abs(got - want) <= 2
                    if not ok:
                        bad += 1
                    rows.append("      wx %4d -> native %5d (want %5d) %s "
                                "[%d poll%s]"
                                % (wx, got, want, "ok" if ok else "MISMATCH",
                                   polls, "" if polls == 1 else "s"))
                print("touchmap: %-9s guest %dx%d band %d -- %d of %d "
                      "columns map correctly"
                      % (size, gw, gh, band, len(rows) - bad, len(rows)))
                for r in rows:
                    print(r)

        if args.click_ext:
            logp = os.path.join(out, "stderr.log")
            def hud_count():
                # The in-field HUD only exists while something is selected, so
                # the element count IS the selection detector -- 1 quest button
                # when nothing is selected, six once a hero is.
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-40000:].decode("latin-1", "replace")
                except OSError:
                    return -1
                # hudquads, NOT the count of elements MOVED.
                #
                # docs/212 introduced hudquads precisely as the selection
                # detector: it counts on-screen HUD quads whether or not
                # anchoring moves them, because the in-field HUD does not exist
                # until something is selected. The moved count is zero whenever
                # anchoring is off or the window is narrow, which makes a
                # perfectly good selection read as "nothing selected".
                ls = [l for l in tail.splitlines() if "hudquads=" in l]
                if not ls:
                    return -1
                try:
                    return int(ls[-1].split("hudquads=")[1].split()[0])
                except (IndexError, ValueError):
                    return -1
            def geom():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-40000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in tail.splitlines() if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                # [lb_geom] t=..ms window=WxH guest=GWxGH field=X,Y WxH scale=..
                import re as _re
                # The guest HEIGHT is captured too now. It used to be
                # discarded, and every y below was mapped through 192 --
                # correct only while the surface was always 192 rows tall.
                # Once the window buys rows (docs/215) that puts the click
                # well above where it was aimed, and the CONTROL click then
                # fails, which reads as "selection is broken" when it is the
                # probe that is.
                m = _re.search(r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) (\d+)x(\d+)", ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            # CONTROL FIRST. If the same click method cannot select a unit
            # INSIDE the native band, a failure outside it says nothing about
            # the extension -- it says the probe is broken. Two runs have
            # already been wasted on measurements that were not
            # measuring anything.
            # A BIG WINDOW, AND AIM LEFT OF CENTRE. The Top Screen panel is
            # drawn in the middle of the window and ImGui swallows clicks under
            # it -- the first control clicked at window (486,190), which is
            # exactly the panel's top-left corner, and "not selected" meant
            # "the overlay ate it". Resizing makes the fixed-size panel a
            # smaller share of the window, and a hero left of centre is clear
            # of it.
            capture(os.path.join(stepdir, "resize.png"), 1600, 900)
            time.sleep(1.0)
            ctl = aim_hero(d, logp, stepdir, capture, lo=20, hi=70)
            g0 = geom()
            if ctl is not None and g0:
                gw0, gh0, fx0, fy0, fw0, fh0 = g0
                cx = fx0 + int((ctl + render_band(gw0)) * fw0 / gw0)
                cy = fy0 + int((95 + render_band_top(gh0)) * fh0 / gh0)
                for _ in range(3):
                    d.cmd("frontend_input", action="click", x=cx, y=cy, down=True)
                    time.sleep(0.25)
                    d.cmd("frontend_input", action="click", x=cx, y=cy, down=False)
                    time.sleep(1.2)
                    if hud_count() >= 3:
                        break
                capture(os.path.join(stepdir, "control-inband.png"))
                print("click-ext CONTROL: in-band sx=%d window=(%d,%d) -> hud=%d %s"
                      % (ctl, cx, cy, hud_count(),
                         "SELECTED" if hud_count() >= 3 else "NOT SELECTED"))
            # Deselect before the real test, so a leftover selection cannot be
            # mistaken for a successful out-of-band click.
            # action="click", not "touch". nds_frontend_debug_touch takes GUEST
            # coordinates and refuses anything past 255/191, so this deselect
            # was silently doing nothing -- and the extended click that follows
            # then "succeeded" against a selection the CONTROL had made.
            # Report whether it actually deselected, because a probe whose
            # setup fails silently is worse than no probe.
            # THE DESELECT ICON, because nothing else deselects.
            #
            # Two targets were tried and measured first. The field's bottom
            # centre is where the in-field HUD panel is and took the count from
            # 5 to 6. Empty ground does not deselect either -- in this game a
            # click on the ground with a unit selected is a MOVE ORDER, so the
            # selection survives it, which is why the count stayed at 5.
            #
            # docs/212 puts the deselect icon at native x 4..27, y 168..192.
            # Anchoring moves it to the window's own left edge, by
            # shift = (visible - 256) / 2, so on a 392-wide surface it sits at
            # surface x 44..67. lb_hud_unmap_touch reports a click there at the
            # element's original position, which is the whole point of it.
            if ctl is None or not g0:
                print("click-ext: no control run, skipping the deselect")
                gw0, gh0, fx0, fy0, fw0, fh0 = 256, 192, 0, 0, 256, 192
            dsx = fx0 + int(55 * fw0 / gw0)
            dsy = fy0 + int((180 + render_band_top(gh0)) * fh0 / gh0)
            for _ in range(2):
                d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                      down=True)
                time.sleep(0.2)
                d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                      down=False)
                time.sleep(1.2)
                if hud_count() < 3:
                    break
            print("click-ext: after deselect hud=%d %s"
                  % (hud_count(),
                     "" if hud_count() < 3 else "-- STILL SELECTED, the test "
                     "below cannot distinguish a new selection"))

            sx = aim_hero(d, logp, stepdir, capture)      # left widened column
            g = geom()
            print("click-ext: hero sx=%s geom=%s hud_before=%d" % (sx, g, hud_count()))
            if sx is not None and g:
                gw, gh, fx, fy, fw, fh = g
                extra = render_band(gw)
                surf_x = sx + extra                       # surface column
                win_x = fx + int(surf_x * fw / gw)
                win_y = fy + int((100 + render_band_top(gh)) * fh / gh)
                print("click-ext: surface x=%d -> window (%d,%d)" % (surf_x, win_x, win_y))
                try:
                    d.cmd("deep_trace", on=True)
                except Exception:                      # noqa: BLE001
                    pass
                for attempt in range(3):
                    # RE-AIM EVERY ATTEMPT. A hero walks, so a position read
                    # once and clicked three times tests three different
                    # places, and only the first of them is where the unit was.
                    fresh = hero_pos(logp)
                    if fresh:
                        surf_x = fresh[0] + extra
                        win_x = fx + int(surf_x * fw / gw)
                        win_y = fy + int((fresh[1] + render_band_top(gh)) * fh / gh)
                        print("click-ext:   re-aim sx=%d sy=%d -> surface %d, "
                              "window (%d,%d)"
                              % (fresh[0], fresh[1], surf_x, win_x, win_y))
                    d.cmd("frontend_input", action="click", x=win_x, y=win_y, down=True)
                    time.sleep(0.25)
                    d.cmd("frontend_input", action="click", x=win_x, y=win_y, down=False)
                    time.sleep(1.2)
                    # DRAIN AT THE CLICK, not at the end of the run. The
                    # resting writes put 0x0215376A = 128 seventy times a
                    # drain, so by the end of a run the click's own value has
                    # long been evicted from the 512-entry ring.
                    try:
                        ev = d.cmd("watch", max=8192)["events"]
                    except Exception:                  # noqa: BLE001
                        ev = []
                    vals = sorted({(e["addr"], e["value"]) for e in ev
                                   if e["addr"] in (0x0215376A, 0x0215376C)})
                    if vals:
                        print("click-ext:   touch struct saw %s"
                              % [("%08X" % a2, v) for a2, v in vals][:8])
                    capture(os.path.join(stepdir, "clickext-%02d.png" % attempt))
                    n = hud_count()
                    print("click-ext: attempt %d -> hud elements=%d %s"
                          % (attempt, n, "SELECTED" if n >= 3 else ""))
                    if n >= 3:
                        break

        if args.dragbox:
            # THE MAPPING HERE IS THE CENTRED BAND, deliberately, and that is
            # not an oversight.
            #
            # A previous attempt replaced it with the port's own clamped band
            # from [lb_band], on the reasoning that the renderer uses that and
            # so the probe should too. The in-band CONTROL settled it: with the
            # centred split a tap on a known target selects 3 of 4, and with the
            # clamped one it selects 0 of 4. The port's TOUCH path and its
            # RENDER path use different bands, and a click has to go through the
            # first. Reverted; see the revert of c59be995.
            bx0, by0, bx1, by1 = (int(v) for v in args.dragbox.split(","))
            g = None
            try:
                with open(os.path.join(out, "stderr.log"), "rb") as fh:
                    tail = fh.read()[-40000:].decode("latin-1", "replace")
                ls = [l for l in tail.splitlines() if l.startswith("[lb_geom]")]
                import re as _re
                if ls:
                    m = _re.search(r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) "
                                   r"(\d+)x(\d+)", ls[-1])
                    if m:
                        g = tuple(int(v) for v in m.groups())
            except OSError:
                pass
            if not g:
                print("dragbox: no geometry -- is LB_VERBOSE armed?")
            else:
                gw, gh, fx, fy, fw, fh2 = g
                extra = render_band(gw)
                extra_y = render_band_top(gh)

                def win(nx, ny):
                    return (fx + int((nx + extra) * fw / gw),
                            fy + int((ny + extra_y) * fh2 / gh))

                ax, ay = win(bx0, by0)
                cx2, cy2 = win(bx1, by1)
                print("dragbox: native (%d,%d)->(%d,%d)  window (%d,%d)->(%d,%d)"
                      % (bx0, by0, bx1, by1, ax, ay, cx2, cy2))
                # THE DEFECT IS A SLOWDOWN: the game collapses and chugs
                # during a drag (docs/343 item 1). The fps
                # figure this harness records is taken BEFORE any drag, so it
                # could never see that. Sample the frame counter across the
                # drag itself and after it.
                def _fstats():
                    try:
                        return d.cmd("frontend_stats")
                    except Exception as e:   # the process may be dying
                        return {"error": str(e)}
                _s0 = _fstats()
                _t0 = time.time()
                d.cmd("frontend_input", action="click", x=ax, y=ay, down=True)
                time.sleep(0.25)
                STEPS = 18
                for i in range(1, STEPS + 1):
                    d.cmd("frontend_input", action="moveto",
                          x=ax + (cx2 - ax) * i // STEPS,
                          y=ay + (cy2 - ay) * i // STEPS)
                    time.sleep(0.10)
                    if (i % 2) == 0:
                        capture(os.path.join(
                            out, "dragbox-%d_%d-%02d.png" % (bx0, bx1, i)))
                time.sleep(0.2)
                _s1 = _fstats()
                _t1 = time.time()
                d.cmd("frontend_input", action="click", x=cx2, y=cy2,
                      down=False)
                time.sleep(3.0)
                _s2 = _fstats()
                _t2 = time.time()

                def _rate(a, b, dt):
                    if "error" in a or "error" in b:
                        return "unavailable (%s)" % (a.get("error") or
                                                     b.get("error"))
                    fr = b.get("frames", 0) - a.get("frames", 0)
                    freq = b.get("freq", 0) or 1
                    emu = (b.get("emu_ticks", 0) - a.get("emu_ticks", 0))
                    return "%.1f fps, emu %.2f ms/frame (%d frames in %.2fs)" % (
                        fr / dt, emu / max(fr, 1) / freq * 1000.0, fr, dt)
                _line = ("dragbox timing: DURING drag %s | AFTER release %s"
                         % (_rate(_s0, _s1, _t1 - _t0),
                            _rate(_s1, _s2, _t2 - _t1)))
                print(_line)
                with open(os.path.join(out, "dragbox-timing.txt"), "w") as fh:
                    fh.write(_line + "\n")
                capture(os.path.join(out, "dragbox-%d_%d-after.png"
                                     % (bx0, bx1)))

        if args.hudpan:
            # docs/349: DOES THE CARTRIDGE HIDE THE IN-FIELD HUD WHILE THE
            # CAMERA MOVES? A recorded replay shows the brick HUD absent from
            # the game's own polygon list whenever the camera is stepping and
            # back ~3 frames after it settles. Asked here of the NATIVE field
            # screen, so it runs the same way with --adaptive-widescreen none:
            # select the hero, hold still, pan with the D-pad, stop, and log
            # camera and HUD-icon pixels over time. The icons are sampled at
            # their native place, x 0..30 y 0..45 (the blue and red bricks).
            import re as _re
            hlog = os.path.join(out, "stderr.log")

            def _htail():
                try:
                    with open(hlog, "rb") as fh:
                        fh.seek(0, 2)
                        n = fh.tell()
                        fh.seek(max(0, n - 200000))
                        return fh.read().decode("latin-1", "replace")
                except OSError:
                    return ""

            hero = None
            for _ in range(20):
                for ln in reversed(_htail().splitlines()):
                    m = _re.match(r"\[lb_hero\] sx=(-?\d+) sy=(-?\d+) owner=0",
                                  ln)
                    if m:
                        hero = (int(m.group(1)), int(m.group(2)))
                        break
                if hero:
                    break
                time.sleep(0.5)
            view = struct_ptr(d, 0x02152034)
            g = None
            for ln in reversed(_htail().splitlines()):
                if ln.startswith("[lb_geom]"):
                    m = _re.search(r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) "
                                   r"(\d+)x(\d+)", ln)
                    if m:
                        g = tuple(int(v) for v in m.groups())
                        break
            if not hero or not view or not g:
                print("hudpan: hero %s view %s geometry %s -- cannot run"
                      % (hero, view, g))
            else:
                gw, gh, fx, fy, fw, fh2 = g
                ex = render_band(gw)
                ey = render_band_top(gh)

                def _win(nx, ny):
                    return (fx + int((nx + ex) * fw / gw),
                            fy + int((ny + ey) * fh2 / gh))

                def _cam():
                    b = bytes.fromhex(d.cmd("read_mem", cpu=9,
                                            addr=view + 0x5E, len=4)["hex"])
                    return (int.from_bytes(b[0:2], "little", signed=True),
                            int.from_bytes(b[2:4], "little", signed=True))

                def _hud_px():
                    r = d.cmd("framebuffer", engine="B")
                    rgb = bytes.fromhex(r.get("rgb", ""))
                    w = r.get("w", 256)
                    n = 0
                    for y in range(0, 45):
                        for x in range(0, 30):
                            i = (y * w + x) * 3
                            if i + 2 >= len(rgb):
                                continue
                            rr, gg, bb = rgb[i], rgb[i + 1], rgb[i + 2]
                            if (bb > 170 and rr < 90) or (rr > 170 and gg < 70
                                                          and bb < 70):
                                n += 1
                    return n

                hx, hy = hero
                ax, ay = _win(hx - 24, hy - 28)
                bx, by = _win(hx + 24, hy + 20)
                d.cmd("frontend_input", action="click", x=ax, y=ay, down=False)
                time.sleep(0.12)
                d.cmd("frontend_input", action="click", x=ax, y=ay, down=True)
                time.sleep(0.25)
                for i in range(1, 13):
                    d.cmd("frontend_input", action="moveto",
                          x=ax + (bx - ax) * i // 12, y=ay + (by - ay) * i // 12)
                    time.sleep(0.08)
                time.sleep(0.2)
                d.cmd("frontend_input", action="click", x=bx, y=by, down=False)
                t0 = time.time()
                timeline = []

                def _sample(phase):
                    timeline.append((round(time.time() - t0, 2), phase, _cam(),
                                     _hud_px()))

                while time.time() - t0 < 2.5:
                    _sample("still")
                d.cmd("keys", mask=0x3FF & ~0x10)
                t1 = time.time()
                while time.time() - t1 < 0.8:
                    _sample("PAN")
                d.cmd("keys", mask=0x3FF)
                t2 = time.time()
                while time.time() - t2 < 2.5:
                    _sample("still")
                print("hudpan: hero at native %s; t, phase, camera, HUD px:"
                      % (hero,))
                for row in timeline:
                    print("hudpan:   %5.2f %-5s cam %-11s hud %d" % row)
                with open(os.path.join(out, "hudpan.txt"), "w") as fh:
                    for row in timeline:
                        fh.write("%5.2f %s %s %d\n" % row)

        if args.vsel:
            # docs/349: DRAG-SELECT IN THE VERTICAL BAND.
            #
            # A drag started anywhere, but what it selected did not match the
            # bounding box (docs/347). docs/313 proved the HORIZONTAL case
            # 14 of 14; docs/348 then found the game still read the drag's
            # CURRENT row from a byte clamped to 0..191 (020AF1BC, 020AF310)
            # and widened those reads -- a box reaching above or below the
            # native rows is exactly what that fixed, and no test had put a
            # unit out there. So: park the player's hero in the widened rows,
            # drag boxes around it, and read the game's own selected set after
            # each release. Judged on the hero alone (its pointer), because
            # that is the one unit whose position the log reports every second.
            import re as _re
            vlog = os.path.join(out, "stderr.log")

            def _vsize():
                try:
                    return os.path.getsize(vlog)
                except OSError:
                    return 0

            def _vsince(off):
                try:
                    with open(vlog, "rb") as fh:
                        fh.seek(off)
                        return fh.read().decode("latin-1", "replace")
                except OSError:
                    return ""

            def _vtail(n=200000):
                return _vsince(max(0, _vsize() - n))

            def _vhero():
                for ln in reversed(_vtail().splitlines()):
                    m = _re.match(r"\[lb_hero\] sx=(-?\d+) sy=(-?\d+) "
                                  r"owner=(\d+) ptr=([0-9A-Fa-f]+)", ln)
                    if m and int(m.group(3)) == 0:
                        return (int(m.group(1)), int(m.group(2)),
                                m.group(4).upper())
                return None

            def _vgeom():
                for ln in reversed(_vtail(80000).splitlines()):
                    if ln.startswith("[lb_geom]"):
                        m = _re.search(r"guest=(\d+)x(\d+) field=(-?\d+),"
                                       r"(-?\d+) (\d+)x(\d+)", ln)
                        if m:
                            return tuple(int(v) for v in m.groups())
                return None

            def _vfresh_hero(after_off):
                # A hero line printed AFTER this offset, i.e. after the camera
                # moved -- the logger writes once a second.
                for _ in range(30):
                    txt = _vsince(after_off)
                    hs = [m for m in (_re.match(
                        r"\[lb_hero\] sx=(-?\d+) sy=(-?\d+) owner=0 "
                        r"ptr=([0-9A-Fa-f]+)", l) for l in txt.splitlines())
                        if m]
                    if hs:
                        m = hs[-1]
                        return (int(m.group(1)), int(m.group(2)),
                                m.group(3).upper())
                    time.sleep(0.2)
                return None

            vres = []
            g = _vgeom()
            h = _vhero()
            if not g or not h:
                print("vsel: geometry %s hero %s -- LB_VERBOSE/LB_ENTLOG "
                      "should be armed by this mode" % (g, h))
            else:
                gw, gh, fx, fy, fw, fh2 = g
                top = render_band_top(gh)
                below = gh - 192 - top
                print("vsel: surface %dx%d, %d widened rows above the field, "
                      "%d below; hero at native (%d,%d) ptr %s"
                      % (gw, gh, top, below, h[0], h[1], h[2]))
                if args.vsel_walk:
                    # WALK THE HERO FIRST (docs/349). The player's base sits
                    # at the map's west edge, so the camera runs out of map
                    # before the hero can stand in the RIGHT widened columns.
                    # Select it with a box, order it DX px east with a tap,
                    # wait for it to arrive, then clear the selection with the
                    # game's own deselect icon -- anchored by the port to the
                    # window's bottom-left corner (docs/212: native 4..27 x
                    # 168..192, a surface corner after anchoring) -- so the
                    # first judged box below starts from nothing selected.
                    wx0 = render_band(gw)
                    wy0 = render_band_top(gh)

                    def _w0(nx, ny):
                        return (fx + int((nx + wx0) * fw / gw),
                                fy + int((ny + wy0) * fh2 / gh))

                    def _hover_press(ax, ay):
                        d.cmd("frontend_input", action="click", x=ax, y=ay,
                              down=False)
                        time.sleep(0.12)
                        d.cmd("frontend_input", action="click", x=ax, y=ay,
                              down=True)

                    hx0, hy0 = h[0], h[1]
                    off = _vsize()
                    ax, ay = _w0(hx0 - 24, hy0 - 28)
                    bx, by = _w0(hx0 + 24, hy0 + 20)
                    _hover_press(ax, ay)
                    time.sleep(0.25)
                    for i in range(1, 13):
                        d.cmd("frontend_input", action="moveto",
                              x=ax + (bx - ax) * i // 12,
                              y=ay + (by - ay) * i // 12)
                        time.sleep(0.08)
                    time.sleep(0.2)
                    d.cmd("frontend_input", action="click", x=bx, y=by,
                          down=False)
                    time.sleep(1.2)
                    sel = [l for l in _vsince(off).splitlines()
                           if l.startswith("[lb_sel]")]
                    if not sel or ("ptr=" + h[2]) not in sel[-1]:
                        print("vsel: walk -- the hero was not selected first "
                              "(%s); walking skipped"
                              % (sel[-1][:80] if sel else "set unchanged"))
                    else:
                        tx, ty = _w0(hx0 + args.vsel_walk, hy0)
                        _hover_press(tx, ty)
                        time.sleep(0.2)
                        d.cmd("frontend_input", action="click", x=tx, y=ty,
                              down=False)
                        h2 = None
                        for _ in range(30):
                            h2 = _vfresh_hero(_vsize())
                            if h2 and abs(h2[0] - (hx0 + args.vsel_walk)) < 14:
                                break
                        h = h2 or h
                        dsx = fx + int(12 * fw / gw)
                        dsy = fy + int((gh - 12) * fh2 / gh)
                        off = _vsize()
                        _hover_press(dsx, dsy)
                        time.sleep(0.2)
                        d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                              down=False)
                        time.sleep(1.2)
                        sel = [l for l in _vsince(off).splitlines()
                               if l.startswith("[lb_sel]")]
                        print("vsel: walked the hero to native (%d,%d); the "
                              "deselect icon -> %s"
                              % (h[0], h[1],
                                 sel[-1][:60] if sel else "(set unchanged)"))
                # Park it in a widened band. KEYINPUT is active low. For the
                # ROWS: Down (bit 7) moves the camera down, so the hero's
                # screen y FALLS; Up (bit 6) raises it. For the COLUMNS:
                # Right (bit 4) moves the camera right, so screen x FALLS;
                # Left (bit 5) raises it. `ai` indexes the hero tuple along
                # the axis under test.
                yaxis = args.vsel_axis == "y"
                ai = 1 if yaxis else 0
                dec, inc = (0x80, 0x40) if yaxis else (0x10, 0x20)
                lo_band = top if yaxis else render_band(gw)
                hi_band = below if yaxis else gw - 256 - render_band(gw)
                span = 192 if yaxis else 256
                lo_name, hi_name = ("above", "below") if yaxis else                     ("left of", "right of")
                plans = []
                if args.vsel_target:
                    tlo, thi = (int(v) for v in args.vsel_target.split(","))
                    plans.append((lo_name if thi < 0 else hi_name, tlo, thi,
                                  dec if thi < 0 else inc))
                else:
                    if lo_band >= 60:
                        plans.append((lo_name, -min(90, lo_band - 30), -30,
                                      dec))
                    if hi_band >= 60:
                        plans.append((hi_name, span + 30,
                                      span + min(90, hi_band - 30), inc))
                parked = None
                for name, lo, hi, key in plans:
                    stuck = 0
                    for _step in range(40):
                        h = _vfresh_hero(_vsize()) or h
                        if lo <= h[ai] <= hi:
                            parked = name
                            break
                        want = dec if h[ai] > hi else inc
                        off = _vsize()
                        d.cmd("keys", mask=0x3FF & ~want)
                        time.sleep(0.08 if abs(h[ai] - (lo + hi) // 2) < 40
                                   else 0.2)
                        d.cmd("keys", mask=0x3FF)
                        h2 = _vfresh_hero(off)
                        if h2 and h2[ai] == h[ai]:
                            stuck += 1
                            if stuck >= 3:
                                break              # the map ran out that way
                        else:
                            stuck = 0
                        h = h2 or h
                    if parked:
                        break
                if not parked:
                    print("vsel: could not park the hero in a widened band "
                          "(last native %s %d); nothing judged"
                          % ("y" if yaxis else "x", h[ai]))
                else:
                    time.sleep(1.5)
                    h = _vfresh_hero(_vsize()) or h
                    hx, hy, hptr = h
                    extra = render_band(gw)
                    extra_y = render_band_top(gh)
                    print("vsel: hero parked %s the field at native (%d,%d); "
                          "band %d,%d" % (parked, hx, hy, extra, extra_y))

                    def _win(nx, ny):
                        return (fx + int((nx + extra) * fw / gw),
                                fy + int((ny + extra_y) * fh2 / gh))

                    # Boxes in AXIS coordinates -- `a` along the axis under
                    # test, `c` across it -- then mapped back to (x, y).
                    ha, hc = (hy, hx) if yaxis else (hx, hy)
                    low = parked == lo_name
                    inner = 60 if low else span - 62
                    far0 = ha - 28 if low else inner
                    far1 = inner if low else ha + 20
                    n0, n1 = (30, 150) if low else (span - 172, span - 52)
                    # The miss box sits beside the hero ACROSS the axis, on
                    # the side away from the bottom HUD row.
                    m0, m1 = (hc + 70, hc + 118) if (yaxis or hc < 96) else                         (hc - 118, hc - 70)

                    def _box(name, a0, c0, a1, c1, expect):
                        if yaxis:
                            return (name, c0, a0, c1, a1, expect)
                        return (name, a0, c0, a1, c1, expect)

                    # expect: True = the box contains the hero, False = it
                    # does not, None = a box over empty ground whose effect
                    # on an EXISTING selection is recorded, not judged (the
                    # harness's own notes disagree on whether bare ground
                    # deselects in this game, docs/349).
                    positive = {
                        "cross": _box("cross", far0, hc - 24, far1, hc + 24,
                                      True),
                        "contains": _box("contains", ha - 28, hc - 24,
                                         ha + 20, hc + 24, True),
                        "cross-rev": _box("cross-rev", far1, hc + 24, far0,
                                          hc - 24, True),
                    }
                    boxes = [
                        _box("miss", ha - 28, m0, ha + 20, m1, False),
                        # Native span only, on the hero's side of it.
                        _box("native-only", n0, hc - 24, n1, hc + 24, False),
                    ]
                    for i, key in enumerate(args.vsel_order.split(",")):
                        if i:
                            boxes.append(_box("clear%d" % i, ha - 28, m0,
                                              ha + 20, m1, None))
                        boxes.append(positive[key.strip()])
                    # The set BEFORE the first drag, from the last line.
                    pre_lines = [l for l in _vtail().splitlines()
                                 if l.startswith("[lb_sel]")]
                    has = bool(pre_lines) and ("ptr=" + hptr) in pre_lines[-1]
                    for name, x0, y0, x1, y1, expect in boxes:
                        ax, ay = _win(x0, y0)
                        cx2, cy2 = _win(x1, y1)
                        # HOVER FIRST, THEN PRESS (docs/349). The port drops
                        # a touch while ImGui::GetIO().WantCaptureMouse is
                        # set (lb_overlay_wants_mouse), and ImGui recomputes
                        # that once a frame from where it last saw the mouse.
                        # A release inside the Top Screen overlay panel
                        # followed by a press elsewhere IN THE SAME FRAME --
                        # which only synthetic input can do -- is therefore
                        # swallowed: the first run of this probe lost a
                        # whole drag that way and scored it as the game's
                        # fault. A button-up at the press point moves the
                        # cursor there with no touch (the frontend ignores an
                        # up it never saw go down), and a few frames let
                        # ImGui catch up, as a real mouse's travel would.
                        d.cmd("frontend_input", action="click", x=ax, y=ay,
                              down=False)
                        time.sleep(0.12)
                        off = _vsize()
                        d.cmd("frontend_input", action="click", x=ax, y=ay,
                              down=True)
                        time.sleep(0.25)
                        STEPS = 12
                        for i in range(1, STEPS + 1):
                            d.cmd("frontend_input", action="moveto",
                                  x=ax + (cx2 - ax) * i // STEPS,
                                  y=ay + (cy2 - ay) * i // STEPS)
                            time.sleep(0.08)
                        time.sleep(0.2)
                        capture(os.path.join(out, "vsel-%s-held.png" % name))
                        d.cmd("frontend_input", action="click", x=cx2, y=cy2,
                              down=False)
                        time.sleep(1.5)
                        txt = _vsince(off)
                        sels = [l for l in txt.splitlines()
                                if l.startswith("[lb_sel]")]
                        rects = [l for l in txt.splitlines()
                                 if l.startswith("[lb_selrect] press")]
                        last = sels[-1] if sels else "(set unchanged)"
                        pre = has
                        got = (("ptr=" + hptr) in last) if sels else pre
                        has = got
                        # Judged only where the state BEFORE the drag lets
                        # the outcome be attributed to this box.
                        if expect is None:
                            verdict = "info"
                        elif expect:
                            verdict = ("inconclusive (already selected)"
                                       if pre else "ok" if got else "WRONG")
                        else:
                            verdict = ("ok" if not got else
                                       "inconclusive (already selected)"
                                       if pre else "WRONG")
                        vres.append((name, expect, got, verdict))
                        print("vsel: %-11s box native (%d,%d)-(%d,%d)  "
                              "expect hero %s  -> %s  %s"
                              % (name, x0, y0, x1, y1,
                                 "?" if expect is None else
                                 "IN" if expect else "out",
                                 last[:150], verdict))
                        if rects:
                            # The LAST builder call is the pen-up's tap-style
                            # one (press == current); the rectangle the drag
                            # selected against is the largest one it built.
                            def _area(l):
                                m = _re.search(r"press (-?\d+),(-?\d+) "
                                               r"current (-?\d+),(-?\d+)", l)
                                if not m:
                                    return (-1, l)
                                a, b, c, e = (int(v) for v in m.groups())
                                return (abs(c - a) * abs(e - b), l)
                            print("        game rect: %s"
                                  % max(rects, key=_area)[len("[lb_selrect] "):])
                    h_after = _vfresh_hero(_vsize())
                    print("vsel: hero after the drags %s (moved: %s)"
                          % (h_after, h_after and (h_after[0], h_after[1])
                             != (hx, hy)))
                    judged = [r for r in vres if r[3] in ("ok", "WRONG")]
                    ok = sum(1 for r in judged if r[3] == "ok")
                    print("vsel: %d of %d judged drags as expected (%d not "
                          "judged)" % (ok, len(judged), len(vres) - len(judged)))
            with open(os.path.join(out, "vsel.txt"), "w") as fh:
                for r in vres:
                    fh.write("%s expect=%s got=%s %s\n" % r)

        if args.touchtruth:
            logp = os.path.join(out, "stderr.log")

            def logsize():
                try:
                    return os.path.getsize(logp)
                except OSError:
                    return 0

            def since(off):
                try:
                    with open(logp, "rb") as fh:
                        fh.seek(off)
                        return fh.read().decode("latin-1", "replace")
                except OSError:
                    return ""

            def last_geom():
                import re as _re
                try:
                    with open(logp, "rb") as fh:
                        t = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in t.splitlines() if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                m = _re.search(r"window=(\d+)x(\d+) guest=(\d+)x(\d+) "
                               r"field=(-?\d+),(-?\d+) (\d+)x(\d+)", ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            try:
                from PIL import Image as _Img
                import numpy as _np
            except ImportError:
                _Img = None

            def _runs(mask, need):
                found = []
                for i in range(mask.shape[0]):
                    row = mask[i]
                    if not row.any():
                        continue
                    idx = _np.flatnonzero(_np.diff(_np.concatenate(
                        ([0], row.view(_np.int8), [0]))))
                    for s, e in zip(idx[0::2], idx[1::2]):
                        if e - s >= need:
                            found.append((int(i), int(s), int(e - 1)))
                return found

            def marquee(before_path, after_path):
                """The drawn box, found by SHAPE rather than by change.

                A frame-diff bounding box is not evidence about a rectangle:
                units walk, water animates and the HUD ticks, and the box that
                results is the union of all of it. A marquee is a one-pixel
                outline, so long thin runs find it and find nothing else.
                """
                if _Img is None:
                    return None
                a = _np.asarray(_Img.open(before_path).convert("RGB"),
                                dtype=_np.int16)
                b = _np.asarray(_Img.open(after_path).convert("RGB"),
                                dtype=_np.int16)
                if a.shape != b.shape:
                    return None
                d2 = (_np.abs(b - a).max(axis=2) > 40)
                hh = _runs(d2, 40)
                vv = _runs(d2.T, 24)
                if not vv:
                    return None
                return (min(v[0] for v in vv), max(v[0] for v in vv),
                        min(h[0] for h in hh) if hh else -1,
                        max(h[0] for h in hh) if hh else -1,
                        len(hh), len(vv))

            info = capture_info()
            g = last_geom()
            print("touchtruth: chrome/client=%s  geom=%s" % (info, g))
            if not info or not g:
                print("touchtruth: no chrome or no geometry -- nothing measured")
            elif (info[2], info[3]) != (g[0], g[1]):
                # REFUSE TO MEASURE AGAINST A STALE LOG.
                #
                # The band and the field rect are read from the tail of a log
                # the port writes when it feels like it, and the probe suite
                # resizes the window. A run that measured a 1712-wide window
                # against a [lb_geom] line describing a 3412-wide one produced
                # numbers that looked like a 500 px error in the renderer and
                # were an error in the ruler.
                print("touchtruth: STALE GEOMETRY -- client is %dx%d, last "
                      "[lb_geom] says %dx%d; refusing to measure"
                      % (info[2], info[3], g[0], g[1]))
            else:
                kx, ky, cw, ch = info
                gw, gh, fx, fy, fw, fh2 = g[2], g[3], g[4], g[5], g[6], g[7]
                centred = (gw - 256) // 2

                def bandcam():
                    """The render band and camera as of the last [lb_band]."""
                    import re as _re
                    try:
                        with open(logp, "rb") as fh:
                            t = fh.read()[-40000:].decode("latin-1", "replace")
                    except OSError:
                        return None
                    ls = [l for l in t.splitlines()
                          if l.startswith("[lb_band]")]
                    if not ls:
                        return None
                    m = _re.search(r"left=(-?\d+) .*?cam=(-?\d+),", ls[-1])
                    return (int(m.group(1)), int(m.group(2))) if m else None

                def to_client(surface_x):
                    return fx + int(surface_x * fw / gw)

                # PRESS POINTS CHOSEN SO THE MAP CANNOT SCROLL UNDER THE TEST.
                #
                # The first run of this probe scrolled the camera during two of
                # its three drags and the x column became unreadable: a box
                # anchored in the world moves when the camera does. The scroll
                # was not incidental. The cartridge's edge test reads the touch
                # x through a byte, so an extended x of 288 arrives as 32 --
                # hard against the left edge -- and the map runs away exactly
                # when the probe is measuring. Every offset below has a LOW
                # BYTE near the middle of the screen, so none of them can.
                #
                # Measured FROM THE RENDER BAND, not from the centred split,
                # because the band is where the picture is and the centred
                # split is only equal to it away from the map edges. The four
                # land left of the native columns, inside them, one band right
                # and two bands right -- so a mapping that is correct in the
                # middle and wrong at the edges cannot pass by luck.
                bc_start = bandcam()
                base_left = bc_start[0] if bc_start else centred
                wanted = [e for e in (-100, 76, 332, 588)
                          if 0 <= to_client(base_left + e) < cw - 130]
                print("touchtruth: render band left=%d, offsets %s"
                      % (base_left, wanted))
                for pi, want_ext in enumerate(wanted):
                    px = to_client(base_left + want_ext)
                    py = int(ch * 0.45)
                    qx, qy = px + 120, py + 100
                    base = os.path.join(out, "tt-%d-base.png" % pi)
                    mid = os.path.join(out, "tt-%d-mid.png" % pi)
                    capture(base)
                    bc0 = bandcam()
                    off = logsize()
                    d.cmd("frontend_input", action="click", x=px, y=py,
                          down=True)
                    time.sleep(0.30)
                    for i in range(1, 7):
                        d.cmd("frontend_input", action="moveto",
                              x=px + (qx - px) * i // 6,
                              y=py + (qy - py) * i // 6)
                        time.sleep(0.10)
                    time.sleep(0.15)
                    capture(mid)
                    # WHAT IS DRAWING THE GUEST'S BOX.
                    #
                    # docs/276 left this open: the port's box lands on the
                    # press, but the cartridge's clamped one is still on screen
                    # beside it, and clearing the tilemap it writes -- directly
                    # and through its own row-pointer table -- changed nothing.
                    # So the box is not that tilemap. The second half of
                    # func_0203b7fc converts the corners back to pixels and
                    # indexes a table at 0x02152160, which has the shape of OAM
                    # entries, and OAM is readable. Ask it rather than read more
                    # disassembly.
                    try:
                        _oam = d.cmd("oam", engine="A").get("hex", "")
                    except Exception:                    # noqa: BLE001
                        _oam = ""
                    if _oam and len(_oam) >= 2048:
                        raw = bytes.fromhex(_oam[:2048])
                        live = []
                        for s in range(128):
                            a0 = raw[s * 8] | (raw[s * 8 + 1] << 8)
                            a1 = raw[s * 8 + 2] | (raw[s * 8 + 3] << 8)
                            a2 = raw[s * 8 + 4] | (raw[s * 8 + 5] << 8)
                            # bits 8-9 of attr0: 10b is "disabled" when the
                            # rot/scale flag is clear.
                            if ((a0 >> 8) & 0x3) == 0x2:
                                continue
                            sy = a0 & 0xFF
                            sx = a1 & 0x1FF
                            if sx >= 256:
                                sx -= 512
                            live.append((s, sx, sy, a2 & 0x3FF))
                        print("    OAM: %d live sprite(s)" % len(live))
                        for s, sx, sy, tile in live[:12]:
                            print("      #%-3d native (%4d,%4d) tile %3d"
                                  % (s, sx, sy, tile))
                    bc1 = bandcam()
                    d.cmd("frontend_input", action="click", x=qx, y=qy,
                          down=False)
                    time.sleep(0.6)
                    txt = since(off)
                    maps = [l for l in txt.splitlines()
                            if l.startswith("[lb_map]")]
                    bands = [l for l in txt.splitlines()
                             if l.startswith("[lb_band]")]
                    print("touchtruth: press client (%d,%d) drag to (%d,%d)"
                          % (px, py, qx, qy))
                    if maps:
                        print("    map first: %s" % maps[0].strip())
                        print("    map last : %s" % maps[-1].strip())
                    else:
                        print("    map: NO [lb_map] -- LB_TOUCHLOG not armed, "
                              "or the click never reached the frontend")
                    if bands:
                        print("    band: %s" % bands[-1].strip()[:140])
                    mq = marquee(base, mid)
                    if mq is None:
                        print("    box: NOT FOUND -- no thin runs; the press "
                              "may have landed on the HUD or on a unit")
                        continue
                    bl, br, bt, bb, nh, nv = mq
                    print("    box: capture x %d..%d y %d..%d "
                          "(h-edges %d, v-edges %d)" % (bl, br, bt, bb, nh, nv))
                    ax, ay = bl - kx, bt - ky
                    print("    ANCHOR client (%d,%d)  PRESS client (%d,%d)"
                          "  MISS (%+d,%+d)"
                          % (ax, ay, px, py, ax - px, ay - py))
                    # A MOVING CAMERA MAKES THE X COLUMN MEANINGLESS.
                    #
                    # Say so rather than printing a number: a box anchored in
                    # the world moves when the camera does, and a miss measured
                    # across a scroll is a measurement of the scroll.
                    if bc0 and bc1 and bc0[1] != bc1[1]:
                        print("    CAMERA MOVED %d -> %d during this press -- "
                              "the x miss above is not evidence"
                              % (bc0[1], bc1[1]))
                        continue
                    if not bc1:
                        print("    no [lb_band] -- cannot predict")
                        continue
                    ext = None
                    if maps:
                        try:
                            ext = int(maps[0].split("ext=")[1].split()[0])
                        except (IndexError, ValueError):
                            ext = None
                    if ext is None:
                        continue
                    # TWO PREDICTIONS, ONE OF WHICH MUST BE WRONG.
                    #
                    # "true" is what a correct port draws: the extended
                    # coordinate placed at the band the picture is drawn with.
                    # "byte" is what the cartridge draws if the rectangle is
                    # built from the recogniser's single-byte fields, which
                    # truncate the extended x modulo 256. Printing both, before
                    # looking, is the difference between a test and a fit.
                    left = bc1[0]
                    p_true = to_client(left + ext)
                    p_byte = to_client(left + (ext % 256))
                    print("    predict TRUE band%+d=%d -> client %d  (err %+d)"
                          % (ext, left + ext, p_true, ax - p_true))
                    print("    predict BYTE band+%d=%d -> client %d  (err %+d)"
                          % (ext % 256, left + (ext % 256), p_byte,
                             ax - p_byte))

        if args.edgescroll:
            logp = os.path.join(out, "stderr.log")

            def es_bandcam():
                import re as _re
                try:
                    with open(logp, "rb") as fh:
                        t = fh.read()[-40000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in t.splitlines() if l.startswith("[lb_band]")]
                if not ls:
                    return None
                m = _re.search(r"left=(-?\d+) .*?cam=(-?\d+),(-?\d+)", ls[-1])
                return (int(m.group(1)), int(m.group(2)),
                        int(m.group(3))) if m else None

            def es_geom():
                import re as _re
                try:
                    with open(logp, "rb") as fh:
                        t = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in t.splitlines() if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                m = _re.search(r"window=(\d+)x(\d+) guest=(\d+)x(\d+) "
                               r"field=(-?\d+),(-?\d+) (\d+)x(\d+)", ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            g = es_geom()
            bc = es_bandcam()
            print("edgescroll: geom=%s band/cam=%s" % (g, bc))
            if not g or not bc:
                print("edgescroll: no geometry or no band -- nothing measured")
            else:
                cw, chh = g[0], g[1]
                gw, fx, fw = g[2], g[4], g[6]
                py = int(chh * 0.45)
                print("  %-8s %-8s %-11s %-11s %-7s %s"
                      % ("client x", "ext", "cam before", "cam after",
                         "hold", "drag"))
                for frac in (0.02, 0.12, 0.25, 0.40, 0.50, 0.60, 0.75, 0.88,
                             0.98):
                    px = int((cw - 1) * frac)
                    before = es_bandcam()
                    if not before:
                        continue
                    d.cmd("frontend_input", action="click", x=px, y=py,
                          down=True)
                    # Long enough for a scroll to be unmistakable: the runs in
                    # docs/275 moved the camera 24-32 px inside a single drag.
                    time.sleep(1.6)
                    after = es_bandcam()
                    d.cmd("frontend_input", action="click", x=px, y=py,
                          down=False)
                    time.sleep(0.7)
                    if not after:
                        continue
                    # AND THE SAME POINT DRAGGED, because the two are different
                    # questions and only one of them was ever observed.
                    #
                    # A hold is "does this pixel read as the screen edge". A
                    # drag is "does the game take this for a map pan". The
                    # camera movements that prompted this probe were all seen
                    # during drags, so measuring only the hold would answer the
                    # question nobody asked.
                    dbefore = es_bandcam()
                    d.cmd("frontend_input", action="click", x=px, y=py,
                          down=True)
                    time.sleep(0.2)
                    for i in range(1, 7):
                        d.cmd("frontend_input", action="moveto",
                              x=px + 140 * i // 6, y=py + 60 * i // 6)
                        time.sleep(0.12)
                    dafter = es_bandcam()
                    d.cmd("frontend_input", action="click", x=px + 140,
                          y=py + 60, down=False)
                    time.sleep(0.7)
                    # The surface x this client pixel lands on, then the guest
                    # coordinate it becomes -- printed so a scroll can be read
                    # against where the press actually was, not against a
                    # fraction of the window.
                    sx = int((px - fx) * gw / fw)
                    ext = sx - before[0]
                    print("  %-8d %-8d %-11d %-11d %+-7d %+d"
                          % (px, ext, before[1], after[1],
                             after[1] - before[1],
                             (dafter[1] - dbefore[1])
                             if (dbefore and dafter) else 0))

        if args.drag_select:
            # DRAG-SELECT ACROSS THE BOUNDARY.
            #
            # The feature is click-to-drag to select multiple units.
            # docs/214 patched the cartridge's three drag readers to
            # sign-extend, and func_020aa8e4 normalises the two corners into a
            # rectangle -- so a drag whose START is outside the native band
            # should produce a box with a negative minimum x.
            #
            # The test is a comparison, not a single run: the SAME drag is done
            # from an out-of-band start and from an in-band start that covers
            # the same ground, and the number of units caught is compared. A
            # drag that only ever catches what is inside the band would show
            # the two as equal.
            logp = os.path.join(out, "stderr.log")

            def hudq():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return -1
                ls = [l for l in tail.splitlines() if "hudquads=" in l]
                if not ls:
                    return -1
                try:
                    return int(ls[-1].split("hudquads=")[1].split()[0])
                except (IndexError, ValueError):
                    return -1

            def base_pos6():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1",
                                                        "replace")
                except OSError:
                    return None
                for ln in reversed(tail.splitlines()):
                    if ln.startswith("[lb_base]"):
                        try:
                            return (int(ln.split("sx=")[1].split()[0]),
                                    int(ln.split("sy=")[1].split()[0]))
                        except (IndexError, ValueError):
                            return None
                return None

            def geom5():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in tail.splitlines() if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                import re as _re
                m = _re.search(
                    r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) (\d+)x(\d+)",
                    ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            capture(os.path.join(stepdir, "drag-resize.png"), 1600, 900)
            time.sleep(1.5)
            g = geom5()
            print("drag-select: geom=%s" % (g,))
            if g:
                gw, gh, fx, fy, fw, fh = g
                extra = render_band(gw)

                def win(nx, ny):
                    return (fx + int((nx + extra) * fw / gw),
                            fy + int((ny + render_band_top(gh)) * fh / gh))

                def deselect():
                    dx2, dy2 = win(-999, 0)   # unused; deselect icon below
                    dsx = fx + int(55 * fw / gw)
                    dsy = fy + int((180 + render_band_top(gh)) * fh / gh)
                    for _ in range(3):
                        d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                              down=True)
                        time.sleep(0.15)
                        d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                              down=False)
                        time.sleep(0.9)
                        if hudq() < 3:
                            return True
                    return False

                def drag(nx0, ny0, nx1, ny1, steps=18):
                    # SLOWLY. The guest samples the stylus once a frame, so
                    # a drag pushed faster than that is a jump from one end
                    # to the other as far as the game is concerned -- and a
                    # box that never grows selects nothing.
                    ax, ay = win(nx0, ny0)
                    bx, by = win(nx1, ny1)
                    d.cmd("frontend_input", action="click", x=ax, y=ay,
                          down=True)
                    time.sleep(0.2)
                    for i in range(1, steps + 1):
                        d.cmd("frontend_input", action="moveto",
                              x=ax + (bx - ax) * i // steps,
                              y=ay + (by - ay) * i // steps)
                        time.sleep(0.09)
                    time.sleep(0.2)
                    d.cmd("frontend_input", action="click", x=bx, y=by,
                          down=False)
                    time.sleep(1.6)

                # A BOX AROUND A KNOWN TARGET, not a box around the screen.
                #
                # A full-screen in-band drag caught nothing, and the touch
                # path shows 19 distinct X values sweeping across it -- so
                # the drag reached the guest and there was simply nothing of
                # the player's inside the native band to catch. That is the
                # same "a control that can fail for want of a target"
                # mistake docs/217 and docs/218 both turned on.
                #
                # So the base is parked OUT of band, exactly as docs/218
                # does for clicks, and the box is drawn around it -- wholly
                # outside the native 256 columns. With the extension on it
                # should catch the base; with it off the clamp puts every
                # corner at native x 0 and the box collapses to nothing.
                bp = None
                for _ in range(40):
                    bp = base_pos6()
                    if bp is None:
                        time.sleep(1.0)
                        continue
                    tgt = args.drag_select_at
                    if tgt - 6 <= bp[0] <= tgt + 6:
                        break
                    key = 0x10 if bp[0] > tgt + 6 else 0x20
                    d.cmd("keys", mask=0x3FF & ~key)
                    time.sleep(0.012 if abs(bp[0] - tgt) < 60 else 0.05)
                    d.cmd("keys", mask=0x3FF)
                    time.sleep(1.2)
                bp = base_pos6() or (args.drag_select_at, 15)
                bx0, by0 = bp[0] - 6, bp[1] - 6
                bx1, by1 = bp[0] + 52, bp[1] + 58
                print("drag-select: base at %s, box native (%d,%d)->(%d,%d)"
                      % (bp, bx0, by0, bx1, by1))
                trials = [("OUT box ", bx0, by0, bx1, by1)] * 3
                try:
                    d.cmd("deep_trace", on=True)
                except Exception:                      # noqa: BLE001
                    pass
                for label, x0, y0, x1, y1 in trials:
                    clean = deselect()
                    before = hudq()
                    drag(x0, y0, x1, y1)
                    # Drain AT the drag. The resting writes put
                    # 0x0215376A = 128 a hundred times a second, so by the
                    # end of a run the drag's own path is long evicted --
                    # and "the touch struct only ever saw 128" then reads
                    # as "the drag never reached the guest" when it is only
                    # a ring that wrapped.
                    try:
                        ev = d.cmd("watch", max=8192)["events"]
                    except Exception:                  # noqa: BLE001
                        ev = []
                    xs = sorted({e["value"] for e in ev
                                 if e["addr"] == 0x0215376A})
                    if xs:
                        show = [v - 65536 if v > 32767 else v for v in xs]
                        print("drag-select:   touch X saw %d distinct: %s"
                              % (len(show), show[:12]))
                    # WHERE ELSE the drag's own coordinates land. With
                    # LB_WATCH over all of main RAM and LB_WATCH_VAL set to
                    # the box's minimum x, this names the address the
                    # selection RECTANGLE lives at -- which is the thing
                    # whose consumer is the remaining suspect.
                    other = {}
                    for e in ev:
                        if e["addr"] == 0x0215376A:
                            continue
                        other.setdefault((e["addr"], e["pc"]),
                                         []).append(e["value"])
                    if other:
                        rows2 = sorted(other.items(),
                                       key=lambda kv: -len(kv[1]))[:10]
                        for (a2, pc2), vs in rows2:
                            print("drag-select:   %08X written by pc %08X "
                                  "x%d  %s"
                                  % (a2, pc2, len(vs),
                                     [v - 65536 if v > 32767 else v
                                      for v in vs[:4]]))
                    after = hudq()
                    capture(os.path.join(stepdir,
                                         "drag-%s%d.png"
                                         % (label.strip().replace(">", "_")
                                            .replace("-", ""), x0)))
                    print("drag-select: %s native (%d,%d)->(%d,%d)  hud %d -> %d"
                          "  %s%s"
                          % (label, x0, y0, x1, y1, before, after,
                             "SELECTED" if after >= 3 else "nothing",
                             "" if clean else "  [deselect failed]"))

        if args.click_watch:
            # ONE CLICK, WITH THE WATCH RING DRAINED AT IT.
            #
            # The question this serves is the last open one for widened touch:
            # the out-of-band coordinate reaches the cartridge (docs/224
            # measured it at the slot) and the cartridge does not act on it, so
            # something between the touch struct and the unit hit test rejects
            # it. docs/217 named the suspect: func_020A8794 loads the touch x
            # with LDRH at 020A8812 -- zero-extending, so -38 becomes 65498 --
            # and stashes it at [r5+0x20].
            #
            # Two steps, both driven from here:
            #   LB_WATCH_PC=20A8814    finds the ADDRESS that store writes to
            #   LB_WATCH_RW=r on that address finds what READS it back
            #
            # The target is the BUILDER, which stands still. A hero walks, and
            # aiming at where one used to be is how three earlier attempts at
            # this got confident wrong answers.
            logp = os.path.join(out, "stderr.log")

            # A BIG TAIL. With LB_HUDLOG on, the port writes several lines
            # per frame, so 60 KB of log covers well under a second -- and
            # [lb_base] prints once a second, so it could be absent from the
            # window entirely. Measured: the deep right-band trial reported
            # "no [lb_base]" and was skipped, having simply looked at too
            # little of the file.
            def tl(n=400000):
                try:
                    with open(logp, "rb") as fh:
                        return fh.read()[-n:].decode("latin-1", "replace")
                except OSError:
                    return ""

            def hudq():
                ls = [l for l in tl().splitlines() if "hudquads=" in l]
                try:
                    return int(ls[-1].split("hudquads=")[1].split()[0])
                except (IndexError, ValueError):
                    return -1

            def upos(tag="[lb_builder]", owner=0):
                for ln in reversed(tl().splitlines()):
                    if ln.startswith(tag) and ("owner=%d" % owner) in ln:
                        try:
                            return (int(ln.split("sx=")[1].split()[0]),
                                    int(ln.split("sy=")[1].split()[0]))
                        except (IndexError, ValueError):
                            return None
                return None

            def gm():
                ls = [l for l in tl().splitlines()
                      if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                import re as _re
                m = _re.search(
                    "guest=(" + chr(92) + "d+)x(" + chr(92) + "d+) field=(-?"
                    + chr(92) + "d+),(-?" + chr(92) + "d+) (" + chr(92)
                    + "d+)x(" + chr(92) + "d+)", ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            # How far below the reported entity position the sprite sits.
            #
            # MEASURED, not assumed. It was first set to a flat 20 from reading
            # a capture, and the in-band control then went from 3 of 3 to 0 of 3
            # -- a guess at this number is exactly as good as a guess at the
            # sprite's size, and wrong either way costs a whole run. The sweep
            # below clicks a range of offsets on a parked builder and keeps the
            # middle of whatever range actually selects.
            aim_dy = [0]
            capture(os.path.join(stepdir, "cw-resize.png"), 1600, 900)
            time.sleep(1.5)
            g = gm()
            print("click-watch: geom=%s" % (g,))
            if g:
                gw, gh, fx, fy, fw, fh = g
                extra = render_band(gw)

                def win(nx, ny):
                    return (fx + int((nx + extra) * fw / gw),
                            fy + int((ny + render_band_top(gh)) * fh / gh))

                def deselect():
                    for _ in range(3):
                        tap(d, 16, 180, 0.8)
                        time.sleep(0.9)
                        if hudq() < 3:
                            return True
                    return False

                def park(lo, hi, tries=30, tag="[lb_builder]", owner=0,
                         ylo=20, yhi=170):
                    # VERTICALLY TOO, or the target may not be on screen at all.
                    #
                    # park only ever moved the camera sideways, on the
                    # assumption that anything worth clicking is already at a
                    # sensible height. The opponent's base is not: it sits at
                    # the far end of the map and was reported at sy=671, so the
                    # trial clicked 479 rows below the touch panel and its own
                    # control correctly said the trial could not succeed.
                    #
                    # sy is the entity's y minus the camera's, so pressing DOWN
                    # raises the camera's y and LOWERS sy.
                    for _ in range(tries):
                        p = upos(tag, owner)
                        if p is None:
                            time.sleep(1.2)
                            continue
                        if not (ylo <= p[1] <= yhi):
                            ykey = 0x80 if p[1] > yhi else 0x40
                            ygap = abs(p[1] - (ylo + yhi) // 2)
                            d.cmd("keys", mask=0x3FF & ~ykey)
                            time.sleep(max(0.05, min(0.40,
                                                     ygap / 32.0 * 0.12)))
                            d.cmd("keys", mask=0x3FF)
                            time.sleep(1.1)
                            continue
                        if lo <= p[0] <= hi:
                            return p
                        key = 0x10 if p[0] > hi else 0x20
                        gap = abs(p[0] - (lo + hi) // 2)
                        d.cmd("keys", mask=0x3FF & ~key)
                        time.sleep(max(0.05, min(0.40, gap / 32.0 * 0.12)))
                        d.cmd("keys", mask=0x3FF)
                        time.sleep(1.1)
                    return upos(tag, owner)

                def one(label, lo, hi, attempts=3, tag="[lb_builder]", owner=0):
                    # THREE ATTEMPTS, AND SAY HOW MANY LANDED.
                    #
                    # A single click is not a reliable measurement here: the
                    # in-band control -- the same click, on the same stationary
                    # builder, at the same coordinates -- selected in some runs
                    # and not others. A control that fails at random makes every
                    # band result unreadable, and three of this project's wrong
                    # conclusions came from reading one anyway. Retrying gives
                    # each side the same number of chances and reports the
                    # count, so "2 of 3" and "0 of 3" are distinguishable from
                    # each other and from "1 of 1, maybe".
                    p = park(lo, hi, tag=tag, owner=owner)
                    if p is None:
                        print("click-watch: %s no %s" % (label, tag))
                        return
                    if not (lo <= p[0] <= hi) or not (0 <= p[1] < 192):
                        print("click-watch: %-8s COULD NOT PARK %s in x %d..%d "
                              "(stuck at %d,%d) -- result would not mean what "
                              "the label says"
                              % (label, tag, lo, hi, p[0], p[1]))
                    # ONLY ATTEMPTS THAT STARTED CLEAN ARE COUNTED.
                    #
                    # An attempt whose deselect did not take begins with
                    # something already selected, and this probe's success test
                    # is "the quad count rose from below 3 to at least 3" -- so
                    # a dirty start is scored as a failure even when the click
                    # was fine. That is most of why the in-band control read
                    # 1 of 3 in one run and 3 of 3 in the next with no code
                    # change between them, and a control that varies at random
                    # cannot support any conclusion about the band.
                    q = settled(tag)
                    if q is not None and q != p:
                        print("click-watch: %-8s position settled from %s to %s"
                              % (label, p, q))
                        p = q
                    hits = tried = 0
                    for att in range(attempts * 2):
                        if tried >= attempts:
                            break
                        r = one_click(label, p, att)
                        if r is None:                  # dirty start, not a trial
                            continue
                        tried += 1
                        if r:
                            hits += 1
                    print("click-watch: %-8s builder at %s  SELECTED %d of %d"
                          % (label, p, hits, tried))
                    # THE CLAMP CONTROL.
                    #
                    # A click outside the DS panel is also delivered CLAMPED to
                    # the panel, so a target close to the boundary can be hit
                    # by the clamped copy no matter what the true coordinate
                    # does. That is exactly the artifact that made docs/218
                    # wrong on the left, and the right band is more exposed to
                    # it: the builder parks at native 259, four pixels past the
                    # clamp point of 255.
                    #
                    # So click AT the clamp point, same y, same everything. If
                    # that selects too, the trial above proves nothing and the
                    # target has to move further out. If it does not, the
                    # trial's successes are the true coordinate's doing.
                    if p[0] < 0 or p[0] > 255:
                        cl = 0 if p[0] < 0 else 255
                        ch = 0
                        ct = 0
                        for att in range(attempts * 2):
                            if ct >= attempts:
                                break
                            r = one_click(label + "-clamp", (cl, p[1]), att)
                            if r is None:
                                continue
                            ct += 1
                            if r:
                                ch += 1
                        print("click-watch: %-8s clamp point (%d,%d)  "
                              "SELECTED %d of %d%s"
                              % (label, cl, p[1], ch, attempts,
                                 "   <-- CONFOUNDED, move the target further "
                                 "out" if ch else "   (clean)"))

                def settled(tag, tries=12):
                    """The position, once it has stopped changing.

                    park() presses a key and releases it, but the camera keeps
                    drifting for a moment afterwards, and [lb_base]/[lb_builder]
                    only print about once a second. So the position park()
                    returns can already be stale by the time the click is
                    delivered -- and the click then lands beside the target.
                    That is the likeliest remaining explanation for a control
                    that calibrates successfully and then selects 0 of 3: the
                    calibration click and the trial clicks were aimed at
                    different places, both of them called "where the unit is".

                    Two consecutive readings that agree means the log has caught
                    up with a camera that has stopped.
                    """
                    prev = None
                    for _ in range(tries):
                        q = unit_pos2(tag)
                        if q is not None and q == prev:
                            return q
                        prev = q
                        time.sleep(1.1)
                    return prev

                def one_click(label, p, att):
                    deselect()
                    before = hudq()
                    if before >= 3:
                        # Could not reach a clean state. Not a trial -- say so
                        # rather than scoring it as a failed click.
                        print("click-watch: %-8s #%d DIRTY START (hud %d), "
                              "not counted" % (label, att, before))
                        return None
                    # DEEP TRACE IS OPT-IN, because it changes the answer.
                    #
                    # It slows the guest, and a tap is delivered in wall clock,
                    # so with tracing on a 0.25 s press is a very different
                    # number of guest frames than it is with tracing off. The
                    # in-band control selected 1 of 3 with tracing on and
                    # reliably without it. The left-versus-right comparison
                    # needs no watch at all -- only the reverse-engineering
                    # does -- so it runs clean by default.
                    if args.cw_trace:
                        try:
                            d.cmd("deep_trace", on=True)
                            d.cmd("watch_clear")       # really empty it
                        except Exception:              # noqa: BLE001
                            pass
                    # AIM AT THE SPRITE, NOT AT THE ENTITY'S ORIGIN.
                    #
                    # [lb_builder] reports the ENTITY position, and the sprite
                    # is drawn below it. Measured off --build-flow's capture of
                    # a selected builder: the selection ring spans native y
                    # 78..96, centre about 87, while the entity reports sy=63.
                    # So every click so far has landed roughly 24 px above the
                    # sprite, at or past its top edge -- which is why the
                    # in-band control came back 0, 1, 2 and 3 of 3 in different
                    # runs of identical code. It was marginal, and whether it
                    # landed depended on the idle animation.
                    #
                    # A control that fails at random is worse than no control,
                    # because every band result is then unreadable. Aiming 20 px
                    # down puts the click inside the sprite with margin either
                    # side.
                    wx, wy = win(p[0], p[1] + aim_dy[0])
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=True)
                    # SHORT. A tap is a tap and a long press is something else.
                    #
                    # This held for a full second and the in-band control then
                    # selected 0 of 3, while --drag-unit's click, which holds
                    # 0.2 s, selects reliably on the same builder at the same
                    # coordinates. deep_trace is on here as well, which slows
                    # the guest, so a second of wall clock is a very long press
                    # in guest frames. Matching the hold that is known to work
                    # is the only way the control earns the right to be trusted.
                    time.sleep(0.25)
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=False)
                    time.sleep(1.0)
                    ev = []
                    if args.cw_trace:
                        try:
                            ev = d.cmd("watch", max=4096)["events"]
                        except Exception:              # noqa: BLE001
                            ev = []
                    time.sleep(0.8)
                    after = hudq()
                    seen = {}
                    for e in ev:
                        seen.setdefault((e["addr"], e["pc"]), []).append(
                            e["value"])
                    ok = after >= 3 and before < 3
                    print("click-watch: %-8s #%d at %s  hud %d -> %d  %s "
                          "(%d event(s))"
                          % (label, att, p, before, after,
                             "SELECTED" if ok else "nothing", len(ev)))
                    if att == 0:
                        for (a, pc), vs in sorted(seen.items(),
                                                  key=lambda kv: -len(kv[1])
                                                  )[:8]:
                            print("click-watch:   %08X  pc %08X  x%-3d  %s"
                                  % (a, pc, len(vs),
                                     [v - 65536 if v > 32767 else v
                                      for v in vs[:4]]))
                        # IN ORDER, not aggregated by address.
                        #
                        # The counts above answer "who touches this?", and the
                        # question left is different: the chain of reads that
                        # runs on a click that SELECTS, against the chain that
                        # runs on one that does not. Where the two diverge, the
                        # last PC they share is the one that decided. Summing
                        # per address throws exactly that ordering away.
                        if args.cw_trace and ev:
                            print("click-watch:   --- %d event(s) in order ---"
                                  % len(ev))
                            for e in ev[:40]:
                                v = e["value"]
                                print("click-watch:   pc %08X  %08X = %d"
                                      % (e["pc"], e["addr"],
                                         v - 65536 if v > 32767 else v))
                    return ok

                # FIND THE OFFSET FIRST, in band, where the answer cannot be
                # confounded by the boundary. Every trial after this uses it, so
                # a band failure is about the band rather than about aiming.
                def calibrate():
                    p = park(60, 190,
                             tag=("[lb_base]" if args.cw_base
                                  else "[lb_builder]"))
                    if p is None:
                        print("click-watch: calibrate: no builder")
                        return
                    good = []
                    for dy in range(-12, 37, 6):
                        aim_dy[0] = dy
                        r = one_click("cal", p, dy)
                        if r:
                            good.append(dy)
                    if good:
                        aim_dy[0] = good[len(good) // 2]
                        print("click-watch: calibrated aim dy=%d (selected at "
                              "%s)" % (aim_dy[0], good))
                    else:
                        aim_dy[0] = 0
                        print("click-watch: NO aim offset selects the builder "
                              "in band -- every trial below is unreadable")

                calibrate()
                # TEN ATTEMPTS, NOT THREE, and here is the arithmetic that
                # forced it. Even aimed at the calibrated offset the in-band
                # click selects about one time in three -- attempts 0 and 1
                # failed and 2 succeeded, on a stationary builder at fixed
                # coordinates. The sprite is animated and its clickable band is
                # only about ten native pixels tall, so a share of clicks miss
                # whatever the offset.
                #
                # With a per-click rate near 1/3, "0 of 3 in the band" happens
                # 30% of the time by chance alone. That is not evidence of
                # anything, and three of this project's wrong conclusions came
                # from reading a number that weak. At ten attempts a genuine
                # 1/3 rate shows 0 successes under 2% of the time.
                N = args.cw_attempts
                # THE BASE IS A BIGGER TARGET THAN THE BUILDER.
                #
                # The builder's sprite animates and its clickable band is about
                # ten native pixels tall, so a calibrated offset works in one
                # run and not the next: the sweep has picked dy=+12 and dy=-12
                # on different runs of identical code, and the in-band control
                # has read anywhere from 0 to 3 of 3 as a result. Every band
                # result measured against that control inherits its noise.
                #
                # A base is stationary and roughly 46x57 native pixels. It is a
                # structure rather than a unit, which matters for DRAG-select
                # (docs/222) but not for a single click -- docs/218 confirmed a
                # click selects it and the top screen names it. --cw-base uses
                # it for both trials.
                if args.cw_base:
                    tgt_tag2 = "[lb_base]"
                else:
                    tgt_tag2 = "[lb_builder]"

                def eptr():
                    for ln in reversed(tl().splitlines()):
                        if ln.startswith("[lb_builder]") and "ptr=" in ln:
                            try:
                                return int(ln.split("ptr=")[1].split()[0], 16)
                            except (IndexError, ValueError):
                                return 0
                    return 0

                def sel_trace(label, lo, hi):
                    """What does a click WRITE into the entity it selects?

                    docs/226 ruled out the entity being absent from the
                    gathered list, and docs/224-225 ruled out the coordinate
                    being lost or mis-signed. So the decision is downstream of
                    both, and the way to find it is to watch the entity struct
                    itself: an in-band click that selects must write something
                    in there, and the instruction that does it names the
                    routine. The same watch on an out-of-band click then says
                    whether that routine ran at all.
                    """
                    p = park(lo, hi)
                    if p is None:
                        print("sel-trace: %s no builder" % label)
                        return
                    ep = eptr()
                    if not ep:
                        print("sel-trace: %s no entity pointer reported"
                              % label)
                        return
                    try:
                        d.cmd("watch_range", lo=ep, hi=ep + 0x200)
                        d.cmd("deep_trace", on=True)
                    except Exception as exc:           # noqa: BLE001
                        print("sel-trace: cannot arm: %s" % (exc,))
                        return
                    # KEEP CLICKING UNTIL ONE SELECTS, and trace THAT one.
                    #
                    # The click lands about half the time even aimed at the
                    # calibrated offset, because the sprite is animated. A trace
                    # of a click that did not select is a trace of nothing, and
                    # would be read as "selection writes nothing to the entity".
                    # In the band no click is expected to succeed, so that side
                    # spends its full budget and reports the last attempt --
                    # which is the point of the comparison.
                    ev = []
                    after = 0
                    for _try in range(8):
                        deselect()
                        try:
                            d.cmd("watch_clear")       # really empty it
                        except Exception:              # noqa: BLE001
                            pass
                        wx, wy = win(p[0], p[1] + aim_dy[0])
                        d.cmd("frontend_input", action="click", x=wx, y=wy,
                              down=True)
                        time.sleep(0.25)
                        d.cmd("frontend_input", action="click", x=wx, y=wy,
                              down=False)
                        # DRAIN CLOSE TO THE CLICK. The entity is written by
                        # the game's own update every frame, so a full second
                        # of that evicts the click's own writes from a
                        # 512-entry ring before they can be read.
                        time.sleep(0.3)
                        try:
                            ev = d.cmd("watch", max=4096)["events"]
                        except Exception:              # noqa: BLE001
                            ev = []
                        time.sleep(0.9)
                        after = hudq()
                        if after >= 3:
                            break
                    print("sel-trace: %-7s entity %08X at %s  hud -> %d  %s  "
                          "(%d write(s))"
                          % (label, ep, p, after,
                             "SELECTED" if after >= 3 else "nothing", len(ev)))
                    byoff = {}
                    for e in ev:
                        byoff.setdefault((e["addr"] - ep, e["pc"]), 0)
                        byoff[(e["addr"] - ep, e["pc"])] += 1
                    print("sel-trace:    %d distinct (offset,pc) pair(s)"
                          % len(byoff))
                    for (off, pc), n2 in sorted(byoff.items())[:200]:
                        print("sel-trace:    +%03X  pc %08X  x%d"
                              % (off, pc, n2))

                def sel_find():
                    """Where does the game record WHICH entity is selected?

                    docs/226 named the next link: a per-frame consumer reads
                    `+0x54` as the selection list and `+0x58` as its count, and
                    whatever WRITES those is the decision this investigation has
                    been chasing. What was missing was the object's address,
                    which nothing static names.

                    It can be found from the other end. Selecting a unit must
                    put that unit's ENTITY POINTER somewhere, and the pointer is
                    known -- [lb_builder] prints it. So watch all of main RAM
                    for a write whose VALUE is that pointer and click the unit:
                    whatever address receives it is the selection list.
                    """
                    p = park(60, 190)
                    if p is None:
                        print("sel-find: no builder")
                        return
                    ep = eptr()
                    if not ep:
                        print("sel-find: no entity pointer reported")
                        return
                    print("sel-find: builder at %s, entity %08X" % (p, ep))
                    try:
                        d.cmd("watch_range", lo=0x02000000, hi=0x02400000)
                        d.cmd("watch_val", value=ep)
                        d.cmd("deep_trace", on=True)
                    except Exception as exc:           # noqa: BLE001
                        print("sel-find: cannot arm: %s" % (exc,))
                        return
                    for att in range(8):
                        deselect()
                        try:
                            d.cmd("watch_clear")
                        except Exception:              # noqa: BLE001
                            pass
                        wx, wy = win(p[0], p[1] + aim_dy[0])
                        d.cmd("frontend_input", action="click", x=wx, y=wy,
                              down=True)
                        time.sleep(0.25)
                        d.cmd("frontend_input", action="click", x=wx, y=wy,
                              down=False)
                        time.sleep(0.4)
                        try:
                            ev = d.cmd("watch", max=4096)["events"]
                        except Exception:              # noqa: BLE001
                            ev = []
                        time.sleep(0.8)
                        if hudq() >= 3:
                            print("sel-find: SELECTED on attempt %d, %d write(s)"
                                  " carrying the entity pointer" % (att, len(ev)))
                            seen2 = {}
                            for e in ev:
                                seen2.setdefault((e["addr"], e["pc"]), 0)
                                seen2[(e["addr"], e["pc"])] += 1
                            for (a2, pc2), n2 in sorted(seen2.items()):
                                print("sel-find:   %08X  written by pc %08X  x%d"
                                      % (a2, pc2, n2))
                            # THE RARE ADDRESS IS THE INTERESTING ONE.
                            #
                            # A slot written hundreds of times per selection is
                            # scratch -- a "current object" the engine reuses.
                            # A slot that receives the pointer once or twice is
                            # a RECORD of which unit is selected, which is what
                            # docs/226 asked for.
                            per_addr = {}
                            for (a2, _pc), n2 in seen2.items():
                                per_addr[a2] = per_addr.get(a2, 0) + n2
                            if not per_addr:
                                return
                            target = min(per_addr, key=lambda k: per_addr[k])
                            print("sel-find: rarest address %08X (%d write(s))"
                                  " -- watching around it for the DECISION"
                                  % (target, per_addr[target]))
                            # Now watch the object AROUND it, with no value
                            # filter, and compare an in-band click against an
                            # out-of-band one. Whatever is written here on the
                            # click that selects and not on the click that does
                            # not is the decision this whole investigation has
                            # been looking for.
                            lo2 = max(0x02000000, target - 0x60)
                            for lbl, plo, phi in (("inband", 60, 190),
                                                  ("band", -55, -35)):
                                q = park(plo, phi)
                                if q is None:
                                    continue
                                try:
                                    d.cmd("watch_val", value=-1)
                                    d.cmd("watch_range", lo=lo2,
                                          hi=target + 0x20)
                                except Exception:      # noqa: BLE001
                                    pass
                                deselect()
                                try:
                                    d.cmd("watch_clear")
                                except Exception:      # noqa: BLE001
                                    pass
                                qx, qy = win(q[0], q[1] + aim_dy[0])
                                d.cmd("frontend_input", action="click", x=qx,
                                      y=qy, down=True)
                                time.sleep(0.25)
                                d.cmd("frontend_input", action="click", x=qx,
                                      y=qy, down=False)
                                time.sleep(0.35)
                                try:
                                    ev2 = d.cmd("watch", max=4096)["events"]
                                except Exception:      # noqa: BLE001
                                    ev2 = []
                                time.sleep(0.8)
                                got = hudq() >= 3
                                agg = {}
                                for e in ev2:
                                    k = (e["addr"] - target, e["pc"])
                                    agg[k] = agg.get(k, 0) + 1
                                print("sel-find: %-7s at %s  %s  (%d write(s)"
                                      " near %08X)"
                                      % (lbl, q,
                                         "SELECTED" if got else "nothing",
                                         len(ev2), target))
                                for (off, pc2), n2 in sorted(agg.items())[:20]:
                                    print("sel-find:    %+04X  pc %08X  x%d"
                                          % (off, pc2, n2))
                            return
                    print("sel-find: never selected in 8 attempts")

                if args.sel_find:
                    sel_find()
                if args.sel_trace:
                    sel_trace("inband", 60, 190)
                    sel_trace("left", -55, -35)
                one("inband", 60, 190, attempts=N, tag=tgt_tag2)
                one("left", -55, -35, attempts=N, tag=tgt_tag2)
                # THE RIGHT BAND, WHERE THE COORDINATE IS POSITIVE.
                #
                # This separates two explanations that the left band alone
                # cannot. If the cartridge rejects a NEGATIVE x -- a sign
                # problem somewhere this port has not found -- then the right
                # band, whose coordinates are 256..323 and perfectly positive,
                # will select. If it rejects anything OUTSIDE 0..255 -- a
                # bounds test against the DS panel -- then the right band fails
                # too, and the fix is a different one entirely.
                #
                # Native 285..315 is surface 353..383, inside the 392-wide
                # surface, and FAR from the boundary. That distance is the
                # point: a click the mapping clamps lands at native 255, so a
                # target sitting at 259 would be hit by a clamped click anyway
                # -- the mirror image of the artifact that made docs/218 wrong
                # on the left. At 285 and beyond a clamp cannot reach it.
                if not args.cw_quick:
                    one("right", 285, 315, attempts=N)
                # THE SAME TWO, AGAINST THE BASE.
                #
                # The builder could not be parked past native 259 -- the camera
                # runs out of map first -- and 259 is four pixels past the
                # clamp point, close enough that a clamped click can hit it.
                # Measured: it did, 2 of 3, which makes any right-band result
                # on the builder unreadable.
                #
                # The base is stationary like the builder, and much wider, so
                # its centre can sit at native 300 with its nearest edge still
                # well past where a clamped click lands. It is a structure
                # rather than a unit, which is exactly the objection docs/222
                # raised against using it for DRAG-select -- but this is a
                # single click, and docs/218 confirmed the base selects and
                # names itself on the top screen when it does.
                    one("baseL", -60, -35, tag="[lb_base]",
                        attempts=N)
                # THE OPPONENT'S BASE, which can reach the deep right band.
                #
                # The control comes first and is not optional: if clicking an
                # enemy structure does not raise the HUD quad count at all,
                # then "0 of 3 out in the band" means nothing, and this probe
                # has already been fooled once by a trial that could not
                # succeed (docs/222). Only if the in-band control fires does
                # the deep trial say anything.
                    one("foeIn", 60, 190, tag="[lb_base]",
                        owner=1, attempts=N)
                    one("foeR", 290, 320, tag="[lb_base]",
                        owner=1, attempts=N)

        if args.build_flow:
            # THE GAME'S BUILD SEQUENCE, DRIVEN AUTOMATICALLY.
            #
            #   tap the builder, tap the red brick to view buildings, tap an
            #   icon, place it in the world on a green space, then tap the
            #   checkmark to command the builder to build
            #
            # This is the richest in-field HUD the game shows, and HUD
            # repositioning + scaling has to work on it -- so it is the
            # thing that anchoring has to be tested against, not the two or
            # three quads a bare selection puts up.
            #
            # TAPPED, not clicked through the frontend. docs/222 measured the
            # two against one target in one frame: tap selects, frontend_input
            # does not, and twelve calibrated offsets did not rescue it. Using
            # the delivery that is known to work is what lets this probe say
            # anything about the HUD rather than about the harness.
            #
            # THE BUTTONS ARE NOT GUESSED EITHER. The port prints every
            # on-screen HUD quad's native rect with LB_HUDLOG=1, and those
            # quads ARE the buttons, so the probe reads where they are instead
            # of hard-coding coordinates that any anchoring change would break.
            logp = os.path.join(out, "stderr.log")

            def tail_log(n=120000):
                try:
                    with open(logp, "rb") as fh:
                        return fh.read()[-n:].decode("latin-1", "replace")
                except OSError:
                    return ""

            def last_line(needle, n=8000000):
                # THE LAST MATCH IN THE WHOLE LOG, not in the last 120 KB.
                #
                # A run reported `footprint -1, active -1` at all ten icons --
                # "no [lb_fp] line in the window", which reads exactly like
                # "placement never started" and is not the same thing. The
                # other render probes in this build are enormous ([lb_seen]
                # alone is 32 rows of ~150 characters a frame), so the 120 KB
                # tail can hold under a second of log and the footprint report
                # -- which only prints when its summary CHANGES -- falls out
                # of it. Read far enough back that a silent probe stays
                # readable.
                try:
                    with open(logp, "rb") as fh:
                        blob = fh.read()[-n:].decode("latin-1", "replace")
                except OSError:
                    return None
                hit = None
                for l in blob.splitlines():
                    if needle in l:
                        hit = l
                return hit

            def hudq():
                ls = [l for l in tail_log().splitlines() if "hudquads=" in l]
                if not ls:
                    return -1
                try:
                    return int(ls[-1].split("hudquads=")[1].split()[0])
                except (IndexError, ValueError):
                    return -1

            def hud_rects():
                """The last non-empty HUD block, converted to NATIVE coords.

                The port logs these in SURFACE space -- the widened 392x212 --
                and the cartridge's own hit test works in the 256x192 it still
                believes it has. The two differ by the shift the summary line
                carries, so a rect logged as `x 71..87 y 26..74` on a
                `shift=68,10` surface is native x 3..19 y 16..64: the brick
                sitting against the left edge of the DS screen.

                Tapping the surface number instead lands 68 px into the field.
                Measured: tapping 136,52 selected the BASE, because that is
                simply where the world is at that coordinate, and the capture
                shows the base ringed and the HUD untouched.
                """
                lines = tail_log().splitlines()
                block, shift, found = [], None, False
                for i in range(len(lines) - 1, -1, -1):
                    ln = lines[i]
                    if "corner element(s) moved" not in ln:
                        continue
                    try:
                        n = int(ln.split("hudquads=")[1].split()[0])
                    except (IndexError, ValueError):
                        continue
                    if n <= 0:
                        continue
                    try:
                        sp = ln.split("shift=")[1].split()[0].split(",")
                        shift = (int(sp[0]), int(sp[1]))
                    except (IndexError, ValueError):
                        shift = (0, 0)
                    # AFTER the summary, not before it. lb_hud.cpp prints the
                    # "N corner element(s) moved" line first and then one line
                    # per quad, so a block is a summary followed by its quads.
                    # Collecting backwards found nothing at all.
                    for j in range(i + 1, len(lines)):
                        if not lines[j].startswith("[lb_hud]   x "):
                            break
                        block.append(lines[j])
                    found = True
                    break
                if not found:
                    return []
                out2 = []
                for ln in block:
                    try:
                        xs = ln.split("x ")[1].split(" y ")[0]
                        ys = ln.split(" y ")[1].split(" ->")[0]
                        x0, x1 = (int(v) for v in xs.split(".."))
                        y0, y1 = (int(v) for v in ys.split(".."))
                    except (IndexError, ValueError):
                        continue
                    out2.append((x0 - shift[0], y0 - shift[1],
                                 x1 - shift[0], y1 - shift[1]))
                return out2

            def unit_pos(tag):
                for ln in reversed(tail_log().splitlines()):
                    if ln.startswith(tag) and "owner=0" in ln:
                        try:
                            return (int(ln.split("sx=")[1].split()[0]),
                                    int(ln.split("sy=")[1].split()[0]))
                        except (IndexError, ValueError):
                            return None
                return None

            def aim_unit(tag, lo, hi, tries=24):
                for _ in range(tries):
                    p = unit_pos(tag)
                    if p is None:
                        time.sleep(1.2)
                        continue
                    if lo <= p[0] <= hi:
                        return p
                    key = 0x10 if p[0] > hi else 0x20
                    gap = abs(p[0] - (lo + hi) // 2)
                    d.cmd("keys", mask=0x3FF & ~key)
                    time.sleep(max(0.05, min(0.40, gap / 32.0 * 0.12)))
                    d.cmd("keys", mask=0x3FF)
                    time.sleep(1.1)
                return unit_pos(tag)

            # TAP THROUGH THE WINDOW, NOT THE RAW TOUCH -- docs/238's trap.
            #
            # tap() sends a GUEST coordinate to the debug `touch` command, which
            # is the DS touchscreen path and cannot carry an x outside 0..255.
            # That was fine while aim_unit could always park the unit inside the
            # native strip. It cannot any more: with the widened view the camera
            # clamps at the map edge while the unit sits at surface x -6, in the
            # left band -- legitimate game state, not a fault -- and every tap
            # then went to a wrapped coordinate and selected nothing.
            #
            # Measured before this: `builder at (-6, 63)`, three taps,
            # `hudquads=0`, and the HUD log reporting `of 0 polys` frame after
            # frame. No unit selected means no in-field HUD, which means the
            # brick-animation question could not even be asked.
            #
            # Same mapping click_native uses and docs/230 verified column by
            # column, via this branch's own geom6().
            # Parses the log itself rather than calling geom6(): that helper is
            # defined LATER in this function body, so calling it from here is a
            # NameError at run time even though it is in scope. Same [lb_geom]
            # line, same fields.
            def click6(nx, ny):
                try:
                    with open(os.path.join(out, "stderr.log"), "rb") as fh:
                        tl = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                import re as _re
                ms = _re.findall(r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) "
                                 r"(\d+)x(\d+)", tl)
                g = tuple(int(v) for v in ms[-1]) if ms else None
                if not g:
                    return None
                gw, gh, fx6, fy6, fw6, fh6 = g
                wx = int(round(fx6 + (nx + render_band(gw)) * fw6 / float(gw)))
                wy = int(round(fy6 + (ny + render_band_top(gh)) * fh6
                               / float(gh)))
                d.cmd("frontend_input", action="click", x=wx, y=wy, down=True)
                time.sleep(0.30)
                d.cmd("frontend_input", action="click", x=wx, y=wy, down=False)
                time.sleep(0.30)
                return wx, wy

            who = args.build_flow_unit
            tag = "[lb_builder]" if who == "builder" else "[lb_hero]"
            p = aim_unit(tag, 60, 190)
            if p and not (60 <= p[0] <= 190):
                # SAY SO rather than let the reader assume the aim worked. The
                # camera is against a map edge and cannot bring the unit into
                # the native strip; the click below reaches it anyway.
                print("build-flow: could not park %s in x 60..190 (at %d,%d) "
                      "-- clicking it where it is, through the window" %
                      (who, p[0], p[1]))
            print("build-flow: %s at %s" % (who, p))
            if p:
                # AIM BELOW THE REPORTED POSITION, AND TRY MORE THAN ONCE.
                #
                # Three blind taps at the entity's own sy selected nothing, and
                # --click-watch's calibrate() already records why: [lb_builder]
                # reports the ENTITY origin while the sprite's clickable band
                # sits below it, so that probe sweeps dy from -12 to +36 in
                # sixes and takes the median of the offsets that select. The
                # same values are reused here rather than invented.
                #
                # And it retries, because the same code records the rate: "even
                # aimed at the calibrated offset the in-band click selects about
                # one time in three -- the sprite is animated and its clickable
                # band is only about ten native pixels tall". At 1/3 per click,
                # three attempts miss entirely 30% of the time, which is not a
                # measurement. Two passes over nine offsets is eighteen clicks:
                # a genuine 1/3 target is missed by that well under 1% of the
                # time, so "hudquads=0" afterwards means something.
                hit_dy = None
                for _pass in range(2):
                    if hit_dy is not None:
                        break
                    for dy_aim in (0, 6, 12, 18, 24, -6, -12, 30, 36):
                        cur = unit_pos(tag) or p
                        if click6(cur[0], cur[1] + dy_aim) is None:
                            tap(d, cur[0], cur[1] + dy_aim, 1.0)
                        time.sleep(0.6)
                        if hudq() >= 3:
                            hit_dy = dy_aim
                            break
                if hit_dy is None:
                    print("build-flow: no aim offset selected the %s in 18 "
                          "clicks -- the HUD below is empty for that reason, "
                          "not because there is no HUD to move" % who)
                else:
                    print("build-flow: selected the %s at aim dy=%+d"
                          % (who, hit_dy))
                print("build-flow: after tapping the %s, hudquads=%d"
                      % (who, hudq()))
                capture(os.path.join(stepdir, "bf-1-selected.png"), 1600, 900)
                rects = hud_rects()
                print("build-flow: %d HUD quad(s): %s" % (len(rects), rects))
                # The red brick is the LEFT-hand one of the pair stacked at the
                # side; the checkmark and cancel sit at the bottom. Rather than
                # decide by eye, tap each distinct quad in turn and report what
                # the HUD does -- the one that opens the build list is the one
                # after which the quad count JUMPS.
                seen = []
                # Which quad opened the most. The sweep already reports this
                # in prose ("<-- OPENED SOMETHING"); recording it lets the
                # placement stage below re-open the build list without
                # hard-coding a button position that any anchoring change
                # would break -- the same argument the sweep itself makes.
                best_quad, best_gain = None, 0
                for (x0, y0, x1, y1) in rects:
                    cxn, cyn = (x0 + x1) // 2, (y0 + y1) // 2
                    if not (0 <= cxn < 256 and 0 <= cyn < 192):
                        continue
                    if any(abs(cxn - a) < 6 and abs(cyn - b) < 6
                           for a, b in seen):
                        continue
                    # LEAVE THE BOTTOM ROW ALONE. This sweep's own header says
                    # "the checkmark and cancel sit at the bottom", and the
                    # measurement agrees: tapping (16,180) took hudquads 18 ->
                    # 0, i.e. it deselected outright and cost the run its unit.
                    # Re-discovering that every run also destroys the state the
                    # placement stage below needs, and placement -- not the
                    # sweep -- is the half of the build sequence that was
                    # never reached (docs/307).
                    if cyn >= 150:
                        print("build-flow:   (skipping bottom-row quad at "
                              "(%d,%d): checkmark/cancel, measured to "
                              "deselect)" % (cxn, cyn))
                        continue
                    seen.append((cxn, cyn))
                    # RE-SELECT FIRST. Each tap changes what is selected -- one
                    # of them deselected outright and took the quad count from
                    # 6 to 0 -- so without this every trial after the first
                    # starts from a different state and none of them are
                    # comparable. Re-tapping the unit costs a second and makes
                    # the trials independent.
                    # ...AND RE-SELECT THE WAY THE FIRST SELECTION DOES.
                    #
                    # This used to be a single tap at dy=0. That contradicts
                    # the calibration twenty lines above, which is explicit
                    # that the entity origin is NOT where the sprite is
                    # clickable ("reports the ENTITY origin while the sprite's
                    # clickable band sits below it") and that a click on the
                    # calibrated offset still only lands "about one time in
                    # three". One tap at the wrong offset therefore fails far
                    # more often than it succeeds, and every quad after the
                    # first was skipped -- which is exactly what the whole
                    # build flow did: five "could not re-select" lines and
                    # placement never reached.
                    #
                    # So reuse hit_dy, the offset that demonstrably selected
                    # this unit moments ago, and retry. Same calibration, same
                    # retry argument, applied to the same problem.
                    # RE-AIM FIRST, because the unit does not stay put.
                    #
                    # Measured: after the first selection the builder had
                    # drifted to surface x=-405 -- far off the left of the
                    # native strip -- so every aim offset failed the
                    # `0 <= ax < 256` test and was skipped WITHOUT TAPPING.
                    # Nine "attempts" that never touched the screen look
                    # exactly like nine misses in the log, which is what made
                    # this read as a flaky click rather than a stale position.
                    #
                    # There is exactly ONE player builder (ptr=02307460, the
                    # only owner=0 [lb_builder] in the run), so this is not an
                    # identity mix-up: the camera moved. aim_unit already
                    # exists to pan it back, and it is the same call the first
                    # selection makes.
                    # NO 0..255 GUARD. That guard is what actually broke this,
                    # and the file says so forty lines above: "the camera
                    # clamps at the map edge while the unit sits at surface
                    # x -6, in the left band -- legitimate game state, not a
                    # fault", which is why click6() exists to reach it through
                    # the WINDOW rather than the DS touchscreen.
                    #
                    # Measured: 131 of 137 owner=0 [lb_builder] samples in one
                    # run read sx=-6, and the six that did not are the moment
                    # aim_unit had it parked. So the guard rejected the unit's
                    # normal position and every "attempt" was skipped without
                    # touching anything. The first selection, twenty lines
                    # above, has no such guard -- which is precisely why it
                    # succeeds where this failed. Same call, same absence of a
                    # guard, and tap() stays as the in-range fallback only.
                    order = [0, 6, 12, 18, 24, -6, -12, 30, 36]
                    if hit_dy is not None:
                        order = [hit_dy] + [v for v in order if v != hit_dy]
                    was = hudq()
                    for dy_re in order:
                        if was >= 3:
                            break
                        cur2 = unit_pos(tag)
                        if not cur2:
                            break
                        ax, ay = cur2[0], cur2[1] + dy_re
                        if click6(ax, ay) is None:
                            if not (0 <= ax < 256 and 0 <= ay < 192):
                                continue
                            tap(d, ax, ay, 0.9)
                        time.sleep(0.6)
                        was = hudq()
                    if was < 3:
                        print("build-flow:   (could not re-select before "
                              "(%d,%d) in %d attempts; skipping)"
                              % (cxn, cyn, len(order)))
                        continue
                    tap(d, cxn, cyn, 0.9)
                    time.sleep(1.2)
                    now = hudq()
                    print("build-flow:   tapped HUD quad at (%d,%d)  "
                          "hudquads %d -> %d%s"
                          % (cxn, cyn, was, now,
                             "   <-- OPENED SOMETHING" if now > was else ""))
                    capture(os.path.join(stepdir,
                                         "bf-2-quad-%d-%d.png" % (cxn, cyn)),
                            1600, 900)
                    if now - was > best_gain:
                        best_gain, best_quad = now - was, (cxn, cyn)

                # ── PLACEMENT, which is the half of the build sequence the
                #    sweep above never reached ─────────────────────────────
                #
                #   tap an icon, place it in the world on a green space, then
                #   tap the checkmark
                #
                # The sweep stops once it has reported what each quad does, so
                # the build list opens and is then closed again by the next
                # trial. Nothing ever enters PLACEMENT MODE, and placement is
                # the only state in which BG3 carries a footprint at all
                # (docs/306) -- so the footprint check was not reachable by this
                # harness, in either window size.
                #
                # This re-opens the list by the quad that opened it, taps each
                # newly-appeared quad as a candidate building icon, and then
                # taps in the field. `[lb_fp]` says whether a footprint
                # actually painted, so the stage reports a measurement rather
                # than an assumption that the taps worked.
                def fp_cells():
                    l = last_line("[lb_fp] cells=")
                    if not l:
                        return -1
                    try:
                        return int(l.split("placeable=")[1].split()[0]) + \
                               int(l.split("blocked=")[1].split()[0])
                    except (IndexError, ValueError):
                        return -1

                def fp_active():
                    # THE GAME'S OWN FLAG, view+0x78, not "did anything
                    # paint". A run reached active=1 four times with zero
                    # cells painted, because func_020AA9F8's first tick had
                    # not run yet -- `rec phase=0`, `cur=0,0` -- and that tick
                    # is what computes the footprint. Gating the drag on a
                    # painted cell therefore never entered the drag, and never
                    # ran the tick, which is the loop docs/307 was stuck in.
                    l = last_line(" active=")
                    if not l or "[lb_fp]" not in l:
                        return -1
                    try:
                        return int(l.split(" active=")[1].split()[0])
                    except (IndexError, ValueError):
                        return -1

                def reselect():
                    order = [0, 6, 12, 18, 24, -6, -12, 30, 36]
                    if hit_dy is not None:
                        order = [hit_dy] + [v for v in order if v != hit_dy]
                    # Same as the sweep: click6 through the window, no
                    # 0..255 guard, tap() only as the in-range fallback.
                    for dy_re in order:
                        if hudq() >= 3:
                            return True
                        cur3 = unit_pos(tag)
                        if not cur3:
                            return False
                        ax, ay = cur3[0], cur3[1] + dy_re
                        if click6(ax, ay) is None:
                            if not (0 <= ax < 256 and 0 <= ay < 192):
                                continue
                            tap(d, ax, ay, 0.9)
                        time.sleep(0.6)
                    return hudq() >= 3

                if best_quad is None:
                    print("build-flow: no quad opened anything, so there is "
                          "no build list to place from")
                else:
                    placed = False
                    if not reselect():
                        print("build-flow: could not re-select for placement")
                    else:
                        # DO NOT RE-TAP A LIST THAT IS ALREADY OPEN.
                        #
                        # The sweep ends with it open (hudquads 18), and
                        # tapping the brick again CLOSES it -- measured, the
                        # icon taps then began from 6. That single mistake also
                        # produced the second symptom: hud_rects() takes the
                        # last logged block, so with the list shut it returned
                        # the unit's own six buttons, about 5 px from where the
                        # sweep had seen them, and "not in the before set"
                        # called all six of them new.
                        q_now = hudq()
                        if q_now > len(rects):
                            print("build-flow: build list already open "
                                  "(hudquads=%d); not re-tapping" % q_now)
                        else:
                            tap(d, best_quad[0], best_quad[1], 0.9)
                            time.sleep(1.2)
                        # Identify icons BY POSITION, not by set difference.
                        # The 18-quad block is a horizontal strip of ~24 px
                        # quads on one row (measured: native y 40..64, x 7..223)
                        # sitting apart from the unit's buttons in the top-left
                        # corner. Centres are compared with a 10 px tolerance
                        # because the surface-to-native shift moves them a few
                        # pixels between reads.
                        # WIDTH DISTINGUISHES THEM, and centre alone does not.
                        # The leftmost icon is rect (7,40,32,64) -- centre
                        # (19,52), width 25 -- while the unit's own button is
                        # (12,40,20,64): centre (16,52), width 8. Three pixels
                        # apart, so a centre-only test threw away the first
                        # icon in the strip. Requiring the WIDTH to match too
                        # keeps it.
                        base_c = [((a + c) // 2, (b + e) // 2, c - a)
                                  for (a, b, c, e) in rects]
                        opened = []
                        for r in hud_rects():
                            cx2, cy2 = (r[0] + r[2]) // 2, (r[1] + r[3]) // 2
                            w2 = r[2] - r[0]
                            if any(abs(cx2 - u) <= 10 and abs(cy2 - v) <= 10
                                   and abs(w2 - w) <= 4
                                   for u, v, w in base_c):
                                continue
                            opened.append(r)
                        print("build-flow: build list: %d quad(s), %d of them "
                              "not one of the unit's own buttons"
                              % (len(hud_rects()), len(opened)))
                        # LEFT TO RIGHT. The strip runs native x 7..223 and
                        # the first candidate tried was x=227 -- past its right
                        # end, and it did nothing (hudquads 18 -> 18), which is
                        # what a scroll arrow or an empty slot does. Real icons
                        # sit at roughly x 19,44,67,91,116,139,163,187,211.
                        # SWEEP THE STRIP MORE THAN ONCE.
                        #
                        # Placement entry is not deterministic from here: the
                        # SAME icon at (44,52) gave `18 -> 8` with 22 footprint
                        # cells in one run and `18 -> 7` with nothing in the
                        # next, and a third run entered on (67,52) with the
                        # list already shut. Across seven runs roughly one in
                        # three passes got in. The icon tap is cheap, the
                        # oracle is view+0x78, and three passes turn a 1-in-3
                        # into a 26-in-27, so the sweep repeats instead of
                        # declaring "placement was never entered" off one try.
                        passes = 3
                        # THE COUNT THAT MEANS "THE LIST IS OPEN". The re-open
                        # test below used to be `hudq() <= len(rects)` -- the
                        # unit's own six buttons -- and a run where an icon
                        # tap left SEVEN quads sailed past it: seven is more
                        # than six, so the list was never reopened and the
                        # remaining nine icons all tapped a closed list
                        # (`7 -> 7` nine times). Measure it instead.
                        open_q = len(hud_rects())
                        opened.sort(key=lambda r: (r[0] + r[2]) // 2)
                        # De-duplicate ONCE, here, rather than as the sweep
                        # runs: the old `tried` list did both jobs at once and
                        # so made a second pass impossible.
                        uniq = []
                        for r in opened:
                            cx3 = (r[0] + r[2]) // 2
                            cy3 = (r[1] + r[3]) // 2
                            if not (0 <= cx3 < 256 and 0 <= cy3 < 192):
                                continue
                            if any(abs(cx3 - (q[0] + q[2]) // 2) < 6 and
                                   abs(cy3 - (q[1] + q[3]) // 2) < 6
                                   for q in uniq):
                                continue
                            uniq.append(r)
                        for (ix0, iy0, ix1, iy1) in uniq * passes:
                            icx, icy = (ix0 + ix1) // 2, (iy0 + iy1) // 2
                            # RE-OPEN IF A PREVIOUS FIELD TAP SHUT IT. A tap in
                            # the field is a world click and deselects, so from
                            # the second icon onwards the list was closed and
                            # every tap hit nothing (hudquads 6 -> 7 each time).
                            # A FRESH SELECTION BEFORE EVERY ICON, not only
                            # when the list has closed.
                            #
                            # The one run that ever reached placement did it
                            # on the icon immediately after a reselect: the
                            # same icon tapped into an already-open list did
                            # nothing (`18 -> 18`), and tapped after a
                            # reselect+reopen it gave `18 -> 8` with 22
                            # footprint cells. Two later runs kept the list
                            # open between icons -- the cheaper thing to do --
                            # and neither entered placement at any of ten
                            # icons. So the list being open is not the same
                            # state as the list having just been opened, and
                            # this pays the reselect every time rather than
                            # guessing which one it is.
                            if not reselect():
                                print("build-flow:   lost the selection "
                                      "-- skipping (%d,%d)" % (icx, icy))
                                continue
                            tap(d, best_quad[0], best_quad[1], 0.9)
                            time.sleep(1.2)
                            # TRY IT ANYWAY. A stricter version skipped every
                            # icon in a whole run because the reopened list
                            # came back with 6, 8, 9 or 13 quads instead of
                            # 18 -- the reselect does not land the unit in the
                            # same state twice. The icon tap is cheap and
                            # view+0x78 is the oracle, so a count that does
                            # not match is worth a note, not a skip.
                            if hudq() < open_q - 2:
                                print("build-flow:   list reopened with %d of "
                                      "%d quads -- trying (%d,%d) anyway"
                                      % (hudq(), open_q, icx, icy))
                            q_before = hudq()
                            # RAW TOUCH FIRST for an icon, not click6.
                            #
                            # The build-list icons sit at native y~52, x 7..223
                            # -- entirely inside the DS touch panel -- and
                            # tap()'s own docstring records that raw `touch`
                            # is the delivery that works there, while
                            # frontend_input runs window coordinates through
                            # the enhanced mapping. click6 is the right tool
                            # for a BAND coordinate a raw touch cannot carry;
                            # for a native one it is the longer way round, and
                            # docs/307 has these taps changing nothing at all
                            # (hudquads 18 -> 18) which is what a mis-delivered
                            # tap looks like.
                            if 0 <= icx < 256 and 0 <= icy < 192:
                                tap(d, icx, icy, 0.9)
                            elif click6(icx, icy) is None:
                                tap(d, icx, icy, 0.9)
                            time.sleep(1.0)
                            # CHECK BEFORE TOUCHING THE FIELD. If the icon put
                            # the game into placement, the footprint paints at
                            # once -- and a field tap would be a PLACE, which
                            # ends it. Asking first is what distinguishes
                            # "placement never started" from "placement started
                            # and the next tap finished it".
                            n0 = fp_cells()
                            a0 = fp_active()
                            print("build-flow:   icon (%d,%d): hudquads "
                                  "%d -> %d, footprint %d, active %d"
                                  % (icx, icy, q_before, hudq(), n0, a0))
                            if n0 > 0 or a0 == 1:
                                print("build-flow: PLACEMENT ACTIVE on the "
                                      "icon alone -- footprint %d cell(s), "
                                      "view+0x78 = %d" % (n0, a0))
                                capture(os.path.join(stepdir,
                                                     "bf-3-placing.png"),
                                        1600, 900)
                                placed = True
                                # ── THE ACTUAL FOOTPRINT MEASUREMENT ───────
                                #
                                # Placement is live, so move the cursor into
                                # the BAND and read the footprint there.
                                # docs/286 traced the cull: any cell outside
                                # 32x24 screen cells is dropped by
                                # func_0203CBB8, so a site out here should
                                # paint fewer cells than the same site inside
                                # -- and [lb_fp] reports the box, the counts
                                # and the game's own verdict for each.
                                #
                                # click6 for these, NOT raw touch: a band
                                # coordinate is outside the DS panel and only
                                # the window path can carry it. That is the
                                # mirror of the icon fix above, and the two
                                # together are why both deliveries exist.
                                def fp_state():
                                    l = last_line("[lb_fp] cells=")
                                    return l.strip() if l else "(none)"

                                # MOVE THE CURSOR, DO NOT CLICK IT.
                                #
                                # A click in the field during placement is a
                                # PLACE: it commits the building and ends
                                # placement mode. A first version of this
                                # sweep used click6 and every one of its five
                                # positions reported the same idle footprint,
                                # because the first click had finished the job
                                # before the second position was tried. This
                                # file already warned about that ten lines up.
                                #
                                # `moveto` carries the pointer without a
                                # button, which is what updates the cursor.
                                def move6(nx, ny):
                                    import re as _re
                                    tl = tail_log()
                                    ms = _re.findall(
                                        r"guest=(\d+)x(\d+) field=(-?\d+),"
                                        r"(-?\d+) (\d+)x(\d+)", tl)
                                    if not ms:
                                        return None
                                    gw, gh, fx6, fy6, fw6, fh6 = (
                                        int(v) for v in ms[-1])
                                    wx = int(round(fx6 + (nx + render_band(gw))
                                                   * fw6 / float(gw)))
                                    wy = int(round(fy6 + (ny +
                                                   render_band_top(gh))
                                                   * fh6 / float(gh)))
                                    d.cmd("frontend_input", action="moveto",
                                          x=wx, y=wy)
                                    return wx, wy
                                # ONE CONTINUOUS DRAG, because a DS has no
                                # hover. `moveto` alone left the footprint
                                # identical at all five positions -- the
                                # placement cursor follows the STYLUS, and the
                                # stylus only has a position while it is down.
                                # So press once, visit every position while
                                # held, and release at the end; the release is
                                # the single commit.
                                # PRESS ON THE GHOST, wherever it actually is.
                                #
                                # [lb_fp] now reports the footprint's own box
                                # in buffer cells, and a cell is 8 px, so its
                                # centre in native pixels is arithmetic rather
                                # than a guess. The previous sweep pressed a
                                # fixed (128,96) and the ghost turned out to
                                # be at cells 14,12..19,15 -- native 112..159
                                # by 96..127 -- so that press landed on its
                                # very corner at best.
                                def ghost_centre():
                                    l = last_line("FOOTPRINT ")
                                    if not l:
                                        return None
                                    import re as _re
                                    m = _re.search(
                                        r"FOOTPRINT (-?\d+),(-?\d+)\.\."
                                        r"(-?\d+),(-?\d+)", l)
                                    if not m:
                                        return None
                                    a, b, c, e = (int(v) for v in m.groups())
                                    if c < a or e < b:
                                        return None
                                    return ((a + c + 1) * 4, (b + e + 1) * 4)
                                gc = ghost_centre()
                                print("build-flow:   ghost centre reads %s"
                                      % (gc,))
                                start = gc or (128, 96)
                                seq = (("ghost-start", start[0], start[1]),
                                       ("near-left-edge", 12, start[1]),
                                       ("LEFT BAND", -60, start[1]),
                                       ("near-right-edge", 243, start[1]),
                                       ("RIGHT BAND", 315, start[1]))
                                # GEOMETRY ONLY -- this used to call move6,
                                # which pushes a motion event with
                                # SDL_BUTTON_LMASK already set, so it landed a
                                # pen-down on the ghost's own centre before
                                # the sweep had pressed anything.
                                import re as _re0
                                w0 = (_re0.findall(
                                    r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) "
                                    r"(\d+)x(\d+)", tail_log()) or [None])[-1]
                                if w0 is None:
                                    print("build-flow:   no geometry to map "
                                          "the cursor with")
                                else:
                                    # DO NOT PRESS ON THE GHOST, and do not
                                    # repeat the press.
                                    #
                                    # func_020AA9F8's first tick tests the
                                    # cursor cell against the ghost's own
                                    # rect, [origin, origin+size):
                                    #
                                    #   INSIDE  -> *(0x0214BB84) = 1 and the
                                    #              origin is LEFT ALONE
                                    #   OUTSIDE -> the origin is recomputed
                                    #              from the cursor
                                    #
                                    # The previous sweep computed the ghost's
                                    # own centre and pressed exactly there,
                                    # which is the single press that cannot
                                    # move it. So the press starts clear of
                                    # the footprint and the ghost is dragged
                                    # from there.
                                    #
                                    # And a touch is STICKY, not a one-frame
                                    # pulse: io.cpp's nds_set_touch sets
                                    # g_tsc_x/y plus the pen-down bit and
                                    # returns, and only an explicit down=false
                                    # clears it. A hold is therefore a press
                                    # followed by moves -- the comment this
                                    # replaces claimed a touch lasts one frame
                                    # and that is wrong.
                                    #
                                    # Everything goes through the WINDOW path
                                    # because the bands need it: a raw touch
                                    # is uint16 and cannot carry x = -60, and
                                    # nds_frontend_debug_move pushes a motion
                                    # event with SDL_BUTTON_LMASK already set,
                                    # so a moveto while held is a drag and
                                    # publishes the extended coordinate.
                                    def win_xy(nx, ny):
                                        tl = tail_log()
                                        import re as _re
                                        ms = _re.findall(
                                            r"guest=(\d+)x(\d+) "
                                            r"field=(-?\d+),(-?\d+) "
                                            r"(\d+)x(\d+)", tl)
                                        if not ms:
                                            return None
                                        gw, gh, fx6, fy6, fw6, fh6 = (
                                            int(v) for v in ms[-1])
                                        return (int(round(fx6 +
                                                (nx + render_band(gw))
                                                * fw6 / float(gw))),
                                                int(round(fy6 +
                                                (ny + render_band_top(gh))
                                                * fh6 / float(gh))))

                                    def press(nx, ny, down=True):
                                        w = win_xy(nx, ny)
                                        if w is None:
                                            return None
                                        d.cmd("frontend_input", action="click",
                                              x=w[0], y=w[1], down=down)
                                        return w

                                    def drag_to(nx, ny):
                                        w = win_xy(nx, ny)
                                        if w is None:
                                            return None
                                        d.cmd("frontend_input",
                                              action="moveto", x=w[0], y=w[1])
                                        return w

                                    # Clear of the footprint: the ghost read
                                    # 6 cells wide, so 64 px to its left is
                                    # outside it by a whole building. Fall to
                                    # the right if that would leave the map.
                                    offx = (start[0] - 64
                                            if start[0] - 64 >= 8
                                            else start[0] + 64)
                                    if press(offx, start[1]) is None:
                                        print("build-flow:   no geometry for "
                                              "the press")
                                    else:
                                        time.sleep(0.5)
                                        print("build-flow:   pressed off the "
                                              "ghost at (%d,%d) -> %s"
                                              % (offx, start[1], fp_state()))
                                        for label, bx, by in seq:
                                            drag_to(bx, by)
                                            time.sleep(0.8)
                                            print("build-flow:   dragged to "
                                                  "%-15s -> %s"
                                                  % (label, fp_state()))
                                            capture(os.path.join(
                                                stepdir, "bf-4-%s.png"
                                                % label.replace(" ", "-")),
                                                1600, 900)
                                        # Release back on valid ground, so the
                                        # single commit lands on the map.
                                        drag_to(seq[0][1], seq[0][2])
                                        time.sleep(0.4)
                                        press(seq[0][1], seq[0][2], False)
                                break
                            # NO FIELD TAP. This used to tap three field
                            # positions after an icon that did not paint, on
                            # the theory that placement needed a world click.
                            # docs/318 measured otherwise -- the icon alone
                            # enters placement -- and the field taps were
                            # actively destructive: a world click deselects,
                            # so each one changed the list under the next
                            # icon. Two runs of the same route diverged on
                            # exactly that (`icon (44,52)` gave footprint 22
                            # in one and 0 in the next), which is a harness
                            # fault reading as a game one.
                            #
                            # An icon that does not paint is simply not a
                            # building icon; move to the next one with the
                            # list left as it was.
                            continue
                    if not placed:
                        print("build-flow: no footprint painted -- placement "
                              "was never entered (see [lb_fp] in stderr.log)")

        if args.drag_unit or args.drag_boundary:
            # DRAG-SELECT, WITH A TARGET THAT CAN ACTUALLY BE DRAG-SELECTED.
            #
            # --drag-select draws its box around the player's BASE, because the
            # base is a stationary target and stationary targets make a probe
            # reproducible. But a drag box in an RTS selects UNITS, and a base
            # is a structure -- so a run that catches nothing is exactly what a
            # correct game would do, and the probe cannot tell that apart from
            # a broken one. That is "a control that can fail for want of a
            # target", the same mistake docs/217 and docs/218 each turned on.
            #
            # This drags around the player's HERO instead, and does it TWICE in
            # one run at one camera height:
            #
            #   control:  hero parked INSIDE the native 256 columns
            #   test:     hero parked OUTSIDE them, in the widened band
            #
            # The control answers "can this probe ever succeed" before the test
            # is allowed to mean anything. If the control catches the hero and
            # the test does not, the boundary is the cause and nothing else is.
            # If neither catches it, the probe is wrong and the band is not
            # implicated at all.
            logp = os.path.join(out, "stderr.log")

            def hudq():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return -1
                ls = [l for l in tail.splitlines() if "hudquads=" in l]
                if not ls:
                    return -1
                try:
                    return int(ls[-1].split("hudquads=")[1].split()[0])
                except (IndexError, ValueError):
                    return -1

            def geom6():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in tail.splitlines()
                      if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                import re as _re
                m = _re.search(
                    "guest=(" + chr(92) + "d+)x(" + chr(92) + "d+) field=(-?"
                    + chr(92) + "d+),(-?" + chr(92) + "d+) (" + chr(92)
                    + "d+)x(" + chr(92) + "d+)", ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            capture(os.path.join(stepdir, "dragunit-resize.png"), 1600, 900)
            time.sleep(1.5)
            g = geom6()
            print("drag-unit: geom=%s" % (g,))
            if g:
                gw, gh, fx, fy, fw, fh = g
                extra = render_band(gw)

                def win(nx, ny):
                    return (fx + int((nx + extra) * fw / gw),
                            fy + int((ny + render_band_top(gh)) * fh / gh))

                def deselect():
                    # TAP THE CANCEL ICON WHERE THE GAME THINKS IT IS.
                    #
                    # This used to click surface x 55 through the frontend,
                    # which is neither of the two places that icon exists: the
                    # cartridge's hit test has it at native 4..28 (surface
                    # 72..96) and this port DRAWS it at surface 4..28 after
                    # anchoring. 55 falls between them and hits neither, so
                    # every deselect silently failed -- and the calibration
                    # loop skips an offset whenever the deselect leaves
                    # something selected, so all twelve were skipped and the
                    # run reported "no offset selects" without having tried
                    # one.
                    #
                    # --build-flow measured the icon at native (4,168)-(28,192)
                    # and tapping its centre took the quad count from 7 to 0.
                    # tap() takes guest coordinates and bypasses the unmapper,
                    # so the ORIGINAL position is the right one to aim at.
                    for _ in range(3):
                        tap(d, 16, 180, 0.8)
                        time.sleep(0.9)
                        if hudq() < 3:
                            return True
                    return False

                def drag(nx0, ny0, nx1, ny1, steps=18):
                    # Slowly: the guest samples the stylus once a frame, so a
                    # drag pushed faster is a jump from one end to the other
                    # and the box never grows.
                    ax, ay = win(nx0, ny0)
                    bx, by = win(nx1, ny1)
                    d.cmd("frontend_input", action="click", x=ax, y=ay,
                          down=True)
                    time.sleep(0.2)
                    for i in range(1, steps + 1):
                        d.cmd("frontend_input", action="moveto",
                              x=ax + (bx - ax) * i // steps,
                              y=ay + (by - ay) * i // steps)
                        time.sleep(0.09)
                    time.sleep(0.2)
                    d.cmd("frontend_input", action="click", x=bx, y=by,
                          down=False)
                    time.sleep(1.6)

                # THE TARGET IS THE BUILDER, NOT THE HERO.
                #
                # A hero WALKS. One run had the control drag select cleanly and
                # the very next run, same build, had it select nothing -- the
                # difference being where the hero had wandered to between the
                # position being read and the drag being made. docs/217 named
                # this exact confound and it is still the easiest way to get a
                # confident wrong answer out of this probe.
                #
                # The builder stands still until it is given a job: --build-flow
                # found it at (123,63) in every run across many minutes. It is
                # also a UNIT, which the base is not, so it can answer a
                # question about box-selection that a structure cannot.
                def unit_pos2(tag):
                    try:
                        with open(logp, "rb") as fh:
                            t = fh.read()[-40000:].decode("latin-1", "replace")
                    except OSError:
                        return None
                    for ln in reversed(t.splitlines()):
                        if ln.startswith(tag) and "owner=0" in ln:
                            try:
                                return (int(ln.split("sx=")[1].split()[0]),
                                        int(ln.split("sy=")[1].split()[0]))
                            except (IndexError, ValueError):
                                return None
                    return None

                def aim_tag(tag, lo, hi, tries=26):
                    for _ in range(tries):
                        p = unit_pos2(tag)
                        if p is None:
                            time.sleep(1.2)
                            continue
                        if lo <= p[0] <= hi:
                            return p
                        key = 0x10 if p[0] > hi else 0x20
                        gap = abs(p[0] - (lo + hi) // 2)
                        d.cmd("keys", mask=0x3FF & ~key)
                        time.sleep(max(0.05, min(0.40, gap / 32.0 * 0.12)))
                        d.cmd("keys", mask=0x3FF)
                        time.sleep(1.1)
                    return unit_pos2(tag)

                tgt_tag = ("[lb_builder]" if args.drag_unit_target == "builder"
                           else "[lb_hero]")

                def trial(name, lo, hi):
                    hp = aim_tag(tgt_tag, lo, hi)
                    if hp is None:
                        print("drag-unit: %s no %s reported"
                              % (name, tgt_tag))
                        return
                    hx, hy = hp
                    clean = deselect()
                    before = hudq()
                    # A generous box. The reported position is the entity's
                    # own, not the sprite's clickable centre -- docs/218 --
                    # so the box is made wide enough that being wrong about
                    # the offset by a dozen pixels cannot decide the result.
                    drag(hx - 40, hy - 40, hx + 40, hy + 40)
                    after = hudq()
                    capture(os.path.join(stepdir, "dragunit-%s.png" % name))
                    print("drag-unit: %-8s hero at (%d,%d) box (%d,%d)->"
                          "(%d,%d)  hud %d -> %d  %s%s"
                          % (name, hx, hy, hx - 40, hy - 40, hx + 40, hy + 40,
                             before, after,
                             "SELECTED" if after >= 3 else "nothing",
                             "" if clean else "  [deselect failed]"))

                    # THE SAME TARGET, CLICKED RATHER THAN DRAGGED.
                    #
                    # Without this a failed drag has two explanations that look
                    # identical from outside: the drag did not reach the guest,
                    # or it reached it and this game does not box-select. A
                    # plain click at the same coordinates settles it -- if the
                    # click selects and the drag does not, delivery is fine and
                    # the question is about drag semantics; if neither does,
                    # the probe is not hitting the hero at all and nothing here
                    # says anything about dragging.
                    #
                    # AND IT DOES NOT HIT IT. Measured: a click at the hero's
                    # reported (sx, sy) selects nothing, in band or out. The
                    # position the port prints is the ENTITY'S, and the sprite
                    # is drawn at an offset from it -- docs/218 saw the same
                    # thing on the base, where the clicks that worked were at
                    # sx+12 and sx+36 rather than at sx.
                    #
                    # So calibrate instead of guessing: click a grid of offsets
                    # around the reported position and report every one that
                    # selects. Done once, in band, where the answer cannot be
                    # confounded by the boundary; the offsets it finds are a
                    # property of the sprite, so they carry to the band.
                    # THE CLICKABLE POINT IS NOT THE REPORTED POINT.
                    #
                    # Measured, one target and one frame:
                    #
                    #   TAP      at the hero's (sx, sy)   hud 0 -> 5 SELECTED
                    #   FRONTEND at the same (sx, sy)     hud 0 -> 0 nothing
                    #
                    # tap() goes through the debug server's `touch`, which
                    # takes GUEST coordinates; frontend_input takes WINDOW
                    # pixels and the frontend converts them back. The two do
                    # not land on the same guest pixel, which is why
                    # --click-sweep has always carried a candidate list of
                    # offsets and calibrated one before trusting any result.
                    #
                    # frontend_input is the only delivery that reaches OUTSIDE
                    # the native columns at all (docs/217: the `touch` command
                    # refuses x >= 256, and negative x has nowhere to go in an
                    # unsigned guest coordinate), so the band test has to use
                    # it -- which means the offset has to be calibrated, in
                    # band, with the same delivery. That is what this does, and
                    # the offset it finds is then reused out of band.
                    # WHAT DOES EACH DELIVERY ACTUALLY PUT IN FRONT OF THE
                    # GAME? Read it, rather than reasoning about it.
                    #
                    # kTouchBase is 0x02153760 and the port publishes the x the
                    # patched cartridge reads at +0x18. +0x0A and +0x0C are the
                    # struct's own current X and Y. Sampling all three WHILE THE
                    # PEN IS HELD, once per delivery, at the same target, says
                    # whether tap and frontend_input differ in what arrives or
                    # only in what happens afterwards.
                    def touch_regs():
                        try:
                            def rd(a):
                                v = int.from_bytes(bytes.fromhex(
                                    d.cmd("read_mem", cpu=9, addr=a,
                                          len=2)["hex"]), "little")
                                return v - 65536 if v > 32767 else v
                            return (rd(0x02153778), rd(0x0215376A),
                                    rd(0x0215376C))
                        except Exception as exc:       # noqa: BLE001
                            return ("err", str(exc)[:40], "")
                    deselect()
                    tapped = None
                    if 0 <= hx < 256 and 0 <= hy < 192:
                        d.cmd("touch", x=hx, y=hy, down=True)
                        time.sleep(0.35)
                        tapped = touch_regs()
                        d.cmd("touch", x=hx, y=hy, down=False)
                        time.sleep(0.8)
                    wx3, wy3 = win(hx, hy)
                    d.cmd("frontend_input", action="click", x=wx3, y=wy3,
                          down=True)
                    time.sleep(0.35)
                    fronted = touch_regs()
                    d.cmd("frontend_input", action="click", x=wx3, y=wy3,
                          down=False)
                    time.sleep(0.8)
                    print("drag-unit: %-8s delivered (slot,+0A,+0C)  "
                          "tap=%s  frontend=%s" % (name, tapped, fronted))

                    if name == "control":
                        for ox, oy in [(0, 0), (16, 16), (24, 24), (16, 32),
                                       (32, 24), (8, 40), (24, 40), (0, 24),
                                       (32, 8), (8, 8), (40, 40), (16, 48)]:
                            deselect()
                            if hudq() >= 3:
                                continue
                            cx2, cy2 = win(hx + ox, hy + oy)
                            # PRINT WHAT THE SERVER SAYS. A click that is
                            # refused and a click that lands on empty ground
                            # both show up as "nothing selected", and this
                            # probe has already spent two runs unable to tell
                            # them apart.
                            r1 = d.cmd("frontend_input", action="click",
                                       x=cx2, y=cy2, down=True)
                            # HELD FOR A FULL SECOND, like tap(). tap holds
                            # 1.0 s and selects; this held 0.2 s and did not,
                            # and hold duration is one of the few things still
                            # differing between the two deliveries.
                            time.sleep(1.0)
                            r2 = d.cmd("frontend_input", action="click",
                                       x=cx2, y=cy2, down=False)
                            if (ox, oy) == (0, 0):
                                print("drag-unit:   frontend_input at window "
                                      "(%d,%d) -> %s / %s"
                                      % (cx2, cy2, r1, r2))
                            time.sleep(1.2)
                            if hudq() >= 3:
                                calib.append((ox, oy))
                                print("drag-unit: calibrated frontend offset "
                                      "%s in band" % ((ox, oy),))
                                break
                        if not calib:
                            print("drag-unit: NO frontend offset selects the "
                                  "hero in band -- the band result below "
                                  "cannot mean anything")
                    if calib:
                        ox, oy = calib[0]
                        deselect()
                        b2 = hudq()
                        cx2, cy2 = win(hx + ox, hy + oy)
                        d.cmd("frontend_input", action="click", x=cx2, y=cy2,
                              down=True)
                        time.sleep(0.2)
                        d.cmd("frontend_input", action="click", x=cx2, y=cy2,
                              down=False)
                        time.sleep(1.4)
                        a2 = hudq()
                        print("drag-unit: %-8s CLICK+%s at (%d,%d)  hud %d -> "
                              "%d  %s"
                              % (name, (ox, oy), hx, hy, b2, a2,
                                 "SELECTED" if a2 >= 3 else "nothing"))

                calib = []
                if args.drag_boundary:
                    # PAIRED TRIALS: the same target, the same box size, and
                    # the only difference is whether the box spans x=0.
                    #
                    # This exists because --drag-unit's control is not one on
                    # this route. It wants the unit in x 70..170 so its +/-40
                    # box stays native; the hero sits at sx=18 (89 of its
                    # samples in one run), so aim_tag never gets there and BOTH
                    # of its trials end up in the band. The probe says so
                    # itself -- "the band result below cannot mean anything" --
                    # and then prints a band result anyway.
                    #
                    # With the target at sx=18 a +/-40 box necessarily crosses
                    # the boundary, so instead of moving the target, move the
                    # BOX: one centred (crosses 0), one shifted right by 24 so
                    # it starts at +2 and is entirely native. Both are 80 wide
                    # and both contain the target, so a difference between them
                    # is the crossing and nothing else.
                    #
                    # Alternated and repeated, because a single drag is not
                    # evidence: this file's own calibration records in-band
                    # clicks landing about one time in three.
                    # THE PAIRING IS ONLY POSSIBLE IF THE TARGET IS NATIVE
                    # ENOUGH, and the probe must CHECK that rather than assume
                    # it. A first version fixed the shift at +24 on the
                    # strength of an sx=18 reading taken from a DIFFERENT run;
                    # the hero moves, this run had it at -6 and -14, and every
                    # box -- including all three labelled "native" -- crossed
                    # x=0. The result line compared two crossing boxes and
                    # meant nothing.
                    #
                    # A box of half-width 40 around a target at sx can avoid
                    # the boundary only if sx >= 40. Below that the target is
                    # itself in or beside the band and no box containing it is
                    # entirely native, so the comparison cannot be made.
                    HALF = 40
                    res = {"cross": [0, 0], "native": [0, 0]}
                    # camera drift: [trials, trials whose camera moved]
                    camdrift = {"cross": [0, 0], "native": [0, 0]}
                    skipped = 0
                    # AIM AT THE WINDOW WHERE BOTH ARMS ARE VALID, rather than
                    # hoping the target drifts into it. Both a crossing box and
                    # a non-crossing box can contain the target only while
                    # 0 <= sx < HALF, so that is what to aim for -- not the
                    # 70..170 --drag-unit wants, which is unreachable here and
                    # is why its control was never a control.
                    parked = aim_tag(tgt_tag, 5, HALF - 5)
                    print("drag-boundary: parked %s at %s (want sx 5..%d)"
                          % (tgt_tag, parked, HALF - 5))
                    for rep in range(9):
                        order = (("cross", True), ("native", False))
                        if rep % 2:
                            order = tuple(reversed(order))
                        for kind, want_cross in order:
                            hp = unit_pos2(tgt_tag)
                            if hp is None:
                                print("drag-boundary: no %s reported" % tgt_tag)
                                continue
                            hx, hy = hp
                            # CAMERA DRIFT'S SURVIVING HALF. docs/277 measured a
                            # HOLD scrolling nothing at nine points, so the
                            # reported auto-pan -- if it is real -- belongs to
                            # DRAGS. Every trial here already performs one, and
                            # the two arms differ only in whether the box
                            # crosses the native edge, so recording the camera
                            # either side turns this probe into that test for
                            # free: drift on the crossing arm and none on the
                            # native arm would be the port's doing.
                            def db_cam():
                                try:
                                    with open(logp, "rb") as fh:
                                        t = fh.read()[-60000:].decode(
                                            "latin-1", "replace")
                                except OSError:
                                    return None
                                for ln in reversed(t.splitlines()):
                                    m = re.search(
                                        r"cam=\(?(-?\d+),\s*(-?\d+)", ln)
                                    if m:
                                        return (int(m.group(1)),
                                                int(m.group(2)))
                                return None
                            cam_before = db_cam()
                            if want_cross:
                                x0, x1 = hx - HALF, hx + HALF
                            else:
                                # Slide right just far enough to clear x=0,
                                # keeping the target inside the box.
                                x0 = max(0, hx - HALF)
                                x1 = x0 + 2 * HALF
                                if x0 > hx or x1 < hx:
                                    print("drag-boundary: rep%d native "
                                          "IMPOSSIBLE -- target sx=%d is under "
                                          "the half-width %d, so no box "
                                          "containing it is entirely native; "
                                          "skipping rather than mislabelling"
                                          % (rep, hx, HALF))
                                    skipped += 1
                                    continue
                            deselect()
                            before = hudq()
                            drag(x0, hy - 40, x1, hy + 40)
                            after = hudq()
                            ok = after >= 3
                            crosses = x0 < 0 <= x1
                            # LABEL FROM THE GEOMETRY, NOT FROM THE INTENT.
                            if crosses != want_cross:
                                print("drag-boundary: rep%d %-6s DISCARDED -- "
                                      "box x %d..%d %s what the arm intends"
                                      % (rep, kind, x0, x1,
                                         "crosses x=0, which is not"
                                         if crosses else "avoids x=0, unlike"))
                                skipped += 1
                                continue
                            res[kind][0] += 1
                            res[kind][1] += 1 if ok else 0
                            cam_after = db_cam()
                            drift = ""
                            if cam_before and cam_after:
                                ddx = cam_after[0] - cam_before[0]
                                ddy = cam_after[1] - cam_before[1]
                                camdrift[kind][0] += 1
                                if ddx or ddy:
                                    camdrift[kind][1] += 1
                                drift = ("  cam %+d,%+d" % (ddx, ddy)
                                         if (ddx or ddy) else "  cam still")
                            print("drag-boundary: rep%d %-6s target sx=%d  box "
                                  "x %d..%d %s  hud %d -> %d  %s%s"
                                  % (rep, kind, hx, x0, x1,
                                     "CROSSES x=0" if crosses else "one side",
                                     before, after,
                                     "SELECTED" if ok else "nothing", drift))
                            capture(os.path.join(
                                stepdir, "dragb-%d-%s.png" % (rep, kind)))
                    if camdrift["cross"][0] or camdrift["native"][0]:
                        print("drag-boundary: CAMERA DRIFT (item 8)  "
                              "crossing %d/%d drags moved the camera, "
                              "entirely-native %d/%d"
                              % (camdrift["cross"][1], camdrift["cross"][0],
                                 camdrift["native"][1],
                                 camdrift["native"][0]))
                    if not res["cross"][0] or not res["native"][0]:
                        print("drag-boundary: NO COMPARISON POSSIBLE -- "
                              "crossing n=%d, entirely-native n=%d, %d trial(s)"
                              " skipped. The target never sat far enough "
                              "inside the native window for a box containing "
                              "it to avoid the boundary, so item 5 is "
                              "UNANSWERED by this run rather than answered "
                              "either way."
                              % (res["cross"][0], res["native"][0], skipped))
                    else:
                        print("drag-boundary: RESULT  crossing %d/%d selected, "
                              "entirely-native %d/%d selected  (%d skipped)"
                              % (res["cross"][1], res["cross"][0],
                                 res["native"][1], res["native"][0], skipped))
                trial("control", 70, 170)
                # SAFELY INSIDE THE DRAWN BAND.
                #
                # -80 was not. The surface is 392 wide with a shift of 68, so
                # it covers native -68..323 and nothing outside that is drawn
                # or clickable. A target at native -69 maps to surface -1,
                # window x -5, which the mapping correctly treats as outside
                # the field and turns into a RELEASE -- so the click was never
                # delivered and the trial was measuring the deselect before it.
                # -55..-35 is comfortably inside, and still far enough out that
                # a clamped click (which lands at native 0) cannot reach it.
                trial("band", -55, -35)

        if args.click_halves:
            # ONE TARGET, ONE CAMERA POSITION, TWO CLICKS.
            #
            # --click-sweep pans between rows, and everything moves with it:
            # the calibrated offset drifts off the sprite, the deselect misses,
            # and the same sx comes back both SELECTED and refused. None of
            # that is about the boundary.
            #
            # A building straddling screen x 0 removes all of it. The player's
            # BASE is parked so that part of its sprite is inside the native
            # band and part is outside, and then it is clicked twice WITHOUT
            # MOVING ANYTHING: once at a native x safely above 0, once at a
            # native x safely below it. Same target, same frame, same offset --
            # the only thing that differs is which side of the boundary the
            # click is on, which is the entire question.
            logp = os.path.join(out, "stderr.log")

            def hudq():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return -1
                ls = [l for l in tail.splitlines() if "hudquads=" in l]
                if not ls:
                    return -1
                try:
                    return int(ls[-1].split("hudquads=")[1].split()[0])
                except (IndexError, ValueError):
                    return -1

            def base_pos():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                for ln in reversed(tail.splitlines()):
                    if ln.startswith("[lb_base]"):
                        try:
                            return (int(ln.split("sx=")[1].split()[0]),
                                    int(ln.split("sy=")[1].split()[0]))
                        except (IndexError, ValueError):
                            return None
                return None

            def geom4():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in tail.splitlines() if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                import re as _re
                m = _re.search(
                    r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) (\d+)x(\d+)",
                    ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            capture(os.path.join(stepdir, "halves-resize.png"), 1600, 900)
            time.sleep(1.5)

            # Park the base so its sprite straddles the boundary. The origin is
            # its top-left-ish corner and the calibrated clickable centre sits
            # about +24 from it, so an sx around -20 puts roughly half the
            # sprite either side of 0.
            for _ in range(40):
                bp = base_pos()
                if bp is None:
                    time.sleep(1.0)
                    continue
                lo = args.click_halves_at - 6
                hi = args.click_halves_at + 6
                if lo <= bp[0] <= hi:
                    break
                key = 0x10 if bp[0] > hi else 0x20   # RIGHT moves it left
                d.cmd("keys", mask=0x3FF & ~key)
                time.sleep(0.012
                           if abs(bp[0] - args.click_halves_at) < 60
                           else 0.05)
                d.cmd("keys", mask=0x3FF)
                time.sleep(1.2)
            bp = base_pos()
            g = geom4()
            print("click-halves: base at %s, geom=%s" % (bp, g))
            if bp and g:
                gw, gh, fx, fy, fw, fh = g
                extra = render_band(gw)
                sx, sy = bp
                dsx = fx + int(55 * fw / gw)
                dsy = fy + int((180 + render_band_top(gh)) * fh / gh)

                def deselect():
                    for _ in range(3):
                        d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                              down=True)
                        time.sleep(0.15)
                        d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                              down=False)
                        time.sleep(0.9)
                        if hudq() < 3:
                            return True
                    return False

                def click_native(nx, ny):
                    wx = fx + int((nx + extra) * fw / gw)
                    wy = fy + int((ny + render_band_top(gh)) * fh / gh)
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=True)
                    time.sleep(0.22)
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=False)
                    time.sleep(1.2)
                    return wx, wy

                # A GRID ACROSS THE WHOLE SPRITE, not two guessed points.
                #
                # Where a building's clickable pixels are relative to the
                # position the port reports is not known, and guessing it wrong
                # makes the IN-band control fail, which invalidates the row the
                # same way docs/217's original probe was invalidated. Sweeping
                # the sprite needs no such guess: if ANY in-band point selects
                # and NO out-of-band point does, at one camera position on one
                # target, the boundary is the cause and nothing else can be.
                results = []
                for dy in (16, 32):
                    for dx in (4, 12, 20, 28, 36, 44):
                        nx = sx + dx
                        clean = deselect()
                        before = hudq()
                        wx, wy = click_native(nx, sy + dy)
                        after = hudq()
                        ok = after >= 3 and before < 3
                        if ok:
                            # WHAT got selected. hudquads says "something is",
                            # not "the base is", and the two are not the same
                            # claim -- a builder or a unit standing on the same
                            # pixels would raise the count identically. The top
                            # screen names the selection.
                            try:
                                r = d.cmd("framebuffer", engine="A")
                                import binascii as _b
                                raw = _b.unhexlify(r["rgb"])
                                png = os.path.join(
                                    stepdir, "sel-%d-%d.png" % (nx, sy + dy))
                                _write_png(png, r["w"], r["h"], raw)
                            except Exception:              # noqa: BLE001
                                pass
                        results.append((nx, sy + dy, nx >= 0, ok, clean))
                        print("click-halves: native (%4d,%3d) %-3s window="
                              "(%d,%d)  hud %d -> %d  %s%s"
                              % (nx, sy + dy, "IN" if nx >= 0 else "OUT",
                                 wx, wy, before, after,
                                 "SELECTED" if ok else "no",
                                 "" if clean else "  [deselect failed]"))
                ins = [r for r in results if r[2]]
                outs = [r for r in results if not r[2]]
                print("click-halves: IN band  %d/%d selected  at x %s"
                      % (sum(1 for r in ins if r[3]), len(ins),
                         sorted({r[0] for r in ins if r[3]}) or "none"))
                print("click-halves: OUT band %d/%d selected  at x %s"
                      % (sum(1 for r in outs if r[3]), len(outs),
                         sorted({r[0] for r in outs if r[3]}) or "none"))

        if args.click_sweep:
            # WHERE EXACTLY DOES OUT-OF-BAND CLICKING STOP WORKING?
            #
            # docs/217 established that a click outside the native 256 columns
            # does not select, with the coordinate proven to arrive. What it
            # could not say is WHERE the cut-off is, and that is the thing that
            # names the cause: a cut-off at exactly screen x 0 is a sign or a
            # bounds test, and a cut-off somewhere else is not.
            #
            # The camera is panned in small steps and the player's BASE is
            # clicked at each one. A base does not move, so sx changes only
            # because the camera did, and every row of the table is the same
            # click against the same target at a different screen x.
            logp = os.path.join(out, "stderr.log")

            def hudq():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return -1
                ls = [l for l in tail.splitlines() if "hudquads=" in l]
                if not ls:
                    return -1
                try:
                    return int(ls[-1].split("hudquads=")[1].split()[0])
                except (IndexError, ValueError):
                    return -1

            def base_pos():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                for ln in reversed(tail.splitlines()):
                    if ln.startswith("[lb_base]"):
                        try:
                            return (int(ln.split("sx=")[1].split()[0]),
                                    int(ln.split("sy=")[1].split()[0]))
                        except (IndexError, ValueError):
                            return None
                return None

            def geom3():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in tail.splitlines() if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                import re as _re
                m = _re.search(
                    r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) (\d+)x(\d+)",
                    ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            capture(os.path.join(stepdir, "sweep-resize.png"), 1600, 900)
            time.sleep(1.5)
            rows = []
            sweep_off = None
            for step in range(args.click_sweep):
                g = geom3()
                bp = base_pos()
                if not g or not bp:
                    print("click-sweep: step %d has no geom/base yet" % step)
                    time.sleep(1.2)
                    continue
                gw, gh, fx, fy, fw, fh = g
                extra = render_band(gw)
                sx, sy = bp
                # Deselect first, through the anchored deselect icon -- the
                # only thing that deselects (a ground click is a move order).
                dsx = fx + int(55 * fw / gw)
                dsy = fy + int((180 + render_band_top(gh)) * fh / gh)
                for _ in range(2):
                    d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                          down=True)
                    time.sleep(0.15)
                    d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                          down=False)
                    time.sleep(0.8)
                    if hudq() < 3:
                        break
                before = hudq()
                # SELF-CALIBRATE THE OFFSET, in band, before trusting any row.
                #
                # The port reports an entity's own position, and a sprite is
                # drawn at an offset from it -- lb_field measures a category-0
                # sprite at (-16, -12) from where the entity says it is. A base
                # is a large building whose origin is nowhere near its middle,
                # and clicking that origin misses it: measured, sx 123 and 43
                # are both INSIDE the native band and neither selected. A probe
                # that cannot select its target in band says nothing about out
                # of band, which is the same trap docs/217 caught.
                #
                # So the first in-band step searches a small grid for an offset
                # that works, and every later step reuses it.
                cand = ([sweep_off] if sweep_off else
                        [(0, 0), (16, 16), (24, 24), (16, 32), (32, 24),
                         (8, 40), (24, 40), (0, 24), (32, 8)])
                after = before
                used = (0, 0)
                for ox, oy in cand:
                    wx = fx + int((sx + ox + extra) * fw / gw)
                    wy = fy + int((sy + oy + render_band_top(gh)) * fh / gh)
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=True)
                    time.sleep(0.22)
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=False)
                    time.sleep(1.0)
                    after = hudq()
                    used = (ox, oy)
                    if after >= 3 and before < 3:
                        if not sweep_off and 0 <= sx < 200:
                            sweep_off = (ox, oy)
                            print("click-sweep: calibrated offset %s in band"
                                  % (sweep_off,))
                        break
                ox, oy = used
                wx = fx + int((sx + ox + extra) * fw / gw)
                wy = fy + int((sy + oy + render_band_top(gh)) * fh / gh)
                ok = after >= 3 and before < 3
                # A PICTURE WHILE SOMETHING IS ACTUALLY SELECTED.
                #
                # This is the only moment a run has one, and two separate
                # checks need it: the top-screen panel must NOT draw its camera
                # box while a unit is selected (the selection panel replaces the
                # minimap), and the brick menus only exist in this state at all,
                # so nothing about their edge alignment can be judged without
                # it. The sweep used to capture nothing.
                if ok:
                    capture(os.path.join(out, "selected-sx%d.png" % sx))
                rows.append((sx, sy, before, after, ok))
                print("click-sweep: sx=%-5d sy=%-4d window=(%d,%d)  hud %d -> %d  %s"
                      % (sx, sy, wx, wy, before, after,
                         "SELECTED" if ok else
                         ("(dirty: was %d)" % before if before >= 3 else "no")))
                # Pan RIGHT, which moves everything LEFT on screen. The
                # hold SHORTENS near the boundary: the question is whether
                # the cut-off sits exactly at screen x 0, and a 40-px step
                # cannot answer that.
                hold = args.click_sweep_hold
                if -70 < sx < 90:
                    hold = args.click_sweep_hold / 4.0
                d.cmd("keys", mask=0x3FF & ~0x10)
                time.sleep(hold)
                d.cmd("keys", mask=0x3FF)
                time.sleep(1.4)
            with open(os.path.join(out, "clicksweep.txt"), "w") as fh:
                for r in rows:
                    fh.write("sx=%d sy=%d before=%d after=%d selected=%d%s"
                             % (r + (chr(10),)))
            good = [r[0] for r in rows if r[4]]
            bad = [r[0] for r in rows if not r[4] and r[2] < 3]
            print("click-sweep: SELECTED at sx %s" % (sorted(good) or "none"))
            print("click-sweep: refused  at sx %s" % (sorted(bad) or "none"))

        if args.click_ents:
            logp = os.path.join(out, "stderr.log")

            def hudq():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return -1
                ls = [l for l in tail.splitlines() if "hudquads=" in l]
                if not ls:
                    return -1
                try:
                    return int(ls[-1].split("hudquads=")[1].split()[0])
                except (IndexError, ValueError):
                    return -1

            def geom2():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                ls = [l for l in tail.splitlines() if l.startswith("[lb_geom]")]
                if not ls:
                    return None
                import re as _re
                m = _re.search(
                    r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) (\d+)x(\d+)",
                    ls[-1])
                return tuple(int(v) for v in m.groups()) if m else None

            def ext_entities():
                # The port names what is out there every report frame. A
                # STRUCTURE is the target this needs: a hero walks, so a
                # position read once and clicked a second later is a position
                # the unit has left, and every earlier attempt at this test was
                # aiming at where a hero used to be.
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-80000:].decode("latin-1", "replace")
                except OSError:
                    return []
                out2, seen = [], set()
                for ln in reversed(tail.splitlines()):
                    if "-EXT" not in ln or "owner=0" not in ln:
                        continue
                    try:
                        sx = int(ln.split("sx=")[1].split()[0])
                        sy = int(ln.split("sy=")[1].split()[0])
                        bp = int(ln.split("bp=")[1].split()[0])
                    except (IndexError, ValueError):
                        continue
                    if bp in seen:
                        continue
                    seen.add(bp)
                    out2.append((sx, sy, bp))
                    if len(out2) >= 6:
                        break
                return out2

            capture(os.path.join(stepdir, "ents-resize.png"), 1600, 900)
            time.sleep(1.5)
            g = geom2()
            ents = ext_entities()
            print("click-ents: geom=%s, %d entity(ies) out of band" % (g, len(ents)))
            if g and ents:
                gw, gh, fx, fy, fw, fh = g
                extra = render_band(gw)
                try:
                    d.cmd("deep_trace", on=True)
                except Exception:                      # noqa: BLE001
                    pass
                for sx, sy, bp in ents:
                    # Deselect first, via the anchored deselect icon, so each
                    # entity is judged from a clean state.
                    dsx = fx + int(55 * fw / gw)
                    dsy = fy + int((180 + render_band_top(gh)) * fh / gh)
                    for _ in range(2):
                        d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                              down=True)
                        time.sleep(0.15)
                        d.cmd("frontend_input", action="click", x=dsx, y=dsy,
                              down=False)
                        time.sleep(0.9)
                        if hudq() < 3:
                            break
                    before = hudq()
                    wx = fx + int((sx + extra) * fw / gw)
                    wy = fy + int((sy + render_band_top(gh)) * fh / gh)
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=True)
                    time.sleep(0.25)
                    d.cmd("frontend_input", action="click", x=wx, y=wy,
                          down=False)
                    time.sleep(1.4)
                    after = hudq()
                    capture(os.path.join(stepdir, "ent-bp%d.png" % bp))
                    print("click-ents: bp=%-4d sx=%-5d sy=%-4d window=(%d,%d)"
                          "  hud %d -> %d  %s"
                          % (bp, sx, sy, wx, wy, before, after,
                             "SELECTED" if after >= 3 and before < 3
                             else ("(was already selected)" if before >= 3
                                   else "not selected")))

        if args.force_click:
            logp = os.path.join(out, "stderr.log")
            def hud_n():
                try:
                    with open(logp, "rb") as fh:
                        tail = fh.read()[-40000:].decode("latin-1", "replace")
                except OSError:
                    return -1
                ls = [l for l in tail.splitlines() if "hudquads=" in l]
                if not ls:
                    return -1
                try:
                    return int(ls[-1].split("hudquads=")[1].split()[0])
                except (IndexError, ValueError):
                    return -1
            # CONTROL: hero inside the band, plain raw tap -- the path already
            # known to select. If this does not report a HUD, nothing below is
            # interpretable.
            c = aim_hero(d, logp, stepdir, capture, lo=60, hi=190)
            if c is not None:
                for _ in range(3):
                    tap(d, max(8, min(247, c)), 95, 1.4)
                    if hud_n() >= 3:
                        break
            print("force-click CONTROL: in-band sx=%s -> hud=%d %s"
                  % (c, hud_n(), "SELECTED" if hud_n() >= 3 else "NOT SELECTED"))
            capture(os.path.join(stepdir, "force-control.png"))
            # DESELECT, AND PROVE IT. Without this the final reading is
            # meaningless: a leftover selection from the control looks exactly
            # like a successful out-of-band click. The deselect BUTTON is no
            # use here -- the HUD anchor has moved it out to the widened edge,
            # where a raw tap cannot reach it -- so click empty ground instead
            # and check the HUD actually collapses.
            dropped = False
            # (15,180) is the deselect BUTTON at its native position. It is
            # only reachable with LB_HUDANCHOR=0 -- with anchoring on this
            # port has moved it out to the widened edge, where a raw tap in
            # guest coordinates cannot reach it. Empty-ground taps do not
            # deselect in this game; five different spots were tried.
            for spot in ((15, 180), (15, 186), (20, 178),
                         (30, 30), (230, 30)):
                tap(d, spot[0], spot[1], 1.3)
                if hud_n() <= 1:
                    dropped = True
                    break
            print("force-click: after deselect hud=%d %s"
                  % (hud_n(), "(cleared)" if dropped else "(STILL SELECTED - "
                     "the result below cannot be trusted)"))
            if not dropped:
                print("force-click: ABORTING the out-of-band test, it would be "
                      "unfalsifiable")
            # A TIGHT band, because LB_TOUCH_FORCE is a fixed number set at
            # launch while the hero's position varies per run. One run
            # forced -45 at a hero standing at -61 and called the 16-px
            # miss a failure of the feature.
            sx = (aim_hero(d, logp, stepdir, capture, lo=-49, hi=-41)
                  if dropped else None)
            if dropped:
                print("force-click: hero now at sx=%s (LB_TOUCH_FORCE should match)" % sx)
            if dropped:
                for _ in range(3):
                    tap(d, 128, 95, 1.4)
                    if hud_n() >= 3:
                        break
                capture(os.path.join(stepdir, "force-ext.png"))
                print("force-click: out-of-band -> hud=%d %s"
                      % (hud_n(), "SELECTED" if hud_n() >= 3 else "NOT SELECTED"))

        if args.polydump:
            # CLASSIFY THE HUD BY MOVING THE CAMERA, not by guessing at z.
            #
            # The HUD has to be repositioned for a wider screen, and
            # game.toml already records that OBJ is disabled outright in
            # gameplay (DISPCNT 00010F08, bit 12 clear) -- so the in-field
            # buttons are not sprites and the anchor key could never have moved
            # them. They are 3D geometry, mixed in with the units in the same
            # polygon list.
            #
            # What separates them is behaviour, not appearance: world geometry
            # slides when the camera pans and screen-locked geometry does not.
            # This is the same test that settled whether the fog was map- or
            # screen-anchored, and it needs no assumption about z.
            import json as _json
            def polys():
                try:
                    return d.cmd("gx_polygons")["polygons"]
                except Exception:                      # noqa: BLE001
                    return []
            before = polys()
            for _ in range(3):
                d.cmd("keys", mask=0x3FF & ~0x10)      # hold Right
                time.sleep(0.25)
                d.cmd("keys", mask=0x3FF)
                time.sleep(0.4)
            after = polys()
            with open(os.path.join(out, "polys.json"), "w") as fh:
                _json.dump({"before": before, "after": after}, fh, indent=1)
            print("polydump: %d polys before, %d after" % (len(before), len(after)))

        def fx_geom_early():
            try:
                with open(os.path.join(out, "stderr.log"), "rb") as fh:
                    tl = fh.read()[-200000:].decode("latin-1", "replace")
            except OSError:
                return None
            import re as _re
            ms = _re.findall(r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) "
                             r"(\d+)x(\d+)", tl)
            return tuple(int(v) for v in ms[-1]) if ms else None

        if args.fxband or args.fyband:
            # GOAL: the last open rendering check -- a NAMED effect drawn in a
            # widened column, on ground that is actually visible.
            #
            # Everything before this failed for one of two reasons, and both
            # are avoidable rather than unlucky:
            #
            #   * panning to where effects already are also panned into
            #     UNEXPLORED map, so `[lb_shroud]` read `shrouded=768/768` and
            #     an effect out there is hidden by the game's own rule; and
            #   * the effects that happen on explored ground cluster around the
            #     base, which the camera keeps near surface x 128 -- and a band
            #     does not begin until 256. `--explore` orders units to native
            #     x 250, so nothing it does can put one in a band either.
            #
            # So order a unit OUT to a band coordinate directly. Clicks there
            # are proven (docs/234: 4 of 4 at x 253..265 and -249..-16), the
            # unit reveals ground as it walks, and its own effects travel with
            # it. This runs in-process because the debug server takes ONE
            # client: a second connection from a helper script reset the first.
            # RESIZE FIRST, AND CHECK THE TARGET IS ON THE SURFACE.
            #
            # --bandsize only resizes inside the band trials, so a run that
            # asks for 1600x900 without --bandselect stays at the default
            # window. This probe then ordered to native x=400 on a 512x192
            # surface whose valid range is -128..383 -- a coordinate the
            # surface cannot hold. The click went nowhere and the unit sat
            # still, which is indistinguishable from "the game ignored it"
            # unless the probe says so. It says so now.
            if args.bandsize:
                bw_, bh_ = (int(v) for v in args.bandsize.split("x"))
                capture(os.path.join(stepdir, "fxband-size.png"), bw_, bh_)
                time.sleep(2.0)
            _g = fx_geom_early()
            if _g:
                _gw = _g[0]
                _left = render_band(_gw)
                _lo, _hi = -_left, 255 + (_gw - 256 - _left)
                if not (_lo <= args.fxband <= _hi):
                    _new = _hi - 8 if args.fxband > 0 else _lo + 8
                    print("fxband: x=%d is off a %d-wide surface (valid "
                          "%d..%d); using %d instead"
                          % (args.fxband, _gw, _lo, _hi, _new))
                    args.fxband = _new
                else:
                    # `_ext` here was a NameError -- the band width is `_left`.
                    # It survived because this is the in-range branch and every
                    # fxband run so far aimed out of range, so only the other
                    # branch ever executed. The first vertical run, which
                    # leaves the x at 0, hit it immediately.
                    print("fxband: surface %d wide, band %d px, target x=%d "
                          "is %s" % (_gw, _left, args.fxband,
                                     "IN THE BAND" if (args.fxband < 0 or
                                                       args.fxband > 255)
                                     else "native"))
            else:
                print("fxband: no [lb_geom] line -- needs LB_VERBOSE=1; "
                      "cannot tell whether the target is on the surface")
            # THE SAME CHECK VERTICALLY, and it is not the same arithmetic --
            # the vertical split is asymmetric (render_band_top's docstring
            # records a 772x550 surface splitting 144/214 where centred would
            # be 179), so the valid row range is not symmetric about the band.
            if args.fyband and _g:
                _gh = _g[1]
                _top = render_band_top(_gh)
                _ylo, _yhi = -_top, 191 + (_gh - 192 - _top)
                if not (_ylo <= args.fyband <= _yhi):
                    _newy = _yhi - 8 if args.fyband > 0 else _ylo + 8
                    print("fyband: y=%d is off a %d-tall surface (valid "
                          "%d..%d); using %d instead"
                          % (args.fyband, _gh, _ylo, _yhi, _newy))
                    args.fyband = _newy
                else:
                    print("fyband: surface %d tall, top band %d px, target "
                          "y=%d is %s"
                          % (_gh, _top, args.fyband,
                             "IN THE BAND" if (args.fyband < 0 or
                                               args.fyband > 191)
                             else "native"))
            if args.fyband:
                print("fyband: ordering a unit out to y=%d -- a row the DS "
                      "panel cannot express" % args.fyband)
            else:
                print("fxband: ordering a unit out to x=%d and watching for a "
                      "named effect there" % args.fxband)
            # `logp` is set inside the other probe branches, so name the log
            # here rather than borrowing a variable this branch never assigns.
            fx_logp = os.path.join(out, "stderr.log")

            def fx_cam():
                try:
                    with open(fx_logp, "rb") as fh:
                        tl = fh.read()[-200000:].decode("latin-1", "replace")
                except OSError:
                    return None
                for ln in reversed(tl.splitlines()):
                    m = re.search(r"cam=\(?(-?\d+),\s*(-?\d+)", ln)
                    if m:
                        return int(m.group(1)), int(m.group(2))
                return None

            # PARK THE CAMERA, NOT A UNIT, FOR A LEFT-BAND TEST.
            #
            # A left-band target is world x = camera + (a negative offset), so
            # with the camera near the map's left edge the whole band lies off
            # the map or inside the impassable rock border that rings it --
            # and a refusal to walk there is CORRECT. docs/314 got two false
            # reproductions that way: at camera x 0..136 on this 1536-px map,
            # every left-band target it tried was world x < 88, all border.
            #
            # So push the camera right until the band covers real ground. The
            # threshold is expressed in world terms rather than guessed:
            # aim for the target landing at world x >= 200, about metatile 8.
            if args.fxband < 0:
                want = 200 - args.fxband
                got = None
                for _ in range(40):
                    c = fx_cam()
                    if c is None:
                        time.sleep(1.0)
                        continue
                    got = c[0]
                    if got >= want:
                        break
                    d.cmd("keys", mask=0x3FF & ~0x10)     # Right held
                    time.sleep(0.35)
                    d.cmd("keys", mask=0x3FF)
                    time.sleep(0.5)
                print("fxband: camera parked at x=%s (want >=%d so the target "
                      "lands at world x>=200, clear of the border)%s"
                      % (got, want,
                         "" if (got is not None and got >= want)
                         else "  -- NOT REACHED, the trials below are not a "
                              "fair left-band test"))

            # DELIVER THE ORDER THE WAY A MOUSE DOES.
            #
            # The first version of this used tap(), which issues a RAW `touch`
            # at the coordinate given. That is the DS touchscreen path and it
            # cannot carry a coordinate outside the native screen -- it wraps.
            # Ordering to 400 put the unit at 144 (400 & 0xFF), which happened
            # to be where it already stood, and ordering to 268 sent it left to
            # 12. Both looked like a clean, reproducible engine bug and both
            # were arithmetic done by the probe. docs/238 records the
            # retraction.
            #
            # A band coordinate only survives through the WINDOW: map it with
            # the live field rect and send it as frontend_input, exactly as
            # click_native does for the selection probes that score 4 of 4.
            def fx_geom():
                try:
                    with open(fx_logp, "rb") as fh:
                        tl = fh.read()[-200000:].decode("latin-1", "replace")
                except OSError:
                    return None
                import re as _re
                ms = _re.findall(r"guest=(\d+)x(\d+) field=(-?\d+),(-?\d+) "
                                 r"(\d+)x(\d+)", tl)
                return tuple(int(v) for v in ms[-1]) if ms else None

            def fx_click(nx, ny, hold=0.30):
                g = fx_geom()
                if not g:
                    return False
                gw, gh, fx_, fy_, fw_, fh2_ = g
                bx = render_band(gw)
                by = render_band_top(gh)
                wx = int(round(fx_ + (nx + bx) * fw_ / float(gw)))
                wy = int(round(fy_ + (ny + by) * fh2_ / float(gh)))
                d.cmd("frontend_input", action="click", x=wx, y=wy, down=True)
                time.sleep(hold)
                d.cmd("frontend_input", action="click", x=wx, y=wy, down=False)
                time.sleep(hold)
                return True
            shots = 0
            t_end = time.time() + max(60, args.linger or 240)
            last_order = 0.0
            while time.time() < t_end and shots < 3:
                time.sleep(3.0)
                try:
                    with open(fx_logp, "rb") as fh:
                        tail = fh.read()[-400000:].decode("latin-1", "replace")
                except OSError:
                    continue
                lines = tail.splitlines()

                if time.time() - last_order > 25.0:
                    last_order = time.time()
                    # THE FIRST ON-SCREEN ONE, not simply the newest line.
                    # The port now logs every owner's builder, so the most
                    # recent line is frequently the OPPONENT's, hundreds of
                    # pixels off this camera -- taking it blind would skip the
                    # order every time and look like the probe doing nothing.
                    # NAME THE ENTITY, so "the unit did not move" cannot be
                    # "a different unit was reported". Every owner's builder is
                    # logged now, so identity has to be proved, not assumed.
                    ux = uy = None
                    uptr = "?"
                    for ln in reversed(lines):
                        if not (ln.startswith("[lb_builder]")
                                or ln.startswith("[lb_hero]")):
                            continue
                        try:
                            cx_ = int(ln.split("sx=")[1].split()[0])
                            cy_ = int(ln.split("sy=")[1].split()[0])
                        except (IndexError, ValueError):
                            continue
                        if 0 <= cx_ < 256 and 0 <= cy_ < 192:
                            ux, uy = cx_, cy_
                            if "ptr=" in ln:
                                uptr = ln.split("ptr=")[1].split()[0]
                            break
                    if ux is not None:
                        # COUNT THE COMMANDS, don't infer them from movement.
                        #
                        # func_020EB910 -- the submit docs/137 read -- appends
                        # the command to its player's list and then bumps a
                        # global counter at 0x021585AC. Reading that across an
                        # order answers "was a command submitted at all?"
                        # directly, with no override and no inference from
                        # whether a unit happened to walk.
                        #
                        # This is the measurement a dispatch observer could not
                        # make: an override on the move issuer never fired even
                        # for an order that demonstrably worked, because the
                        # handler table captures the function at registration.
                        def cmdcount():
                            try:
                                return int.from_bytes(bytes.fromhex(
                                    d.cmd("read_mem", cpu=9, addr=0x021585AC,
                                          len=4)["hex"]), "little")
                            except Exception:          # noqa: BLE001
                                return None
                        # FOLLOW THE ORDERED ENTITY EVEN OUT OF BAND.
                        #
                        # The unit has to be ON screen to be clicked, but it
                        # does not have to stay there to be reported -- and for
                        # a LEFT-band order it must not, since the whole point
                        # is that it ends at a negative screen x. Reporting
                        # only 0..255 made a successful order look like it
                        # stopped at the edge.
                        def where(pt):
                            for ln in reversed(lines):
                                if pt in ln and ("[lb_builder]" in ln
                                                 or "[lb_hero]" in ln):
                                    try:
                                        return (int(ln.split("sx=")[1].split()[0]),
                                                int(ln.split("sy=")[1].split()[0]))
                                    except (IndexError, ValueError):
                                        return None
                            return None
                        before = cmdcount()
                        ok1 = fx_click(ux, uy)
                        mid = cmdcount()
                        # ONE AXIS MOVES PER RUN. With --fyband the column is
                        # held at the unit's own x, so the only coordinate the
                        # DS panel cannot express is the row -- and a result is
                        # therefore attributable to the row.
                        if args.fyband:
                            tx, ty = ux, args.fyband
                        else:
                            tx, ty = args.fxband, uy
                        ok2 = fx_click(tx, ty)
                        after = cmdcount()
                        if ok1 and ok2:
                            # IS THE TARGET EVEN ON THE MAP? A band column is
                            # only a fair test of band ORDERING while the world
                            # position behind it exists. With the camera at
                            # x=0..136 on a 1536-px map, a left-band target at
                            # native -110 is world x=-110..26 -- off the left
                            # edge for most of a run -- and refusing to walk
                            # there is CORRECT, not a defect.
                            #
                            # A first reading of this took a left-band run that
                            # moved nothing as evidence of a left-band bug. It
                            # was not: the right-band trial it was compared
                            # against had the target comfortably on the map, so
                            # the two were not the same experiment.
                            # Read the log FRESH. `lines` is not assigned until
                            # further down this same loop body, so using it
                            # here is either stale from the previous iteration
                            # or a NameError on the first -- the exact shape of
                            # bug this file has hit before with helpers used
                            # above their definition.
                            camx = camy = None
                            try:
                                with open(fx_logp, "rb") as fh:
                                    tl = fh.read()[-200000:].decode(
                                        "latin-1", "replace")
                                for ln in reversed(tl.splitlines()):
                                    m = re.search(r"cam=\(?(-?\d+),\s*(-?\d+)",
                                                  ln)
                                    if m:
                                        camx = int(m.group(1))
                                        camy = int(m.group(2))
                                        break
                            except OSError:
                                pass
                            onmap = ""
                            if camx is not None:
                                wx, wy = camx + tx, camy + ty
                                bad = (wx < 0 or wy < 0 or
                                       wx > 1535 or wy > 1023)
                                onmap = ("  world=(%d,%d)%s"
                                         % (wx, wy,
                                            "  OFF-MAP: a refusal here is "
                                            "correct" if bad else ""))
                            print("%s: ordered %s from (%d,%d) to (%d,%d)  "
                                  "commands %s -> %s -> %s (select +%s, "
                                  "order +%s)%s"
                                  % ("fyband" if args.fyband else "fxband",
                                     uptr, ux, uy, tx, ty,
                                     before, mid, after,
                                     "?" if (before is None or mid is None)
                                     else mid - before,
                                     "?" if (mid is None or after is None)
                                     else after - mid, onmap))
                            # Where it ended up, in band or out. This is the
                            # line that shows a LEFT-band order working: a
                            # negative sx cannot be produced by a byte
                            # truncation or by a clamp toward the native width.
                            time.sleep(6.0)
                            try:
                                with open(fx_logp, "rb") as fh:
                                    lines = fh.read()[-400000:].decode(
                                        "latin-1", "replace").splitlines()
                            except OSError:
                                pass
                            endp = where(uptr)
                            if endp:
                                # SCORE THE AXIS THAT WAS MOVED. Scoring x on a
                                # vertical run would report "in the native
                                # columns" for a perfectly good row order and
                                # read as a failure.
                                if args.fyband:
                                    out_of_range = (endp[1] < 0 or
                                                    endp[1] > 191)
                                    tag = "  <- OUT OF THE NATIVE ROWS"
                                else:
                                    out_of_range = (endp[0] < 0 or
                                                    endp[0] > 255)
                                    tag = "  <- OUT OF THE NATIVE COLUMNS"
                                print("%s:   %s is now at (%d,%d)%s"
                                      % ("fyband" if args.fyband else "fxband",
                                         uptr, endp[0], endp[1],
                                         tag if out_of_range else ""))
                        else:
                            print("fxband: no [lb_geom] yet; needs LB_VERBOSE=1")
                    else:
                        print("fxband: no unit on screen to order yet")

                hits = [l for l in lines if l.startswith("[lb_fx] ")
                        and ("RIGHT-BAND" in l or "LEFT-BAND" in l)]
                if not hits:
                    continue
                shroud = [l for l in lines if l.startswith("[lb_shroud]")]
                print("fxband: %s" % hits[-1][:110])
                if shroud:
                    # PRINT THE SHROUD WITH IT. An effect in a fogged column is
                    # not visible, so a band hit without this line cannot tell
                    # the two cases apart -- which is exactly how the earlier
                    # result overstated itself.
                    print("fxband:   %s" % shroud[-1][:140])
                # The ADAPTIVE framebuffer, which is the widened surface --
                # this is the picture that shows the effect out in the band.
                # lb_exp's Dbg has no shot() of its own (that is lb_warp's), so
                # take the framebuffer and write the PNG with the helper this
                # file already has.
                for eng in ("A", "B"):
                    name = os.path.join(stepdir, "fxband-%s-%d.png" % (eng, shots))
                    try:
                        r = d.cmd("framebuffer", engine=eng, adaptive=True)
                        _write_png(name, r["w"], r["h"],
                                   binascii.unhexlify(r["rgb"]))
                        print("fxband:   wrote %s (%dx%d)"
                              % (name, r["w"], r["h"]))
                    except Exception as exc:           # noqa: BLE001
                        print("fxband:   framebuffer failed: %s" % exc)
                shots += 1
            print("fxband: captured %d moment(s)" % shots)

        if args.coldshots:
            # THE FIRST SECONDS, at one-second granularity.
            #
            # The linger loop below samples every ten seconds, which cannot
            # see a cold start at all: "the area outside the original aspect
            # ratio loads slowly on entering a match" is a thing that happens
            # and finishes between its first two frames. These are named by
            # the second so the fill can be watched frame by frame.
            for t in range(args.coldshots):
                capture(os.path.join(stepdir, "cold-%02d.png" % t))
                time.sleep(1.0)
            print("cold: captured %d shot(s) at 1s spacing" % args.coldshots)

        if args.linger:
            print("linger: %ds in gameplay" % args.linger)
            for t in range(0, args.linger, 10):
                time.sleep(10)
                capture(os.path.join(stepdir, "linger-%03d.png" % t))
            # AGAIN, so the pair brackets the wait. "Is this counter moving?"
            # is the question one dump cannot answer, and it is the one that
            # came up: a stream position of 0 could mean "not playing" or
            # "just started".
            if args.peek:
                post = peek_dump(d, args.peek)
                print("--- peek after the %ds linger" % args.linger)
                print(chr(10).join(post))
                with open(os.path.join(out, "peek.txt"), "w") as fh:
                    fh.write("before:" + chr(10) +
                             chr(10).join(args._peek_pre) + chr(10) +
                             "after:" + chr(10) +
                             chr(10).join(post) + chr(10))
                if post == args._peek_pre:
                    print("peek: IDENTICAL before and after -- nothing moved")
            if args.peekptr:
                print("--- peekptr after the %ds linger" % args.linger)
                print(chr(10).join(peekptr_dump(d, args.peekptr)))

        if args.stop_after:
            if args.keep:
                # HOLD THE STOPPED ROUTE OPEN, without the size sweep. A
                # tutorial is a chain of "read the box, do what it says";
                # one run per tap costs a boot each time.
                # LET GO OF THE DEBUG SOCKET FIRST. The server serves one
                # client: while this connection stays open, another client's
                # first command times out and its reconnect is refused.
                d.close()
                print("READY: route stopped; gameplay is live on port %d; "
                      "drive it, then kill nds_runner.exe" % PORT, flush=True)
                while proc.poll() is None:
                    time.sleep(5)
                return 0
            for size in SIZES:
                w, h = (int(v) for v in size.split("x"))
                capture(os.path.join(out, size + ".png"), w, h)
            return 0

        if not in_gameplay(d) and args.keep:
            # The route is not reliable enough to gate a held run on. The
            # profile-name keyboard is driven by D-pad and A rather than touch
            # -- the highlight never leaves Q however the keys are tapped -- so
            # the route cannot create a profile on a fresh save. Rather than
            # throw the instance away, hold it open and wait: it can be driven
            # by hand, and the point of --keep is to have ONE live battle to
            # ask questions of.
            capture(os.path.join(out, "WAITING.png"))
            print("WAITING: route did not reach gameplay; drive it by hand. "
                  "Port %d. Polling for a battle..." % PORT, flush=True)
            wait_until = time.time() + 900
            while time.time() < wait_until and proc.poll() is None:
                time.sleep(5)
                if in_gameplay(d):
                    break

        # ONE SAMPLE IS NOT A VERDICT, and this one throws the run away.
        #
        # in_gameplay reads live IO and pointers, so it is a snapshot of one
        # instant: a frame caught mid-transition -- a fade, a dialog raising an
        # overlay, the map pointer between stores -- reads as "menu". Gating an
        # abort on a single such sample is what discarded four consecutive
        # runs that were in fact sitting in a battle. Sample a few times over a
        # couple of seconds and believe the best of them; a run that really is
        # in a menu fails all of them just as fast as it failed one.
        for _attempt in range(8):
            if in_gameplay(d):
                break
            time.sleep(0.25)
        else:
            capture(os.path.join(out, "NOT-GAMEPLAY.png"))
            print("%s: route did not reach gameplay (8 samples over 2s)"
                  % args.label, file=sys.stderr)
            return 1

        # Guest state, read while gameplay is confirmed on screen. A patch that
        # rewrites an instruction can report success and still change nothing,
        # so the check is what the GUEST's own structures say afterwards, not
        # what the patcher logged.
        probe = []
        # The VIEW object and the map, which the tilemap-widening work needs.
        # func_02039A5C allocates 0x1B0 bytes and stores the pointer at
        # 0x02152030+4; the camera is at +0x5E/+0x60 and must move in X only
        # when panning sideways.
        try:
            view = int.from_bytes(bytes.fromhex(
                d.cmd("read_mem", cpu=9, addr=0x02152034, len=4)["hex"]),
                "little")
            mapo = int.from_bytes(bytes.fromhex(
                d.cmd("read_mem", cpu=9, addr=0x02156CB0, len=4)["hex"]),
                "little")
            probe.append("  view   @02152034 -> %08X" % view)
            probe.append("  map    @02156CB0 -> %08X" % mapo)
            if 0x02000000 <= view < 0x02400000:
                def cam():
                    b = bytes.fromhex(d.cmd("read_mem", cpu=9,
                                            addr=view + 0x5E, len=4)["hex"])
                    return (int.from_bytes(b[0:2], "little", signed=True),
                            int.from_bytes(b[2:4], "little", signed=True))
                c0 = cam()
                d.cmd("keys", mask=0x3FF & ~0x10)
                time.sleep(0.8)
                d.cmd("keys", mask=0x3FF)
                time.sleep(0.4)
                c1 = cam()
                probe.append("  camera %s -> %s  %s" % (
                    c0, c1,
                    "CONFIRMED" if (c1[0] != c0[0] and c1[1] == c0[1])
                    else "not confirmed"))
            if 0x02000000 <= mapo < 0x02400000:
                w = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=mapo + 0x14,
                                        len=1)["hex"])[0]
                probe.append("  map width (metatiles) +0x14 = %d" % w)
        except Exception as exc:                   # noqa: BLE001
            probe.append("  view/map probe failed: %s" % exc)

        for label, addr, n in (("view-dim insn", 0x020333A4, 4),
                               ("fog object", 0x02151F28, 0x20),
                               # The fog bit buffer itself. Whether the guest
                               # POPULATES the widened mask is a different
                               # question from whether it ALLOCATED one, and
                               # only the bits answer it.
                               ("fog bits", 0x020251E8, 168)):
            try:
                r = d.cmd("read_mem", cpu=9, addr=addr, len=n)
                probe.append("%-14s %08X  %s" % (label, addr, r["hex"]))
            except Exception as exc:               # noqa: BLE001
                probe.append("%-14s %08X  FAILED %s" % (label, addr, exc))
        # Engine A's 2D registers, read in gameplay. BGxCNT bit 6 is mosaic and
        # MOSAIC's low nibble is its X size; a mosaic BG would explain alternate
        # columns dropping out, and BGxHOFS says which tile column the leftmost
        # pixels are decoded from.
        for label, addr in (("DISPCNT", 0x04000000), ("WIN0H", 0x04000040),
                            ("WIN1H", 0x04000042), ("WIN0V", 0x04000044),
                            ("WIN1V", 0x04000046), ("WININ", 0x04000048),
                            ("WINOUT", 0x0400004A), ("BG0CNT", 0x04000008),
                            ("BG1CNT", 0x0400000A), ("BG2CNT", 0x0400000C),
                            ("BG3CNT", 0x0400000E), ("BG0HOFS", 0x04000010),
                            ("BG1HOFS", 0x04000014), ("BG2HOFS", 0x04000018),
                            ("BG3HOFS", 0x0400001C), ("MOSAIC", 0x0400004C),
                            # THE BLEND REGISTERS, AND ENGINE B'S COPIES.
                            #
                            # The defect: the fog is transparent where it
                            # should be opaque, with a screenshot where the
                            # cloud lattice takes the colour of the water and
                            # the sand underneath it -- blue chevrons over the
                            # lake, tan ones over the path. That is not terrain
                            # visible through holes in a tile, it is BG2's own
                            # pixels tinted by BG1's, which is alpha blending.
                            # The probe never read BLDCNT, so the layer that is
                            # blended and the coefficients doing it have been
                            # guessed at repeatedly and measured never.
                            #
                            # Both engines, because the field is the BOTTOM
                            # screen and which hardware engine that is depends
                            # on POWCNT1 bit 15. Reading only 0x0400xxxx risks
                            # describing the HUD while reasoning about the map.
                            ("BLDCNT", 0x04000050),
                            ("BLDALPHA", 0x04000052),
                            ("BLDY", 0x04000054),
                            ("B_DISPCNT", 0x04001000),
                            ("B_BG0CNT", 0x04001008),
                            ("B_BG1CNT", 0x0400100A),
                            ("B_BG2CNT", 0x0400100C),
                            ("B_BG3CNT", 0x0400100E),
                            ("B_WININ", 0x04001048),
                            ("B_WINOUT", 0x0400104A),
                            ("B_BLDCNT", 0x04001050),
                            ("B_BLDALPHA", 0x04001052),
                            ("B_BLDY", 0x04001054),
                            ("POWCNT1", 0x04000304)):
            try:
                v = d.cmd("read_io", cpu=9, addr=addr, width=16)["value"]
                probe.append("  %-9s %08X = %04X" % (label, addr, v & 0xFFFF))
            except Exception as exc:               # noqa: BLE001
                probe.append("  %-9s FAILED %s" % (label, exc))

        # Sampled repeatedly, because a single read catches an arbitrary moment
        # in the frame. "Windows are enabled" and "windows are enabled while the
        # visible lines are being drawn" are different claims, and only the
        # second one would explain a masked sliver at the screen edge.
        seen = {}
        for _ in range(20):
            key = (d.cmd("read_io", cpu=9, addr=0x04000000, width=16)["value"],
                   d.cmd("read_io", cpu=9, addr=0x04000040, width=16)["value"],
                   d.cmd("read_io", cpu=9, addr=0x0400004A, width=16)["value"])
            seen[key] = seen.get(key, 0) + 1
            time.sleep(0.05)
        for (dc, w0h, wo), n in sorted(seen.items(), key=lambda kv: -kv[1]):
            probe.append("  x%-2d DISPCNT=%04X WIN0H=%04X WINOUT=%04X  win_on=%d"
                         % (n, dc, w0h, wo, 1 if (dc & 0xE000) else 0))

        # AND THE SAME QUESTION FOR THE BLEND, because "BLDCNT is 0" read once
        # at an arbitrary point in the frame does not mean the blend is off
        # while the field's scanlines are being drawn. A per-scanline BLDCNT is
        # exactly how a game gives one band of the screen a different effect,
        # and a single sample would report the value it holds during vblank.
        bseen = {}
        for _ in range(20):
            try:
                key = (d.cmd("read_io", cpu=9, addr=0x04000050,
                             width=16)["value"],
                       d.cmd("read_io", cpu=9, addr=0x04000052,
                             width=16)["value"],
                       d.cmd("read_io", cpu=9, addr=0x04001050,
                             width=16)["value"],
                       d.cmd("read_io", cpu=9, addr=0x04001052,
                             width=16)["value"])
            except Exception:                      # noqa: BLE001
                break
            bseen[key] = bseen.get(key, 0) + 1
            time.sleep(0.05)
        for (ba, bb, bc, bd), n in sorted(bseen.items(), key=lambda kv: -kv[1]):
            probe.append("  x%-2d A_BLDCNT=%04X A_BLDALPHA=%04X   "
                         "B_BLDCNT=%04X B_BLDALPHA=%04X"
                         % (n, ba, bb, bc, bd))

        # THE CHEAT FLAGS. flags[0] is Reveal Map (lb_overlay.cpp's table, whose
        # comment records that setting it revealed the whole minimap). Free
        # Play ALWAYS has fog unless the no-fog cheat is on, so an
        # all-clear fog mask means something here is set -- most likely in the
        # seeded save, which is a fully-unlocked profile.
        try:
            fp = int.from_bytes(bytes.fromhex(
                d.cmd("read_mem", cpu=9, addr=0x02150A70, len=4)["hex"]),
                "little")
            probe.append("  cheat flags block @02150A70 -> %08X" % fp)
            if 0x02000000 <= fp < 0x02400000:
                fb = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=fp,
                                         len=16)["hex"])
                probe.append("  cheat flags: %s"
                             % " ".join("%d" % b for b in fb))
        except Exception as exc:                   # noqa: BLE001
            probe.append("  cheat flag probe failed: %s" % exc)

        # THE FOG MASK, decoded the way the game reads it.
        #
        # func_020EEBF4(fog, tx, ty) is
        #     row = *(u32*)(*(u32*)(fog+0x14) + ty*4);  row[tx>>3] & (1<<(tx&7))
        # and func_0203A4B4 walks tx 0..31, ty 0..23 writing tile 100 wherever
        # that is set. So the mask is SCREEN space, one bit per tile.
        #
        # The question for the widened columns is whether anything populates it
        # past column 31. LB_VIEW_WIDTH resizes the mask to 56 columns; whether
        # the guest's fog update actually fills them is what this prints.
        try:
            fog = 0x02151F28
            hdr = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=fog,
                                      len=0x18)["hex"])
            stride = int.from_bytes(hdr[4:8], "little")
            fw = int.from_bytes(hdr[8:12], "little")
            fh = int.from_bytes(hdr[12:16], "little")
            rowtab = int.from_bytes(hdr[20:24], "little")
            probe.append("  fog: stride %d width %d height %d rowtable %08X"
                         % (stride, fw, fh, rowtab))
            if 0x02000000 <= rowtab < 0x02400000 and 0 < fh <= 64:
                ptrs = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=rowtab,
                                           len=4 * min(fh, 24))["hex"])
                for ty in (0, 8, 16):
                    rp = int.from_bytes(ptrs[ty * 4:ty * 4 + 4], "little")
                    if not (0x02000000 <= rp < 0x02400000):
                        continue
                    rb = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=rp,
                                             len=max(stride, 1))["hex"])
                    bits = "".join(
                        "#" if (rb[tx >> 3] >> (tx & 7)) & 1 else "."
                        for tx in range(min(fw, stride * 8)))
                    probe.append("    row %2d: %s" % (ty, bits))
        except Exception as exc:                   # noqa: BLE001
            probe.append("  fog probe failed: %s" % exc)

        # VRAMCNT_A..I. Which banks back engine-A BG, and how much of the space
        # at 0x06000000 actually exists, decides where a relocated screen block
        # can live. Two attempts have now picked a block by elimination and hit
        # live data (docs/199); this reads the mapping instead.
        #
        # Each is a byte: bit 7 enables, bits 0-2 are MST (1 = engine-A BG for
        # banks A-D), bits 3-4 the 128 KB offset within that target.
        for i, name in enumerate("ABCDEFGHI"):
            try:
                v = d.cmd("read_io", cpu=9, addr=0x04000240 + i,
                          width=8)["value"] & 0xFF
                probe.append("  VRAMCNT_%s = %02X  enabled=%d mst=%d ofs=%d"
                             % (name, v, (v >> 7) & 1, v & 7, (v >> 3) & 3))
            except Exception as exc:               # noqa: BLE001
                probe.append("  VRAMCNT_%s FAILED %s" % (name, exc))

        # The leftmost columns of BOTH screens. If only the field screen drops
        # alternate columns at the very left it is that scene's tile data; if
        # both do, it is the renderer's left edge and a general fault.
        for eng in ("A", "B"):
            r = d.cmd("framebuffer", engine=eng)
            raw = binascii.unhexlify(r["rgb"])
            w = r["w"]
            for y in (0, 48, 96, 144):
                row = "  screen %s y=%3d " % (eng, y)
                for x in range(8):
                    i = (y * w + x) * 3
                    row += "%02x%02x%02x " % (raw[i], raw[i + 1], raw[i + 2])
                probe.append(row)

        with open(os.path.join(out, "probe.txt"), "w") as fh:
            fh.write("\n".join(probe) + "\n")

        if args.touchwho:
            # WHO READS THE TOUCH COORDINATE DURING A CLICK THAT WORKS.
            #
            # This question has now spent three documents following the static call
            # graph and both functions it arrived at are measurably off the
            # selection path: func_020aa8d4 is never called at all, and
            # func_020a8794 is rejected by its own [r4+12] guard on EVERY
            # click -- including in-band ones that select successfully. So the
            # chain docs/224 and docs/260 reasoned about does not pick units,
            # and their shared conclusion ("the coordinate arrives and the
            # cartridge ignores it") was measured on the wrong path.
            #
            # Ask the machine instead of the call graph. 0215376A/0215376C are
            # the cartridge's own touch x/y (docs/214, and the literals the
            # handler loads at 020A880C/020A8810). A read watch over them
            # during a click that DOES select names every consumer with its PC,
            # which is the picker plus whatever else samples the point.
            #
            # IN-BAND ON PURPOSE. The question is not yet "why does out-of-band
            # fail" but "what handles a click at all"; the out-of-band case is
            # only meaningful once the working path is known.
            try:
                d.cmd("watch_clear")
            except Exception:                          # noqa: BLE001
                pass
            d.cmd("watch_range", lo=0x02153760, hi=0x02153780)
            d.cmd("deep_trace", on=True)
            time.sleep(0.3)
            tap_verified(d, args.click_x, args.click_y, 0.25)
            time.sleep(0.8)
            ev = d.cmd("watch", max=8192)["events"]
            d.cmd("deep_trace", on=False)
            reads, writes = {}, {}
            for e in ev:
                tgt = writes if e["write"] else reads
                tgt.setdefault((e["pc"], e["addr"]), []).append(e["value"])
            lines = ["touchwho: %d events over 02153760..02153780" % len(ev)]
            for name, tbl in (("READ ", reads), ("WRITE", writes)):
                for (pc, addr), vals in sorted(tbl.items(),
                                               key=lambda kv: -len(kv[1])):
                    lines.append("  %s pc=%08X addr=%08X n=%-5d vals=%s"
                                 % (name, pc, addr, len(vals),
                                    [("%d" % ((v - 65536) if v > 32767 else v))
                                     for v in vals[:6]]))
            with open(os.path.join(out, "touchwho.txt"), "w") as fh:
                fh.write("\n".join(lines) + "\n")
            print("\n".join(lines[:28]))

        if args.drainwatch:
            # THE LAST THING THE RUN DOES, so whatever the probes above
            # provoked is still in the ring. --whowrites drains early and does
            # its own taps at fixed points; this one takes whatever the rest of
            # the run left, which is how "who READ the touch coordinate while a
            # unit was being selected" gets answered.
            # ARM IT HERE, not before the route.
            #
            # Deep trace routes every main-RAM access through the slow
            # path, and arming it at boot made the whole route miss its
            # taps -- a run that reported "route did not reach gameplay"
            # about a watch, not about the game. Anything rebuilt per frame
            # is captured by arming it for a second here.
            try:
                d.cmd("deep_trace", on=True)
                time.sleep(1.5)
                ev = d.cmd("watch", max=8192)["events"]
            except Exception as e:                       # noqa: BLE001
                ev = []
                print("drainwatch: %s" % e)
            byte_pc = {}
            for e in ev:
                key = (e["write"], e["pc"])
                byte_pc.setdefault(key, []).append((e["addr"], e["value"]))
            lines = ["events=%d" % len(ev)]
            for (w, pc), lst in sorted(byte_pc.items(),
                                       key=lambda kv: -len(kv[1])):
                lines.append("%s pc=%08X n=%-5d sample=%s"
                             % ("WRITE" if w else "READ ", pc, len(lst),
                                [("%08X" % a, v) for a, v in lst[:4]]))
            with open(os.path.join(out, "drainwatch.txt"), "w") as fh:
                fh.write("\n".join(lines) + "\n")
            print("\n".join(lines[:14]))

        # Frame rate first: it is the measurement most easily spoiled by the
        # resizes below, which force the frontend to rebuild its textures.
        a = d.cmd("frontend_stats")
        t0 = time.time()
        time.sleep(6.0)
        b = d.cmd("frontend_stats")
        dt = time.time() - t0
        frames = b.get("frames", 0) - a.get("frames", 0)
        # drain_ticks is the audio-queue WAIT. The frame loop is paced by
        # the audio device, so if the port is running below 60 this is the
        # first number to look at: time here is the loop choosing to sleep,
        # not the emulator failing to keep up.
        keys = ("emu_ticks", "present_ticks", "adaptive_ticks",
                "upload_ticks", "draw_ticks", "swap_ticks", "drain_ticks")
        delta = {k: b.get(k, 0) - a.get(k, 0) for k in keys}
        with open(os.path.join(out, "fps.txt"), "w") as fh:
            fh.write("%.2f fps  (%d frames in %.2fs)\n"
                     % (frames / dt, frames, dt))
            # THESE ARE SDL PERFORMANCE-COUNTER TICKS, NOT NANOSECONDS.
            #
            # Dividing by 1e9 assumed a 1 GHz counter; Windows reports 10 MHz,
            # so every phase printed 100x too small. emu_ticks read as "0.213
            # ms/frame" for emulating an entire DS frame, which made the
            # emulation look free and sent this hunt off after the frontend,
            # the audio pacing and the scheduler in turn -- all of which were
            # innocent. The real figure is 21.3 ms of a 23.4 ms frame.
            # frontend_stats has always returned the counter frequency; the
            # harness simply was not reading it.
            freq = b.get("freq", 0) or 1
            fh.write("   (counter frequency %d Hz)\n" % freq)
            for k in keys:
                fh.write("   %-15s %8.3f ms/frame\n"
                         % (k, delta[k] / max(frames, 1) / freq * 1000.0))
            # AUDIO HEALTH, and it has to come from here.
            #
            # The runner does print underruns, but only in the [sdl] soak
            # summary at clean shutdown, and this harness kills the process --
            # so grepping a run log for it finds nothing and that silence reads
            # exactly like a pass. An underrun is a real risk whenever the
            # frame budget changes, because the loop is paced by the audio
            # device, so it is measured rather than assumed. Cumulative over
            # the process, so the OVER-THE-WINDOW delta is what the change did;
            # a nonzero total with a zero delta is boot, not gameplay.
            fh.write("   %-15s %8d total, %d in this window\n"
                     % ("underruns", b.get("underruns", 0),
                        b.get("underruns", 0) - a.get("underruns", 0)))
            # STABILITY, not the mean (docs/349): how many frames in the
            # window had more WORK than one 60 Hz period, and worse. The
            # counters exist from the docs/349 build on; an older binary
            # (LB_EXE) reports none, and says so rather than printing zeros.
            if "work_over_16ms" in b:
                spikes = ["%s %d" % (lab, b.get(k, 0) - a.get(k, 0))
                          for lab, k in ((">16.7ms", "work_over_16ms"),
                                         (">20ms", "work_over_20ms"),
                                         (">25ms", "work_over_25ms"),
                                         (">33ms", "work_over_33ms"),
                                         (">50ms", "work_over_50ms"))]
                fh.write("   %-15s %s (of %d frames)\n"
                         % ("long frames", ", ".join(spikes), frames))
            else:
                fh.write("   %-15s not reported by this binary\n"
                         % "long frames")
            # CADENCE (docs/349): the spread of present-to-present intervals
            # over the window. Steady motion needs steady presents, and the
            # frame loop presents as soon as a frame's work is done, so the
            # mean alone (the fps) says nothing about it.
            if "present_intervals" in b:
                n = b["present_intervals"] - a.get("present_intervals", 0)
                sm = b["present_interval_sum"] - a.get("present_interval_sum",
                                                       0)
                sq = b["present_interval_sq"] - a.get("present_interval_sq", 0)
                if n > 1:
                    mean = sm / float(n)
                    var = max(0.0, sq / float(n) - mean * mean)
                    fh.write("   %-15s mean %.3f ms, sd %.3f ms, >20ms %d, "
                             ">25ms %d (of %d intervals)\n"
                             % ("present cadence", mean / freq * 1000.0,
                                (var ** 0.5) / freq * 1000.0,
                                b["present_gap_20ms"] -
                                a.get("present_gap_20ms", 0),
                                b["present_gap_25ms"] -
                                a.get("present_gap_25ms", 0), n))
        print("%s: %.1f fps" % (args.label, frames / dt))
        # Did the port change its own settings during this run? (docs/349)
        if _pin_ok:
            try:
                import json as _json
                with open(_pinned, encoding="utf-8") as fh:
                    _want = _json.load(fh)
                with open(_live, encoding="utf-8") as fh:
                    _got = _json.load(fh)
                _chg = sorted(k for k in set(_want) | set(_got)
                              if _want.get(k) != _got.get(k))
                if _chg:
                    print("%s: WARNING -- the port CHANGED its settings during "
                          "this run: %s. The next run is re-pinned, but THIS "
                          "one may have simulated something else from the "
                          "change on." % (args.label, ", ".join(
                              "%s %r->%r" % (k, _want.get(k), _got.get(k))
                              for k in _chg)))
            except (OSError, ValueError) as exc:
                print("%s: could not compare settings: %s" % (args.label, exc))

        # These accumulators only
        # exist when NDS_PROFILE_GPU / NDS_PROFILE_SCHED armed them at process
        # start, which is why they are read as raw JSON rather than summarised
        # here -- a zero field means "not armed", not "no time spent".
        for name in ("profile", "dispatch_stats", "event_counts"):
            try:
                with open(os.path.join(out, name + ".json"), "w") as fh:
                    fh.write(repr(d.cmd(name)))
            except Exception as exc:               # noqa: BLE001
                with open(os.path.join(out, name + ".json"), "w") as fh:
                    fh.write("FAILED %s" % exc)

        # WHICH GUEST ADDRESSES ARE BEING INTERPRETED.
        #
        # docs/254: a gdb sampling profile puts tier3_run on the main thread's
        # stack in 52.5% of samples -- over half the frame is the armv4t
        # interpreter, because code the guest COPIED INTO RAM (ITCM residents,
        # the IRQ handler) has no statically lifted bank. Which addresses those
        # are decides whether the fix is a few missing entry points in
        # port/config/lb_arm9_itcm.toml or something structural.
        #
        # Issued HERE, on the harness's own connection, and not from a separate
        # probe process: the debug server accepts exactly one client at a time
        # (listen(fd, 1) + serve-until-hangup), so a second client times out in
        # the backlog and a third is refused. Three runs were lost to reading
        # that as "the tier-3 commands crash the server".
        try:
            cov = d.cmd("tier3_coverage", max=262144)
            ent = sorted(cov.get("entries", []),
                         key=lambda e: -e.get("hits", 0))
            total = sum(e.get("hits", 0) for e in ent) or 1
            with open(os.path.join(out, "tier3_coverage.txt"), "w") as fh:
                fh.write("%d entries, %d total hits\n" % (len(ent), total))
                fh.write("%-6s %-12s %-6s %-5s %-12s %12s %7s\n"
                         % ("cpu", "pc", "thumb", "kind", "caller",
                            "hits", "share"))
                for e in ent:
                    fh.write("arm%-3d 0x%08X %-6d %-5d 0x%08X %12d %6.2f%%\n"
                             % (e["cpu"], e["pc"], e["thumb"], e["kind"],
                                e["caller"], e["hits"],
                                100.0 * e["hits"] / total))
            print("%s: tier3 %d entries, %d hits"
                  % (args.label, len(ent), total))
        except Exception as exc:                   # noqa: BLE001
            with open(os.path.join(out, "tier3_coverage.txt"), "w") as fh:
                fh.write("FAILED %s" % exc)

        if args.cull_width:
            # ARM data-processing immediates are an 8-bit value rotated right by
            # an even amount, so not every constant is expressible. Build the
            # encoding and refuse rather than write a wrong instruction.
            def arm_imm12(v):
                for rot in range(16):
                    r = ((v << (2 * rot)) | (v >> (32 - 2 * rot))) & 0xFFFFFFFF
                    if r <= 0xFF:
                        return (rot << 8) | r
                return None
            addr = 0x02038FD0
            cur = int(d.cmd("read_mem", cpu=9, addr=addr, len=4)["hex"][:8], 16)
            cur = int.from_bytes(cur.to_bytes(4, "big"), "little")
            imm = arm_imm12(args.cull_width)
            note = []
            note.append("  cull insn @%08X = %08X (expect E2822C01)" % (addr, cur))
            if imm is None:
                note.append("  %d is not an ARM immediate; not patched"
                            % args.cull_width)
            elif cur != 0xE2822C01:
                note.append("  NOT the stock encoding; refusing to patch")
            else:
                word = 0xE2822000 | imm
                d.cmd("write_mem", cpu=9, addr=addr, value=word, width=32)
                back = int(d.cmd("read_mem", cpu=9, addr=addr,
                                 len=4)["hex"][:8], 16)
                back = int.from_bytes(back.to_bytes(4, "big"), "little")
                note.append("  wrote %08X, read back %08X" % (word, back))
                time.sleep(2.0)
                polys = d.cmd("gx_polygons")["polygons"]
                outside = [q for q in polys
                           if q["min_x"] < 96 or q["max_x"] > 351]
                note.append("  after widen: %d polys, %d outside native"
                            % (len(polys), len(outside)))
                if polys:
                    note.append("  x range %d..%d"
                                % (min(q["min_x"] for q in polys),
                                   max(q["max_x"] for q in polys)))
                capture(os.path.join(out, "cullwide.png"))
            for n in note:
                print(n)
            with open(os.path.join(out, "probe.txt"), "a") as fh:
                fh.write(os.linesep.join(note) + os.linesep)

        if args.select:
            sx, sy = args.select
            capture(os.path.join(out, "select-before.png"))
            # The same geometry list with NOTHING selected. Whatever appears
            # only in the second dump is the HUD, which is a stronger claim
            # than picking polygons whose bounding box happens to sit where a
            # button was measured.
            try:
                with open(os.path.join(out, "gx_before.json"), "w") as fh:
                    fh.write(repr(d.cmd("gx_polygons")))
            except Exception:                      # noqa: BLE001
                pass
            tap(d, sx, sy, 2.0)
            capture(os.path.join(out, "select-after.png"))
            # DISPCNT again: if selecting turns OBJ on, the in-field HUD is
            # sprites after all and adaptive_hud_anchor becomes relevant again.
            # Bit 12 was measured clear with nothing selected, which is not the
            # same measurement.
            # The 3D geometry actually being rendered. The in-field HUD is 3D
            # (docs/194), so telling a button apart from a soldier has to be
            # done here rather than in any 2D layer. Screen-space bounding
            # boxes plus z and attributes are what a classifier would key on.
            try:
                with open(os.path.join(out, "gx_polygons.json"), "w") as fh:
                    fh.write(repr(d.cmd("gx_polygons")))
                with open(os.path.join(out, "gx_state.json"), "w") as fh:
                    fh.write(repr(d.cmd("gx_state")))
            except Exception as exc:               # noqa: BLE001
                print("gx dump failed: %s" % exc, file=sys.stderr)

            v = d.cmd("read_io", cpu=9, addr=0x04000000, width=16)["value"]
            with open(os.path.join(out, "probe.txt"), "a") as fh:
                fh.write("  after select: DISPCNT=%04X  OBJ=%d\n"
                         % (v, 1 if (v & 0x1000) else 0))
            print("%s: after select DISPCNT=%04X OBJ=%d"
                  % (args.label, v, 1 if (v & 0x1000) else 0))

        if args.overlay:
            # Open the settings overlay and photograph it. The save manager tab
            # only executes when the panel is drawn, so "it compiles" is not
            # evidence that it renders.
            try:
                d.cmd("frontend_input", action="key", key="F1", down=True)
                time.sleep(0.1)
                d.cmd("frontend_input", action="key", key="F1", down=False)
            except Exception as exc:               # noqa: BLE001
                print("overlay: F1 failed (%s)" % exc, file=sys.stderr)
            time.sleep(1.5)
            capture(os.path.join(out, "overlay.png"))
            # Click a tab in the overlay, in ABSOLUTE WINDOW PIXELS.
            #
            # THE OLD SEQUENCE NEVER WORKED, and nothing noticed because it
            # failed silently into a screenshot of whatever tab was already
            # open. It captured the mouse, pinned it with a relative
            # (-5000,-5000), then walked back out by (click_x, click_y) --
            # plausible, and wrong: `overlay-tab.png` in exp-savetab2, the run
            # that named itself after the Saves tab, shows the DISPLAY tab. So
            # does every later one. Any claim resting on "the Saves tab was
            # exercised" through this path was unfounded.
            #
            # debug_server.cpp offers `action="click"`, which takes absolute
            # window pixels and pushes them unchanged. That is what an imgui
            # hit-test wants; a captured relative cursor is not.
            #
            # Coordinates are window pixels as they appear in the capture, minus
            # the ~8 px window border: the Saves tab sits at image x=418 and its
            # long-standing default here is 410.
            try:
                d.cmd("frontend_input", action="click",
                      x=args.click_x, y=args.click_y, down=True)
                time.sleep(0.15)
                d.cmd("frontend_input", action="click",
                      x=args.click_x, y=args.click_y, down=False)
                time.sleep(0.8)
            except Exception as exc:               # noqa: BLE001
                print("overlay: click failed (%s)" % exc, file=sys.stderr)
            capture(os.path.join(out, "overlay-tab.png"))

        if args.nudge_cam:
            # Defined HERE, at the top of the branch, because both
            # the before and after reads need it and this function
            # has already produced three NameErrors
            # from helpers defined below their first use.
            # `after` is a byte offset into the log: only lines written
            # BEYOND it count. Without it this returns the newest
            # [lb_base] line whether or not one has been emitted since
            # the nudge -- and that log is periodic, so a run where it has
            # not fired yet reads the PRE-nudge position and reports the
            # entity as not having moved. One run did exactly that:
            # sx -293 -> -293 against a 287 px camera move, which reads as
            # a large defect and is a stale line.
            def _base_sx(tries=12, after=None):
                for _ in range(tries):
                    try:
                        with open(os.path.join(out, "stderr.log"),
                                  "rb") as fh:
                            if after is not None:
                                fh.seek(after)
                                raw = fh.read()
                            else:
                                raw = fh.read()[-300000:]
                            tl = raw.decode("latin-1", "replace")
                    except OSError:
                        tl = ""
                    for ln in reversed(tl.splitlines()):
                        if (ln.startswith("[lb_base]")
                                and "owner=0" in ln and "sx=" in ln):
                            try:
                                return int(
                                    ln.split("sx=")[1].split()[0])
                            except (IndexError, ValueError):
                                pass
                    time.sleep(0.25)
                return None
            # `want` stays None unless the write actually happened. The nudge
            # has a REFUSAL path -- it compares the hardcoded view pointer at
            # 0x02152034 against the camera the port reports and declines to
            # write when they disagree by more than 32, because the pointer goes
            # stale between runs. That refusal is correct and it fires in
            # practice, so everything downstream has to cope with there being no
            # nudge rather than reading an unbound name.
            _sx0 = None
            _logmark = None
            want = None
            # The camera the game ACTUALLY ended at, which is not the one
            # asked for whenever the request falls outside the map: a
            # nudge to -10 was pinned to 265, a real delta of -175 rather
            # than the -450 requested. Predicting from the request then
            # reports DISAGREES for a run that is in fact exact.
            camx0 = None
            achieved = None
            try:
                view = int.from_bytes(bytes.fromhex(
                    d.cmd("read_mem", cpu=9, addr=0x02152034,
                          len=4)["hex"]), "little")
                raw = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=view + 0x5E,
                                          len=2)["hex"])
                cam = int.from_bytes(raw, "little", signed=True)

                # CHECK IT AGAINST THE PORT BEFORE WRITING ANYTHING.
                #
                # `0x02152034` is a HARDCODED view pointer, and the port stopped
                # trusting one long ago: lb_field_camera() locates the view
                # object by scanning for the scroll callback's record, with the
                # comment "the view object moves between runs, so it is located
                # rather than hardcoded". This read had not been updated, so it
                # reported `camx -7889` -- tile -987 on a 192-tile map -- while
                # the port's own [lb_wide] line said `cam_tx=17` in the same
                # frames. A poke at that address lands somewhere arbitrary, and
                # the run afterwards looks like a camera experiment that simply
                # did not move anything.
                #
                # The port prints the camera it actually uses, so compare. This
                # refuses rather than corrupting unrelated memory.
                shown = None
                try:
                    # NOT `logp`: that name is bound LATER in this function, so
                    # reaching it from here raises "cannot access local variable
                    # 'logp'" and the whole nudge reports `nudge failed` without
                    # ever writing anything. Same trap as the build-flow branch's
                    # geom6(). The run directory is in scope; use it.
                    with open(os.path.join(out, "stderr.log"), "rb") as fh:
                        tail = fh.read()[-200000:].decode("latin-1", "replace")
                    import re as _re
                    ms = _re.findall(r"cam=\((-?\d+),\s*(-?\d+)\)", tail)
                    if ms:
                        shown = int(ms[-1][0])
                except OSError:
                    pass
                # LET THE CAMERA SETTLE BEFORE JUDGING THE TWO READS.
                #
                # The refusal below compares the hardcoded view's camx against
                # the camera the port reports, and the two are sampled at
                # DIFFERENT moments -- one by read_mem, one out of the log. A
                # camera that is still drifting therefore trips the threshold
                # for a reason that has nothing to do with the pointer.
                #
                # Measured: two consecutive runs refused with the two values
                # exactly 48 apart (584 vs 536, then 632 vs 584). A CONSTANT
                # difference across independent runs is not staleness, which
                # would be arbitrary; it is the camera moving a fixed amount
                # between the two samples while the route's pan settles. Other
                # runs nudged successfully with no disagreement at all.
                #
                # So: re-read both until the port's own camera stops changing,
                # then judge. If it never settles, the refusal still fires and
                # still says so -- this removes a false refusal, not the check.
                # COMPARE AGAINST THE LOG'S RECENT HISTORY, NOT ITS LAST LINE.
                #
                # This check exists to catch a stale view pointer, and it was
                # firing on healthy runs: three refusals in a row with the two
                # values EXACTLY 48 apart (584/536, 632/584, 616/568). A
                # constant difference across independent runs is not staleness,
                # which would be arbitrary.
                #
                # It is a LOG LAG. [lb_band] prints on change plus periodically,
                # so its newest `cam=` is behind the live camera by however far
                # the camera has moved since that line was written. In the
                # refused run the log does reach 616 -- the very value the
                # pointer reported -- but port_camx had returned 568 from an
                # earlier line. An earlier attempt to fix this by waiting for
                # the logged value to settle could not work, because waiting
                # does not make a periodic log catch up.
                #
                # The right question is not "does the pointer match the latest
                # logged camera" but "is the pointer tracking the camera the
                # port has been reporting at all". So: accept if it is close to
                # ANY recent logged value. A genuinely stale pointer -- the
                # docs/174 case that read camx -7889 on a 192-tile map -- misses
                # every one of them by thousands and is still refused.
                recent = []
                try:
                    import re as _re2
                    with open(os.path.join(out, "stderr.log"), "rb") as fh:
                        _t = fh.read()[-300000:].decode("latin-1", "replace")
                    recent = [int(m[0]) for m in
                              _re2.findall(r"cam=\(?(-?\d+),\s*(-?\d+)\)?", _t)]
                except OSError:
                    pass
                recent = recent[-12:]
                if recent and min(abs(c - cam) for c in recent) > 32:
                    print("nudge: REFUSED -- the hardcoded view says camx=%d, "
                          "which matches none of the port's last %d reported "
                          "cameras %s. The pointer is stale. Not writing."
                          % (cam, len(recent), recent))
                    raise _NudgeRefused()
                # THE BEFORE SHOT, so the pair is a crossing test rather than a
                # single picture. The whole point of nudging by a known amount
                # is that content at native column c must appear at c - delta
                # afterwards -- and for c < delta it has moved out of the
                # guest's own columns and into the port's band. Comparing the
                # two is the single-renderer measurement docs/293 asked for,
                # with a delta that is verified rather than assumed (the pan
                # gives 7 or 484; this gives exactly what is asked).
                capture(os.path.join(out, "nudge-before.png"))
                _sx0 = _base_sx(tries=8)
                try:
                    _logmark = os.path.getsize(
                        os.path.join(out, "stderr.log"))
                except OSError:
                    _logmark = None
                want = cam + args.nudge_cam
                d.cmd("write_mem", cpu=9, addr=view + 0x5E, value=want & 0xFFFF,
                      width=16)
                time.sleep(0.4)
                back = int.from_bytes(bytes.fromhex(
                    d.cmd("read_mem", cpu=9, addr=view + 0x5E,
                          len=2)["hex"]), "little", signed=True)
                # Read it BACK. An injected write to state the game owns can be
                # reverted within a frame, and "nothing moved" would then look
                # like a rendering result instead of a failed poke.
                print("nudge: camx %d -> asked %d, now %d %s"
                      % (cam, want, back,
                         "(held)" if back == want else "(CLAMPED)"))
                camx0 = cam
                achieved = back
            except Exception as exc:               # noqa: BLE001
                print("nudge failed: %s" % exc, file=sys.stderr)
            # PAIR THE BAND TO THE CAPTURE INSTEAD OF HOPING TWO CADENCES MEET.
            #
            # A verified +150 guest-px camera move produced a -288 window-px
            # picture shift where 2x magnification predicts -300 (41bcdca8).
            # Six guest pixels unaccounted for, and the obvious suspect is the
            # band split, which [lb_band] shows going 262, 264, 265 as the
            # camera crosses 266, 268, 272 -- it demonstrably varies.
            #
            # That could not be settled from the log because [lb_band] prints on
            # CHANGE plus periodically, and there was no line for the capture's
            # surface at the post-nudge camera. Waiting for one whose cam
            # matches makes the band and the picture the same moment by
            # construction, which is the whole lesson of docs/290: do not pair
            # two clocks and hope.
            def _band_at(camwant, tries=24):
                import re as _re
                pat = (r"\[lb_band\] (\d+)x(\d+)  left=(\d+) right=(\d+)"
                       r"[^\n]*?cam=(-?\d+),(-?\d+)")
                for _ in range(tries):
                    try:
                        with open(os.path.join(out, "stderr.log"), "rb") as fh:
                            tl = fh.read()[-300000:].decode("latin-1", "replace")
                    except OSError:
                        tl = ""
                    for m in reversed(_re.findall(pat, tl)):
                        if int(m[4]) == camwant:
                            return tuple(int(v) for v in m)
                    time.sleep(0.25)
                return None
            try:
                # Bound before every consumer below, not inside one of them.
                _delta = (achieved - camx0) if (achieved is not None
                                                and camx0 is not None) else 0
                if achieved is None or camx0 is None:
                    print("nudge: no write happened, so there is nothing "
                          "to pair a band or a displacement against")
                    raise StopIteration
                _b0 = _band_at(camx0, tries=1)
                _b = _band_at(achieved)
                if _b0:
                    print("nudge: band BEFORE at cam %d -> %dx%d left=%d "
                          "right=%d" % (_b0[4], _b0[0], _b0[1], _b0[2], _b0[3]))
                if _b:
                    print("nudge: band AFTER  at cam %d -> %dx%d left=%d "
                          "right=%d" % (_b[4], _b[0], _b[1], _b[2], _b[3]))
                # THE DISPLACEMENT, FROM AN ENTITY RATHER THAN FROM PIXELS.
                #
                # docs/294: correlating terrain cannot measure this. The ground
                # repeats every 24 guest px (the metatile, and the 3-tile period
                # LB_TILECHK reads as 000/1E0/012), so two shifts 48 window px
                # apart both scored a perfect residual of 0.0 and the finder
                # locked onto the lattice instead of the true displacement.
                #
                # An ENTITY is unique. The base is the right one: stationary in
                # the world, unlike a builder that walks while the measurement
                # is being taken. Its screen x before and after a verified
                # camera nudge IS the displacement, as one number out of the
                # log, with no search and no lattice to be trapped by.
                # Only a line written AFTER the nudge counts.
                _sx1 = _base_sx(after=_logmark)
                if _sx0 is not None and _sx1 is not None:
                    _moved = _sx1 - _sx0
                    # NO BAND TERM. sx is a GUEST screen coordinate, measured
                    # from the guest's own origin, so where the compositor
                    # places those 256 columns inside the surface does not
                    # enter into it.
                    #
                    # Determined rather than assumed. The one run where the band
                    # actually moved -- left 265 -> 261, delta -4 -- reported
                    # the base moving +271 against a prediction of +267, and the
                    # error was +4: exactly the band delta, with the opposite
                    # sign. Every earlier run had a band delta of 0 and so could
                    # not tell the two conventions apart. Drop the term and that
                    # run is exact.
                    _delta = achieved - camx0
                    _pred = -_delta
                    print("nudge: base sx %d -> %d  (moved %+d guest px, "
                          "predicted %+d)  %s"
                          % (_sx0, _sx1, _moved, _pred,
                             "AGREES" if abs(_moved - _pred) <= 2
                             else "DISAGREES by %+d" % (_moved - _pred)))
                elif _sx0 is None:
                    print("nudge: no [lb_base] owner=0 sx= before the nudge -- "
                          "displacement unmeasured (needs LB_ENTLOG=1)")
                if _b0 and _b:
                    # THE ARITHMETIC THE DISCREPANCY TURNS ON, stated so it is
                    # checked rather than eyeballed. The picture should move by
                    # -(camera delta) plus whatever the band moved, all times
                    # the magnification.
                    _dl = _b[2] - _b0[2]
                    print("nudge: band delta %+d guest px -> picture should "
                          "move %+d window px (camera %+d, band %+d, mag 2)"
                          % (_dl, (-_delta + _dl) * 2,
                             -_delta * 2, _dl * 2))
                else:
                    print("nudge: NO [lb_band] line at cam %d -- the band "
                          "cannot be paired with this capture, so a shift "
                          "discrepancy is unattributable (needs LB_BANDLOG=1)"
                          % want)
            except StopIteration:
                pass
            except Exception as exc:               # noqa: BLE001
                print("nudge: band pairing failed: %s" % exc, file=sys.stderr)
            capture(os.path.join(out, "nudged.png"))

        if args.explore:
            # Find something to move. The field's units are 3D geometry, so the
            # polygon list is the way to locate one without guessing at pixels:
            # take polygons inside the native band, ignore the very large ones
            # (terrain and the base), and aim at the centre of a small one.
            polys = []
            try:
                polys = d.cmd("gx_polygons")["polygons"]
            except Exception as exc:                   # noqa: BLE001
                print("explore: no polygon list (%s)" % exc, file=sys.stderr)
            units = []
            for q in polys:
                w = q["max_x"] - q["min_x"]
                h = q["max_y"] - q["min_y"]
                cx = (q["min_x"] + q["max_x"]) // 2
                cy = (q["min_y"] + q["max_y"]) // 2
                if 2 <= w <= 40 and 2 <= h <= 40 and 96 <= cx < 352:
                    units.append((cx, cy))
            print("explore: %d polygons, %d unit-sized candidates"
                  % (len(polys), len(units)))
            # The polygon list came back EMPTY at this point in the frame, so
            # it cannot be relied on to find a unit. Fall back to sweeping the
            # field: tap a grid of points, each followed by a move order. Crude,
            # but a select only needs to land on a unit once.
            if not units:
                units = [(x, y) for y in (60, 96, 132)
                         for x in (128, 160, 192, 224, 256, 288)]
                print("explore: falling back to a %d-point tap sweep"
                      % len(units))

            def vis_count():
                """Revealed cells, straight out of the runner's own report."""
                return None

            # Guest touch coordinates are the native 256-wide field, so the
            # surface column has the widened band's 96 subtracted off.
            ordered = 0
            for cx, cy in units[:18]:
                gx = max(4, min(251, cx - 96))
                gy = max(4, min(187, cy))
                tap(d, gx, gy, 0.6)             # select
                tap(d, 250, gy, 1.2)            # order it to the right edge
                ordered += 1
            print("explore: issued %d select+move pairs" % ordered)
            # Let it walk. Exploration is the unit reaching new ground, which
            # takes seconds, and the grid report only prints every 120 frames.
            for _ in range(6):
                time.sleep(5.0)
                d.cmd("keys", mask=0x3FF & ~0x10)   # nudge right, keep it going
                time.sleep(0.3)
                d.cmd("keys", mask=0x3FF)
            capture(os.path.join(out, "explore-after.png"))

        if args.drag_native:
            # CAMERA DRIFT DURING A DRAG, the arm that works in BOTH modes.
            #
            # --drag-boundary delivers through the window with click6, which
            # needs [lb_geom]; Vanilla does not emit that line, so the Vanilla
            # control stopped at "geom=None" having performed no drags at all
            # (docs/317). Raw `touch` takes guest coordinates directly and
            # needs no geometry, so a drag confined to the native 256x192 can
            # be delivered identically in either mode. That is the whole point
            # of this mode: it is the comparison, not another Enhanced run.
            dn_log = os.path.join(out, "stderr.log")

            def dn_cam():
                try:
                    with open(dn_log, "rb") as fh:
                        t = fh.read()[-60000:].decode("latin-1", "replace")
                except OSError:
                    return None
                for ln in reversed(t.splitlines()):
                    m = re.search(r"cam=\(?(-?\d+),\s*(-?\d+)", ln)
                    if m:
                        return int(m.group(1)), int(m.group(2))
                return None

            def dn_drag(x0, y0, x1, y1, steps=18):
                d.cmd("touch", x=x0, y=y0, down=True)
                time.sleep(0.2)
                for i in range(1, steps + 1):
                    d.cmd("touch",
                          x=x0 + (x1 - x0) * i // steps,
                          y=y0 + (y1 - y0) * i // steps, down=True)
                    time.sleep(0.09)
                time.sleep(0.2)
                d.cmd("touch", x=x1, y=y1, down=False)
                time.sleep(1.6)

            # One path hugging the left edge -- where an edge-scroll would
            # trigger if the game has one -- and one across the middle, which
            # should not scroll. Both are entirely inside 0..255 / 0..191.
            paths = (("edge", 8, 60, 8, 140),
                     ("middle", 100, 60, 160, 140))
            tal = {k: [0, 0] for k, _, _, _, _ in paths}
            for rep in range(6):
                for name, x0, y0, x1, y1 in paths:
                    c0 = dn_cam()
                    dn_drag(x0, y0, x1, y1)
                    c1 = dn_cam()
                    if not c0 or not c1:
                        print("drag-native: rep%d %-6s no camera reading"
                              % (rep, name))
                        continue
                    ddx, ddy = c1[0] - c0[0], c1[1] - c0[1]
                    tal[name][0] += 1
                    if ddx or ddy:
                        tal[name][1] += 1
                    print("drag-native: rep%d %-6s (%d,%d)->(%d,%d)  "
                          "cam %s" % (rep, name, x0, y0, x1, y1,
                                      "%+d,%+d" % (ddx, ddy)
                                      if (ddx or ddy) else "still"))
            print("drag-native: RESULT  " + "  ".join(
                "%s %d/%d moved the camera" % (k, v[1], v[0])
                for k, v in tal.items()))

        if args.pause_cycle:
            # FOG/SHROUD FINICKY WHEN UNPAUSING.
            #
            # Read the counters, pause, linger, unpause, linger, read again.
            # The counters are cumulative, so the DELTA across the cycle is
            # what the pause caused -- no baseline run needed.
            logp2 = os.path.join(out, "stderr.log")

            def counters():
                try:
                    with open(logp2, "rb") as fh:
                        tl = fh.read()[-400000:].decode("latin-1", "replace")
                except OSError:
                    return None
                import re as _re
                out_d = {}
                m = _re.findall(r"model_bad_frames=(\d+) \(after-first-ok "
                                r"(\d+)\)", tl)
                if m:
                    out_d["model_bad"] = int(m[-1][0])
                    out_d["model_bad_after_ok"] = int(m[-1][1])
                m = _re.findall(r"mine vs guest BG1: (\d+)/(\d+) differ", tl)
                if m:
                    out_d["terrain_differ"] = int(m[-1][0])
                m = _re.findall(r"bare=(\d+)", tl)
                if m:
                    out_d["bare"] = int(m[-1])
                m = _re.findall(r"shrouded=(\d+)/", tl)
                if m:
                    out_d["shrouded"] = int(m[-1])
                m = _re.findall(r"fallbacks=(\d+)", tl)
                if m:
                    out_d["band_fallbacks"] = int(m[-1])
                return out_d

            def tap_start():
                # KEYINPUT is active LOW: 0x3FF is all released, START is bit 3.
                d.cmd("keys", mask=0x3FF & ~0x008)
                time.sleep(0.25)
                d.cmd("keys", mask=0x3FF)

            before = counters()
            capture(os.path.join(out, "pause-0-before.png"))
            print("pause-cycle: before  %s" % before)

            tap_start()
            time.sleep(3.0)
            paused = counters()
            capture(os.path.join(out, "pause-1-paused.png"))
            print("pause-cycle: paused  %s" % paused)

            # CATCH A TRANSIENT. "Finicky when unpausing" describes something
            # brief, and a capture three seconds later cannot see it -- the
            # first attempt took exactly that and the before/after frames came
            # out identical, which says nothing about the moment in between.
            # So shoot a burst across the first second, and read the counters
            # beside each shot so a wrong frame can be tied to a rejection.
            # docs/349: THE LAG ITSELF, from the live stats across the
            # unpause -- the defect (docs/324 item 12) is lag, and 311
            # measured the fog instead. The five captures below run
            # inside this span, so a hitch here may be the harness's own.
            _ps0 = d.cmd("frontend_stats")
            _pt0 = time.time()
            tap_start()
            burst = []
            prev = 0.0
            for i, delay in enumerate((0.10, 0.20, 0.35, 0.60, 1.00)):
                time.sleep(delay if i == 0 else delay - prev)
                prev = delay
                if not args.pause_nocap:
                    capture(os.path.join(out, "pause-2-unpause-%03dms.png"
                                         % int(delay * 1000)))
                burst.append((delay, counters()))
            for delay, c in burst:
                print("pause-cycle: +%4dms  %s" % (int(delay * 1000), c))
            time.sleep(2.0)
            _ps1 = d.cmd("frontend_stats")
            _pdt = time.time() - _pt0
            try:
                _pf = _ps1["frames"] - _ps0["frames"]
                _pq = _ps1.get("freq", 0) or 1
                print("pause-cycle: after the unpause, %.1f fps over %.1f s, "
                      "emu %.2f ms/frame, underruns %d; frames with >16.7 / "
                      ">20 / >33 ms of work: %d / %d / %d; present gaps "
                      ">20 / >25 ms: %d / %d"
                      % (_pf / _pdt, _pdt,
                         (_ps1["emu_ticks"] - _ps0["emu_ticks"]) /
                         max(_pf, 1) / _pq * 1000.0,
                         _ps1.get("underruns", 0) - _ps0.get("underruns", 0),
                         _ps1.get("work_over_16ms", 0) -
                         _ps0.get("work_over_16ms", 0),
                         _ps1.get("work_over_20ms", 0) -
                         _ps0.get("work_over_20ms", 0),
                         _ps1.get("work_over_33ms", 0) -
                         _ps0.get("work_over_33ms", 0),
                         _ps1.get("present_gap_20ms", 0) -
                         _ps0.get("present_gap_20ms", 0),
                         _ps1.get("present_gap_25ms", 0) -
                         _ps0.get("present_gap_25ms", 0)))
            except (KeyError, TypeError):
                print("pause-cycle: frame stats unavailable across the unpause")
            after = counters()
            capture(os.path.join(out, "pause-3-settled.png"))
            print("pause-cycle: after   %s" % after)

            if before and after:
                keys = sorted(set(before) | set(after))
                print("pause-cycle: DELTA across the cycle:")
                for k in keys:
                    b, a = before.get(k), after.get(k)
                    if b is None or a is None:
                        print("   %-22s %s -> %s" % (k, b, a))
                    else:
                        print("   %-22s %6d -> %6d   %+d" % (k, b, a, a - b))
            else:
                print("pause-cycle: counters unavailable -- the port may not "
                      "have reached gameplay; check stderr.log rather than "
                      "reading this as a clean result")

        if args.pan:
            # Scroll the camera, then dump again. Whether an artefact is
            # anchored to the SCREEN or to the MAP is the difference between a
            # rendering bug and the game's own tile data, and panning is the
            # only thing that tells them apart. It also exercises the reported
            # "panning moves the units but not the map".
            if args.pan_size:
                pw, ph = (int(v) for v in args.pan_size.split("x"))
                capture(os.path.join(out, "pan-size.png"), pw, ph)
                time.sleep(1.5)
            capture(os.path.join(out, "pan-before.png"))
            # KEYINPUT is active LOW and the command takes the raw mask, so
            # "Right held" is 0x3FF with bit 4 cleared, and 0x3FF is all
            # released. Passing named arguments here silently did nothing.
            # Sample BG1HOFS across the pan. func_0202d1b4 writes BGxOFS as
            # (-x) & 0x1FF from the scroll callback's `4 - (camx % 8)`, so the
            # terrain's horizontal offset should walk through a small set of
            # values as the camera crosses tile boundaries -- and the 4-pixel
            # bias is the candidate cause of the left-edge sliver.
            hofs = []
            steps = []
            # Log the CAMERA, not just BG1HOFS. Placing a unit inside a
            # widened column needs to know how far one step actually travels,
            # and the note that a 0.35s hold moves "about 27 tiles" was an
            # estimate that no run ever checked. camx is at view+0x5E -- the
            # same field func_02038F28 builds its gather rect from -- so this
            # also says exactly which columns the rect covered at each capture.
            # RE-READ THE VIEW EVERY STEP, not once before the loop.
            #
            # It used to be hoisted, so a single unreadable sample -- the
            # battle not live at that instant, the pointer caught mid-store --
            # disabled camera logging for the WHOLE pan and left `cams` empty,
            # which is the state the loud warning below reports. The pointer is
            # four bytes; reading it per step costs nothing and lets a pan
            # recover the moment the battle is back.
            # WAIT FOR THE VIEW OBJECT, which is not ready when in_gameplay
            # first says yes.
            #
            # in_gameplay tests DISPCNT, the map pointer and the map's
            # dimensions -- all of which are satisfied before the view object
            # at 02152034 is allocated. Four runs reported "view pointer not
            # readable" while the port's own log showed 89 fog scans and a live
            # camera in the same run, and the diagnostic that settled it was
            # printing the VALUE: the read succeeds and returns 00000000. The
            # battle is simply younger than the check.
            view = 0
            for _ in range(40):                            # up to ~10s
                view = struct_ptr(d, 0x02152034)
                if view:
                    break
                time.sleep(0.25)
            if not view:
                print("  pan: view object never appeared; panning blind",
                      flush=True)

            # CLEAR WHATEVER IS SWALLOWING THE INPUT, then check it worked.
            #
            # The pan is delivered as held KEYINPUT, and a modal -- a briefing,
            # a build menu, a confirmation -- consumes it, so the camera never
            # moves and the run produces hundreds of samples at the starting
            # position. Five runs went that way.
            #
            # B is the cancel button everywhere in this game's UI and, unlike a
            # tap, cannot issue a move order to a selected unit or select
            # something new. Press it, then PROVE the camera responds by
            # nudging and reading camx back rather than assuming -- an injected
            # input that silently does nothing is exactly what this is here to
            # detect.
            _panlog = os.path.join(out, "stderr.log")

            def _camx():
                # The PORT's camera, not a hardcoded pointer. See port_camx.
                c = port_camx(_panlog)
                if c is not None:
                    return c
                v = view or struct_ptr(d, 0x02152034)
                if not v:
                    return None
                b = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=v + 0x5E,
                                        len=2)["hex"])
                return int.from_bytes(b, "little", signed=True)

            before_nudge = _camx()
            if before_nudge is not None:
                d.cmd("keys", mask=0x3FF & ~args.pan_key)
                time.sleep(0.25)
                d.cmd("keys", mask=0x3FF)
                time.sleep(0.20)
                if _camx() == before_nudge:
                    print("  pan: camera did not respond; pressing B to clear "
                          "a modal and retrying", flush=True)
                    for _ in range(2):
                        d.cmd("keys", mask=0x3FF & ~0x02)     # B held
                        time.sleep(0.20)
                        d.cmd("keys", mask=0x3FF)
                        time.sleep(0.25)

            # One file per run, opened here so the pan loop can append to it.
            _polyf = (open(os.path.join(out, "polysample.jsonl"), "w")
                      if args.polysample else None)
            cams = []
            for i in range(args.pan_steps):
                c = port_camx(_panlog)
                if c is not None:
                    cams.append((c, 0))
                elif view:
                    b = bytes.fromhex(d.cmd("read_mem", cpu=9,
                                            addr=view + 0x5E, len=4)["hex"])
                    cams.append((int.from_bytes(b[0:2], "little", signed=True),
                                 int.from_bytes(b[2:4], "little", signed=True)))
                d.cmd("keys", mask=0x3FF & ~args.pan_key)
                # A FRAME WHILE THE CAMERA IS ACTUALLY MOVING.
                #
                # Every capture in this loop was taken 0.15 s AFTER the key was
                # released, so the pan sequence only ever showed settled
                # frames -- and those measure clean: the map's right green edge
                # translates by exactly -2x the camera delta (-192 for camdx
                # 96, -224 for 112). The defect is that panning the camera
                # causes a rendering collapse that drags on-screen terrain and
                # unit sprites with it, which is a description of what happens
                # DURING the motion. A settled frame cannot show it, so the
                # hold is split and a frame taken in the middle of it.
                # A BURST, NOT ONE FRAME.
                #
                # One mid-hold capture was not enough. Watching a run live, the
                # base and unit sprites shift with a camera move, visible where
                # they overlap the crystal trees -- so the fault is a RELATIVE
                # displacement between sprites and terrain, not the whole-frame
                # smear the first reading looked for. Measured in the single
                # mid-hold frame, a unit blob and the terrain blob both moved
                # dx=-374: together, no displacement. A transient that lasts a
                # frame or two cannot be caught by sampling once per 0.25 s
                # hold, so take several in quick succession and let the
                # comparison run over all of them.
                for _b in range(4):
                    capture(os.path.join(out, "panmid-%02d-%d.png" % (i, _b)))
                    # THE POLYGONS THEMSELVES, MID-HOLD.
                    #
                    # Every instrument used on the sprite drift so far reports
                    # a UNION -- lb_drift3d's bounding box over the whole 3D
                    # layer -- and the in-field HUD is 3D geometry that is
                    # screen-fixed, so the box says "nothing moved" whatever
                    # the units do. Three separate readings were taken from it
                    # and all three measured the HUD or two clocks.
                    #
                    # gx_polygons is the render list AFTER the viewport
                    # transform: one record per polygon, with the screen rect
                    # it occupies. That is per-object rather than per-frame,
                    # and it is the thing the clipper drops, so both questions
                    # -- "did this unit move relative to the ground" and "was
                    # it clipped away" -- are answerable from it directly.
                    #
                    # Sampled HERE, inside the mid-hold burst, because that is
                    # where the transient lives: the loop above already records
                    # that settled frames measure clean.
                    if args.polysample:
                        try:
                            gp = d.cmd("gx_polygons")
                            polys = gp.get("polygons", [])
                            rec = {
                                "step": i, "burst": _b,
                                "cam": port_camx(_panlog),
                                "count": gp.get("count", 0),
                                # Compact: the screen rect is what this is for.
                                "p": [[p.get("submission_index"),
                                       p.get("min_x"), p.get("max_x"),
                                       p.get("min_y"), p.get("max_y")]
                                      for p in polys],
                            }
                            _polyf.write(json.dumps(rec) + "\n")
                            _polyf.flush()
                        except Exception as exc:          # noqa: BLE001
                            print("polysample: %s" % exc, file=sys.stderr)
                time.sleep(args.pan_hold / 2.0)
                hofs.append(d.cmd("read_io", cpu=9, addr=0x04000014,
                                  width=16)["value"] & 0x1FF)
                d.cmd("keys", mask=0x3FF)
                time.sleep(0.15)
                # A capture after EVERY step. The base and its units start near
                # the middle of the native band, so they only occupy a widened
                # column briefly while the camera slides past -- a single
                # after-the-fact capture misses exactly the frame that proves
                # units render out there.
                # CAPTURE ON A VERIFIED CAMERA DELTA, NOT ON A STEP COUNT.
                #
                # docs/290 retracted a conclusion that rested on these frames
                # being "four frames of camera motion". They are not: a shift
                # finder validated on known offsets (-64, -16, 0, +24, +96, all
                # recovered exactly) reports +0 px between every pair, while the
                # run's own log shows the camera going 136 -> 265 -> 268 -> 272
                # -> 584. The camera moved BETWEEN the capture moments rather
                # than across them, so the pan worked and the sampling did not.
                #
                # A step counter cannot detect that; the camera can. Wait for it
                # to actually differ from the last captured value before taking
                # the shot, and SAY SO when it never does, so a run that cannot
                # support a crossing test reports that rather than producing
                # frames that silently look like one.
                prev_cam = cams[-1][0] if cams else None
                if prev_cam is not None:
                    _w = 0.0
                    while _w < 3.0:
                        _c = port_camx(_panlog)
                        if _c is not None and _c != prev_cam:
                            break
                        time.sleep(0.25)
                        _w += 0.25
                capture(os.path.join(out, "pan-%02d.png" % i))
                c = port_camx(_panlog)
                if c is not None:
                    if prev_cam is not None:
                        print("pan: step %d captured at cam %d (delta %+d)%s"
                              % (i, c, c - prev_cam,
                                 "  <- NO MOVEMENT: this pair cannot support "
                                 "a crossing test" if c == prev_cam else ""))
                    cams.append((c, 0))
                elif view:
                    b = bytes.fromhex(d.cmd("read_mem", cpu=9,
                                            addr=view + 0x5E, len=4)["hex"])
                    cams.append((int.from_bytes(b[0:2], "little", signed=True),
                                 int.from_bytes(b[2:4], "little", signed=True)))
                hofs.append(d.cmd("read_io", cpu=9, addr=0x04000014,
                                  width=16)["value"] & 0x1FF)
                # THE CULLING QUESTION. The widened surface is 448 and the
                # guest's own view is columns 96..351 of it. If the guest
                # submits geometry only for its own view, no polygon can ever
                # have an x outside that band; if melonDS's widened frustum is
                # already carrying units past it, some will. Panning walks
                # units toward the edge, which is the only way to ask.
                try:
                    polys = d.cmd("gx_polygons")["polygons"]
                    outside = [p for p in polys
                               if p["min_x"] < 96 or p["max_x"] > 351]
                    steps.append((i, len(polys), len(outside),
                                  min([p["min_x"] for p in polys], default=None),
                                  max([p["max_x"] for p in polys], default=None)))
                except Exception:                  # noqa: BLE001
                    pass
            # PAN BACK, AND DIFF AGAINST WHERE WE STARTED.
            #
            # The offset is STATIC -- it stays wrong after stopping, and it
            # happens throughout, with any movement -- so it does
            # not need frame-resolution capture at all. It needs the SAME camera
            # value photographed before and after a pan.
            #
            # Every earlier measurement compared two different camera positions,
            # where "sprites moved by the same amount as terrain" is consistent
            # with both a correct renderer and a drifting one. Returning the
            # camera to its start removes that: pan-before and pan-return are
            # the same world, the same camera and both settled, so anything that
            # differs between them is the defect and nothing else can be.
            #
            # The camera is READ BACK rather than assumed to have returned. A
            # hold does not travel a fixed distance -- measured, the same 0.25 s
            # press moved it 267 px once and 763 px another time -- so the two
            # legs will not cancel exactly and the residual has to be reported
            # with the pictures or the diff cannot be interpreted.
            if args.pan_return:
                # CLOSED LOOP, because a hold does not travel a fixed distance.
                #
                # Counting out the same number of presses does NOT come back:
                # measured, two 0.3 s presses right moved the camera 421 -> 1288
                # (clamped at the map's edge, so the leg was truncated) and two
                # back moved it only to 1088 -- a residual of 667, which makes
                # the two frames incomparable and the whole test worthless.
                #
                # So drive it by the camera instead of by the clock: tap the
                # opposite key and re-read until it is within a few pixels of
                # where it started, or give up and say so.
                back_key = 0x20 if args.pan_key == 0x10 else 0x10

                def _camx_now():
                    c = port_camx(_panlog)
                    if c is not None:
                        return c
                    if view:
                        b = bytes.fromhex(d.cmd("read_mem", cpu=9,
                                                addr=view + 0x5E,
                                                len=4)["hex"])
                        return int.from_bytes(b[0:2], "little", signed=True)
                    return None

                start_cam = cams[0][0] if cams else None
                back_cam = _camx_now()
                if start_cam is not None:
                    for _try in range(40):
                        back_cam = _camx_now()
                        if back_cam is None:
                            break
                        err = back_cam - start_cam
                        if abs(err) <= 4:
                            break
                        key = back_key if err > 0 else args.pan_key
                        # Shorten the press as it closes in, or it overshoots
                        # and oscillates instead of settling.
                        #
                        # NOT named `tap`. `tap` is a module-level FUNCTION
                        # used all over main(), and binding it here made it a
                        # local for the whole of main() -- so --build-flow,
                        # hundreds of lines earlier, died with
                        # "UnboundLocalError: cannot access local variable
                        # 'tap'". Python decides local-vs-global per function,
                        # not per line, so a single assignment anywhere breaks
                        # every earlier call.
                        press = 0.20 if abs(err) > 300 else (
                            0.08 if abs(err) > 80 else 0.03)
                        d.cmd("keys", mask=0x3FF & ~key)
                        time.sleep(press)
                        d.cmd("keys", mask=0x3FF)
                        time.sleep(0.18)
                    time.sleep(0.5)
                    back_cam = _camx_now()
                print("pan-return: camx %s -> %s (residual %s)"
                      % (start_cam, back_cam,
                         None if (start_cam is None or back_cam is None)
                         else back_cam - start_cam), flush=True)
                capture(os.path.join(out, "pan-return.png"))

            with open(os.path.join(out, "probe.txt"), "a") as fh:
                fh.write("  pan step: (i, polys, outside_native, min_x, max_x)\n")
                for s in steps:
                    fh.write("    %s\n" % (s,))
                # camx per capture, and how far each hold actually travelled.
                # This is what makes "put a unit in a widened column" a
                # measurement rather than a guess: with the gather rect now
                # covering camx-120..camx+352, the capture whose camx sits
                # 96..352 px past a known building is the one that should show
                # it in the extension.
                fh.write("  pan camera (px), hold=%.3fs key=0x%02X\n"
                         % (args.pan_hold, args.pan_key))
                for j, c in enumerate(cams):
                    prev = cams[j - 1][0] if j else None
                    fh.write("    %2d camx=%-6d camy=%-6d d=%s\n"
                             % (j, c[0], c[1],
                                "-" if prev is None else str(c[0] - prev)))
            # A PAN THAT DID NOT PAN IS A FAILED RUN, and it used to be a
            # silent one.
            #
            # The camera comes from view+0x5E, and `view` is read once before
            # the loop; when that pointer is stale -- the battle not actually
            # live at this moment, whatever in_gameplay said a few seconds
            # earlier -- `cams` stays empty, every key press lands nowhere, and
            # the run continues to completion and exits 0. Three separate
            # 25-minute runs were spent that way and their output looked
            # perfectly healthy: hundreds of fog samples, all of them taken at
            # the starting camera, so the search could never see a fog frontier
            # and nothing in the log said why.
            #
            # Say it, loudly, on stderr where the other failures go.
            if not cams:
                print("%s: PAN DID NOTHING -- view pointer at 02152034 was "
                      "not readable, so no camera was sampled and the key "
                      "presses went nowhere. Every measurement below is at "
                      "the starting camera." % args.label, file=sys.stderr)
            elif len(set(c[0] for c in cams)) == 1:
                print("%s: PAN DID NOT MOVE -- camx stayed at %d for all %d "
                      "steps (key 0x%02X, hold %.2fs). The guest ignored the "
                      "input; a modal dialog or a menu will do that."
                      % (args.label, cams[0][0], len(cams), args.pan_key,
                         args.pan_hold), file=sys.stderr)
            if cams:
                print("  pan camx: %s" % ", ".join(str(c[0]) for c in cams))
            for s in steps:
                print("  pan step %d: %d polys, %d outside native, x %s..%s"
                      % s)
            # THE FOG, AFTER MOVING AWAY FROM THE BASE.
            #
            # At match start the camera sits on the player's own base, which is
            # explored, so an all-clear mask there says nothing -- which is how
            # "there is no fog in these matches" got recorded. Fog can only show
            # up once the view contains ground the player has not seen.
            #
            # Mask: func_020EEBF4 -> row[tx>>3] & (1<<(tx&7)) via fog+0x14.
            # Output: func_0203D90C -> *(u32*)(0x02152090 + y*4) + x*2, and the
            # shroud pass writes tile index 100 there.
            fogline = []
            try:
                hdr = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=0x02151F28,
                                          len=0x18)["hex"])
                stride = int.from_bytes(hdr[4:8], "little")
                fw = int.from_bytes(hdr[8:12], "little")
                rowtab = int.from_bytes(hdr[20:24], "little")
                ptrs = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=rowtab,
                                           len=96)["hex"])
                setbits = 0
                for ty in range(24):
                    rp = int.from_bytes(ptrs[ty * 4:ty * 4 + 4], "little")
                    if not (0x02000000 <= rp < 0x02400000):
                        continue
                    rb = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=rp,
                                             len=max(stride, 1))["hex"])
                    row = "".join("#" if (rb[tx >> 3] >> (tx & 7)) & 1 else "."
                                  for tx in range(min(fw, stride * 8)))
                    setbits += row.count("#")
                    if ty in (0, 6, 12, 18):
                        fogline.append("    mask row %2d: %s" % (ty, row))
                fogline.insert(0, "  fog after pan: %d bits set" % setbits)
                fogline.insert(1, "  fog rowtable %08X first rows %s"
                               % (rowtab, " ".join(
                                   "%08X" % int.from_bytes(
                                       ptrs[i*4:i*4+4], "little")
                                   for i in range(3))))

                st = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=0x02152090,
                                         len=96)["hex"])
                shroud = 0
                for ty in range(24):
                    rp = int.from_bytes(st[ty * 4:ty * 4 + 4], "little")
                    if not (0x02000000 <= rp < 0x02400000):
                        continue
                    rb = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=rp,
                                             len=64)["hex"])
                    shroud += sum(
                        1 for i in range(0, 64, 2)
                        if int.from_bytes(rb[i:i + 2], "little") == 100)
                fogline.append("  shroud rowtable @02152090 first rows %s"
                               % " ".join("%08X" % int.from_bytes(
                                   st[i*4:i*4+4], "little")
                                   for i in range(3)))
                fogline.append("  shroud tiles (index 100) in buffer: %d"
                               % shroud)

                # WHAT IS ACTUALLY ON SCREEN.
                #
                # The two probes above read main RAM through symbols recovered
                # from the cartridge, and both say "no fog" -- while the capture
                # taken at the same instant is wall-to-wall grey cloud. So read
                # the PICTURE'S OWN SOURCE instead: engine A's BG tilemaps live
                # in bank E, at the screen bases the registers name (BG2 0x0000,
                # BG1 0x0800, BG3 0x1000). Whichever block is dominated by one
                # repeated tile index in a fogged view IS the shroud.
                for name, base in (("BG2", 0x0000), ("BG1", 0x0800),
                                   ("BG3", 0x1000)):
                    blk = bytes.fromhex(d.cmd("vram", bank=4, off=base,
                                              len=2048)["hex"])
                    hist = {}
                    for i in range(0, 2048, 2):
                        e = int.from_bytes(blk[i:i + 2], "little")
                        hist[e] = hist.get(e, 0) + 1
                    top = sorted(hist.items(), key=lambda kv: -kv[1])[:4]
                    fogline.append(
                        "  %s @%04X: %d distinct, top %s"
                        % (name, base, len(hist),
                           " ".join("%04X x%d" % (e, n) for e, n in top)))
                    # BG3 is the shroud: func_020396E0's third pass writes tile
                    # 0x1F for every tile the visibility test flags, and the
                    # histogram above finds exactly 32*24 of them in a fogged
                    # view. Draw it, so the SHAPE of the fog can be compared
                    # against the camera rather than only its quantity.
                    # BG2 is the shroud -- masking DISPCNT bit 10 removes the
                    # fog completely and leaves clean terrain across the whole
                    # widened window. Print its entries so the pattern behind
                    # those ~31 distinct cloud tiles can be read off directly.
                # The camera, and the scroll registers for all four layers.
                # func_020396E0 writes BGxOFS from the camera's sub-tile
                # remainder, so the mapping from a hardware tile column to an
                # absolute map tile cannot be assumed to be camera/8 alone.
                view = int.from_bytes(bytes.fromhex(
                    d.cmd("read_mem", cpu=9, addr=0x02152034, len=4)["hex"]),
                    "little")
                cam = bytes.fromhex(d.cmd("read_mem", cpu=9, addr=view + 0x5E,
                                          len=4)["hex"])
                camx = int.from_bytes(cam[0:2], "little", signed=True)
                camy = int.from_bytes(cam[2:4], "little", signed=True)
                ofs = []
                for bg in range(4):
                    h = d.cmd("read_io", cpu=9,
                              addr=0x04000010 + bg * 4, width=16)["value"]
                    v = d.cmd("read_io", cpu=9,
                              addr=0x04000012 + bg * 4, width=16)["value"]
                    ofs.append("BG%d %d,%d" % (bg, h, v))
                fogline.append("  cam %d,%d (tile %d,%d)  %s"
                               % (camx, camy, camx >> 3, camy >> 3,
                                  "  ".join(ofs)))
            except Exception as exc:               # noqa: BLE001
                fogline.append("  fog-after-pan probe failed: %s" % exc)
            for ln in fogline:
                print(ln)

            with open(os.path.join(out, "probe.txt"), "a") as fh:
                fh.write("  BG1HOFS during pan: %s\n" % (hofs,))
                fh.write(os.linesep.join(fogline) + os.linesep)
            print("%s: BG1HOFS during pan %s" % (args.label, hofs))
            time.sleep(1.0)
            capture(os.path.join(out, "pan-after.png"))
            # Geometry after the camera moved. A polygon that keeps its screen
            # X while the world slides past it is screen-space by observation,
            # which is a far better basis for anchoring it than a z constant
            # that happened to hold in one frame.
            try:
                with open(os.path.join(out, "gx_panned.json"), "w") as fh:
                    fh.write(repr(d.cmd("gx_polygons")))
            except Exception:                      # noqa: BLE001
                pass

        if args.cull_ab:
            # A/B/A/B the entity-cull width with the camera parked so units sit
            # just outside the view. The first attempt at this ran with two
            # emulator instances alive and is withdrawn (docs/198); this one
            # runs under assert_single_runner and confirms gameplay on BOTH
            # sides of every reading.
            lines = []
            for width in (256, 448, 256, 448):
                try:
                    lb_probe_cull(d, width)
                except Exception as exc:           # noqa: BLE001
                    lines.append("  width %d: patch failed: %s" % (width, exc))
                    continue
                time.sleep(2.0)
                live = in_gameplay(d)
                polys = d.cmd("gx_polygons")["polygons"]
                outside = [q for q in polys
                           if q["min_x"] < 96 or q["max_x"] > 351]
                lines.append(
                    "  width %3d -> %2d polys, %d outside native, "
                    "in_gameplay=%s" % (width, len(polys), len(outside), live))
            with open(os.path.join(out, "cull_ab.txt"), "w") as fh:
                fh.write(os.linesep.join(lines) + os.linesep)
            for ln in lines:
                print(ln)

        # Capture EVERY size, and report the gameplay check rather than obeying
        # it. It is a colour census, and once the camera is over unexplored
        # ground the field is wall-to-wall grey fog -- so it says "left
        # gameplay" and the sweep used to break after the first size, losing
        # exactly the captures that show the widened fog at different window
        # widths. Same lesson as the tap retries: a heuristic gets to inform the
        # log, not to decide what the run does.
        # DOES A BIGGER WINDOW BUY MORE FIELD, OR A BIGGER PICTURE?
        #
        # The requirement: the game size must scale properly and extend with
        # window size, not just stretch to fit, so a larger screen sees more
        # instead of the game being simply blown up. The sweep captured an image at each size and said nothing
        # about the one number that answers that -- the GUEST SURFACE. A window
        # twice as wide showing the same 256 guest columns is a picture blown
        # up; one showing 512 is more battlefield.
        #
        # So report it, with the magnification (window px per guest px) beside
        # it. Constant magnification across the sweep is the property being
        # claimed; where it rises, the renderer has hit the 512x256 vertex
        # format ceiling and is scaling instead, and the log should say so
        # rather than leave it to be discovered from a screenshot.
        def geom_now():
            try:
                with open(os.path.join(out, "stderr.log"), "rb") as fh:
                    t = fh.read()[-200000:].decode("latin-1", "replace")
            except OSError:
                return None
            import re as _re
            ls = [l for l in t.splitlines() if l.startswith("[lb_geom]")]
            if not ls:
                return None
            m = _re.search(r"window=(\d+)x(\d+) guest=(\d+)x(\d+)", ls[-1])
            return tuple(int(v) for v in m.groups()) if m else None

        print("%s: window -> guest surface (magnification)" % args.label)
        for size in SIZES:
            w, h = (int(v) for v in size.split("x"))
            # RESIZE, DO NOT PHOTOGRAPH. The numbers below are the point of
            # this sweep; the eight stretched PNGs it used to leave behind are
            # not, and nothing reads them. --size-shots brings
            # them back for the rare case where the picture at a size is the
            # question.
            if args.size_shots:
                capture(os.path.join(out, size + ".png"), w, h)
            else:
                resize_window(w, h)
            time.sleep(1.5)
            g = geom_now()
            if g:
                ww, wh, gw2, gh2 = g
                # THREE numbers, because two of them can each look fine while
                # the pair is wrong. Magnification is window pixels per guest
                # pixel: it is the "is the picture blown up" number, and it
                # should stay CONSTANT down the sweep. Field of view is
                # guest pixels over the DS's own 256x192: it is the "does a
                # bigger screen show more" number, and it should be
                # RISING. A run where magnification climbs and field of view
                # stays at 1.00x is precisely the reported fault, and it is
                # not visible from the guest size alone.
                print("%s:   %-9s -> %dx%d guest  mag %.2fx/%.2fx  fov "
                      "%.2fx/%.2fx%s"
                      % (args.label, size, gw2, gh2, ww / float(gw2),
                         wh / float(gh2), gw2 / 256.0, gh2 / 192.0,
                         "   CEILING" if gw2 >= 1024 or gh2 >= 512 else ""))
            if not in_gameplay(d):
                print("%s: gameplay check negative at %s (fog reads as "
                      "non-gameplay; capture kept)" % (args.label, size),
                      file=sys.stderr)

        if args.keep:
            d.close()               # one client at a time; see the note above
            print("READY: gameplay is live on port %d; probe it, then kill "
                  "nds_runner.exe" % PORT, flush=True)
            while proc.poll() is None:
                time.sleep(5)
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()

        # A MISSING READOUT IS A QUESTION, NOT A PASS.
        #
        # The try this closes has no except clause, so anything the probes
        # raise -- a Dbg TimeoutError on the debug socket is the one that
        # actually happens -- unwinds THROUGH here and lands as a traceback on
        # this process's stderr, which a batch loop routinely discards. When
        # that happened, exp-e946_2 and _3 printed "linger: 15s in gameplay",
        # then "converted 4 ppm dump(s)" from this very block, and nothing in
        # between: no fps.txt, no tier3 line, no resize table, no explanation.
        # The arm reported three samples and had one. Write it where the
        # reader of the results is already looking.
        _exc = sys.exc_info()[1]
        if _exc is not None and not isinstance(_exc, (KeyboardInterrupt,
                                                      SystemExit)):
            try:
                with open(os.path.join(out, "FAILED.txt"), "w") as fh:
                    fh.write("".join(traceback.format_exception(
                        type(_exc), _exc, _exc.__traceback__)))
            except Exception:                           # noqa: BLE001
                pass
            print("%s: RUN INCOMPLETE after the route -- %s: %s  "
                  "(traceback in %s/FAILED.txt)"
                  % (args.label, type(_exc).__name__, _exc, out),
                  file=sys.stderr)

        # CONVERT THIS RUN'S .ppm DUMPS BEFORE LEAVING.
        #
        # LB_DUMP_ADAPTIVE writes an uncompressed P6 every 30 ticks in a
        # four-deep ring. That is small per run and enormous in aggregate: 7926
        # files, 3.33 GB, across 690 runs -- 72% of the whole repository, which
        # tools/lb_prune_shots.py had to reclaim in one pass.
        #
        # Cleaning up afterwards does not stop it coming back, so convert here
        # instead. PNG is lossless, roughly 25x smaller on this material, and
        # unlike PPM it previews in a file browser, which is how these are
        # actually looked at. A source is removed only after its
        # PNG has been read back and confirmed identical.
        try:
            import lb_prune_shots as _prune
            n = kept = 0
            for _b, _d, _fs in os.walk(out):
                for _f in _fs:
                    if not _f.endswith(".ppm"):
                        continue
                    src = os.path.join(_b, _f)
                    img = _prune.read_ppm(src)
                    if img is None:
                        kept += 1
                        continue
                    w, h, rgb = img
                    dst = src[:-4] + ".png"
                    _prune.write_png(dst, w, h, rgb)
                    if _prune.read_png_rgb(dst) == (w, h, rgb):
                        os.remove(src)
                        n += 1
                    else:
                        kept += 1
            if n or kept:
                print("%s: converted %d ppm dump(s) to png%s"
                      % (args.label, n,
                         ", left %d unconverted" % kept if kept else ""))
        except Exception as exc:                        # noqa: BLE001
            print("%s: ppm conversion skipped: %s" % (args.label, exc))


if __name__ == "__main__":
    raise SystemExit(main())
