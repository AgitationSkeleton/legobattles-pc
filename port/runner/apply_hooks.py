#!/usr/bin/env python3
"""Insert the LEGO Battles overlay hooks into a COPY of the ndsrecomp runner.

The upstream tree is never modified. port/runner/build.sh copies it to a build
directory and runs this over the copy, so every change this port makes to the
runner lives in legobattles-decomp and is reviewable here.

Anchors are matched by exact text and each one MUST appear exactly once. A
missing or duplicated anchor is a hard error, never a silent skip: upstream may
change, and a hook that quietly fails to apply would produce a runner that
builds and shows no UI, which is the worst possible failure mode. Compare
docs/91, where a CMake glob that matched nothing cost eleven minutes of link
time to diagnose.

    python apply_hooks.py <build-tree-runner-dir> <imgui-dir>
"""

from __future__ import annotations

import hashlib
import json
import os
import sys

# EVERY FILE THIS SCRIPT WRITES, so their mtimes can be made content-derived.
#
# WHY. build.sh rebuilds the tree from pristine upstream each time and this
# script re-patches it, so every patched file gets a fresh mtime on every build.
# One of them is recompiler/armv4t/runtime_arm.h, and all 302 files in
# port/generated/ include it -- so make correctly invalidated every lifted bank
# on every build. Measured: 23 bank objects in 25 minutes at LB_JOBS=1, i.e.
# about five hours for a full pass, for a one-line change to an ImGui panel.
#
# The fix is NOT to pin the mtime to the upstream file's, which is the obvious
# version and is unsafe: edit this script and the patched output changes while
# the timestamp does not, make skips the rebuild, and the binary silently does
# not contain the change. This project has already lost time to exactly that
# class of fault ("bash port/build.sh from PowerShell silently runs the old
# binary").
#
# Content addressing is safe. After patching, hash each written file; if the
# hash matches what was recorded last build, restore that build's mtime, so
# make sees no change because there IS no change. If the hash differs -- for any
# reason, including an edit to this script -- the file keeps its new mtime and
# everything downstream rebuilds. The cache is a hint that can be deleted at any
# time; losing it costs one full rebuild and nothing else.
_written = set()


def _stamp_cache_path():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        ".hookstamps.json")


def stamp_written():
    """Give unchanged patched files their previous mtime. Returns a summary."""
    path = _stamp_cache_path()
    try:
        with open(path, "r", encoding="utf-8") as fh:
            cache = json.load(fh)
    except Exception:                                    # noqa: BLE001
        cache = {}

    held, bumped = 0, 0
    for target in sorted(_written):
        try:
            with open(target, "rb") as fh:
                digest = hashlib.sha1(fh.read()).hexdigest()
        except OSError:
            continue
        # Canonical key. os.path.normcase lowercases on Windows but does NOT
        # reconcile separators here: build.sh hands this script MSYS-style
        # forward-slash paths, while a path built from a plain relative string
        # comes back with backslashes. The first cache written had both spellings
        # of runtime_arm.h in it and therefore never hit.
        key = os.path.normcase(os.path.abspath(target)).replace("\\", "/")
        prev = cache.get(key)
        if prev and prev[0] == digest:
            try:
                os.utime(target, ns=(int(prev[1]), int(prev[1])))
                held += 1
                continue
            except OSError:
                pass
        try:
            cache[key] = [digest, os.stat(target).st_mtime_ns]
            bumped += 1
        except OSError:
            pass

    try:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(cache, fh, indent=1, sort_keys=True)
    except OSError:
        pass
    return "mtimes: %d unchanged (held), %d new (rebuild downstream)" % (
        held, bumped)


class HookError(RuntimeError):
    pass


def edit(path, anchor, insert, *, after=True, indent=""):
    """Insert `insert` before/after the unique line containing `anchor`."""
    with open(path, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()

    count = text.count(anchor)
    if count != 1:
        raise HookError(
            "anchor appears %d times, expected exactly 1, in %s\n    %s"
            % (count, os.path.basename(path), anchor))

    start = text.index(anchor)
    line_start = text.rfind("\n", 0, start) + 1
    line_end = text.index("\n", start) + 1
    # Indent from the anchor LINE's own indentation, not from the text before
    # the anchor within it. When the anchor sits on a continuation line -- as
    # the two-line set_touch_from_mouse signature does -- the latter would
    # inherit the argument alignment and produce a 30-column indent.
    stripped = text[line_start:line_end]
    lead = stripped[:len(stripped) - len(stripped.lstrip())]
    block = "".join("%s%s%s\n" % (lead, indent, ln) for ln in insert)

    if after:
        text = text[:line_end] + block + text[line_end:]
    else:
        text = text[:line_start] + block + text[line_start:]

    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    _written.add(os.path.abspath(path))
    return True


def replace(path, old, new):
    """Replace the unique occurrence of `old` with `new`."""
    with open(path, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    count = text.count(old)
    if count != 1:
        raise HookError(
            "text appears %d times, expected exactly 1, in %s\n    %s"
            % (count, os.path.basename(path), old))
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace(old, new))
    _written.add(os.path.abspath(path))
    return True


def apply(runner_dir, imgui_dir):
    front = os.path.join(runner_dir, "src", "frontend.cpp")
    gpu = os.path.join(runner_dir, "src", "gpu2d.cpp")
    mainc = os.path.join(runner_dir, "src", "main.cpp")
    bus = os.path.join(runner_dir, "src", "bus.cpp")
    rtarm = os.path.join(runner_dir, "src", "runtime_arm.cpp")
    cmake = os.path.join(runner_dir, "CMakeLists.txt")
    backup = os.path.join(runner_dir, "src", "cart_backup.cpp")
    io = os.path.join(runner_dir, "src", "io.cpp")
    for p in (front, cmake, backup, io):
        if not os.path.isfile(p):
            raise HookError("not found: %s" % p)

    done = []


    # NOTE: an earlier hook here widened EEPROM addressing to 3 bytes at
    # exactly 64 KB, on the theory that upstream's "> 65536" boundary
    # excluded the 512 Kbit chip it should include. That is probably a real
    # upstream off-by-one, but LEGO Battles does not use that chip (it is
    # eeprom-tiny, 512 bytes), so the change fixed nothing here and could
    # not be verified. Removed rather than carried on speculation.
    #
    # 0b. Save-chip tracing, opt-in via LB_TRACE_SAVE=1. The save chip is
    #     invisible from the outside: every candidate type reaches the same 107
    #     code pages because the game loops on its error screen either way, so
    #     the only way to tell a correct chip from a wrong one is to watch the
    #     transactions.
    replace(backup,
            "            default: chip.cmd = value; chip.addr = 0; break;",
            "            default: {\n"
            "                chip.cmd = value; chip.addr = 0;\n"
            "                /* legobattles-decomp: LB_TRACE_SAVE=1 */\n"
            "                static int lb_n = -1;\n"
            "                if (lb_n < 0) lb_n = std::getenv(\"LB_TRACE_SAVE\") ? 0 : 1000000;\n"
            "                if (lb_n < 64) {\n"
            "                    ++lb_n;\n"
            "                    std::fprintf(stderr, \"[lb_save] cmd=0x%02X size=%u\\n\",\n"
            "                                 (unsigned)value,\n"
            "                                 (unsigned)chip.sram.size());\n"
            "                }\n"
            "                break;\n"
            "            }")
    replace(backup, "#include \"cart_backup.h\"",
            "#include \"cart_backup.h\"\n#include <cstdio>\n#include <cstdlib>"
            "   /* legobattles-decomp */")
    done.append("cart_backup.cpp: LB_TRACE_SAVE command tracing")

    # 0c. Batch-mode auto-driver, opt-in via LB_AUTOPLAY=1.
    #
    #     A cycle-budget run presses no buttons, so the game reaches the title
    #     screen and stops there forever waiting for Start. That makes
    #     everything past the title unverifiable without a human at the
    #     keyboard, which is no way to check a front end for regressions.
    #
    #     This taps VBlank -- once per frame, at VCOUNT 192 -- and holds Start
    #     and A on a slow cycle. It is a TEST HARNESS, off unless the
    #     environment variable is set, and it is deliberately dumb: it cannot
    #     "play" the game, only walk menus that advance on Start or A.
    replace(io, "        } else if (g_vcount == 192u) {\n"
                "            g_in_vblank = true;",
            "        } else if (g_vcount == 192u) {\n"
            "            g_in_vblank = true;\n"
            "            lb_patches_on_vblank();  /* legobattles-decomp */\n"
            "            {   /* legobattles-decomp: LB_AUTOPLAY=1 */\n"
            "                static int lb_auto = -1;\n"
            "                static unsigned lb_frame = 0;\n"
            "                if (lb_auto < 0)\n"
            "                    lb_auto = std::getenv(\"LB_AUTOPLAY\") ? 1 : 0;\n"
            "                if (lb_auto) {\n"
            "                    ++lb_frame;\n"
            "                    /* KEYINPUT is ACTIVE LOW: a set bit is a key\n"
            "                       that is NOT pressed. frontend.cpp releases\n"
            "                       everything with nds_set_key_mask(0x0FFF).\n"
            "                       Starting from 0 holds all twelve buttons\n"
            "                       down forever, which is how the first\n"
            "                       version of this hook failed. */\n"
            "                    uint32_t lb_keys = 0x0FFFu;\n"
            "                    if (lb_frame > 180u) {\n"
            "                        const unsigned ph = lb_frame % 60u;\n"
            "                        if (ph < 8u)        lb_keys &= ~(1u << 3);\n"
            "                        else if (ph >= 30u && ph < 38u) lb_keys &= ~(1u << 0);\n"
            "                    }\n"
            "                    nds_set_key_mask(lb_keys);\n"
            "                    /* LEGO Battles is a DS game and its front end\n"
            "                       is driven by the stylus, so buttons alone do\n"
            "                       not leave the title screen. Tap the middle of\n"
            "                       the touch screen on the same slow cycle. */\n"
            "                    if (lb_frame > 180u) {\n"
            "                        const unsigned tp = lb_frame % 60u;\n"
            "                        if (tp >= 45u && tp < 52u)\n"
            "                            nds_set_touch(128, 96, true);\n"
            "                        else\n"
            "                            nds_set_touch(0, 0, false);\n"
            "                    }\n"
            "                }\n"
            "            }")
    replace(io, "// (0x04000006). The display controller raises the VBlank/HBlank/VCount IRQ",
            "#include <cstdlib>   /* legobattles-decomp: LB_AUTOPLAY */\n"
            "/* legobattles-decomp: forward declaration, NOT the header.\n"
            "   lb_overlay.h pulls in SDL.h, and including that here breaks\n"
            "   io.cpp's include order with a wall of errors inside cmath. */\n"
            "extern \"C\" void lb_patches_on_vblank(void);\n"
            "// (0x04000006). The display controller raises the VBlank/HBlank/VCount IRQ")
    done.append("io.cpp: LB_AUTOPLAY front-end driver")

    # 1. The include.
    edit(front, '#include "relative_mouse_touch.h"',
         ['#include "lb_overlay.h"   /* legobattles-decomp */'])
    done.append("frontend.cpp: include")

    # 1b. ...AND THE TEARDOWN, which had no caller at all.
    #
    #     lb_overlay_shutdown() is complete -- it saves the settings, destroys
    #     the two SDL textures, shuts both ImGui backends down and destroys the
    #     context -- and nothing in the runner invoked it, so none of that ever
    #     ran. Found by the ImGui coverage sweep: it is an exported entry point
    #     declared in lb_overlay.h with zero call sites, which is the same
    #     "present but never reached" shape as an unwired setting.
    #
    #     Nothing was visibly broken by it, and that is worth writing down so
    #     the fix is not oversold: settings still persist, because
    #     lb_overlay_draw autosaves whenever the settings hash changes, and the
    #     process was exiting anyway so the OS reclaimed the textures. What was
    #     actually wrong is that the last few frames' worth of changes depended
    #     on that autosave having fired, and an exported shutdown that never
    #     runs is a trap for whoever adds something to it next.
    #
    #     HERE rather than beside SDL_Quit, because the overlay owns SDL
    #     textures created from presentation.renderers[0]. Destroying the
    #     renderer first would leave lb_overlay_shutdown freeing textures whose
    #     renderer is gone. It guards on g_ready, so calling it when the overlay
    #     never initialised -- the headless and early-exit paths -- is a no-op.
    replace(front,
            "void destroy_presentation(FrontendPresentation& presentation) {",
            "/* legobattles-decomp: the frame counter every other trace is\n"
            "   keyed by, so a dumped frame can name which one it was. AT FILE\n"
            "   SCOPE because a linkage specification is illegal inside a\n"
            "   function -- the first two attempts at this put it in the block\n"
            "   and cost a build each. */\n"
            "extern \"C\" unsigned lb_dump_frame_get(void);\n"
            "extern \"C\" int lb_widemap_cam_px(int* x, int* y);\n"
            "/* docs/342: the 3D layer exactly as composited, for a pixel\n"
            "   gate that can see composite-time changes. */\n"
            "extern \"C\" int lb_dump3d_on(void);\n"
            "extern \"C\" const uint32_t* lb_used3d_rgb(int* w, int* h);\n"
            "\n"
            "/* LB_DUMP_ONMOVE=1: record ONLY frames that have both camera\n"
            "   motion and geometry on screen.\n"
            "\n"
            "   WHY IT EXISTS. Six attempts to measure the jitter in pixels\n"
            "   failed because the recording window never contained the case.\n"
            "   Asking demos/user-combat-2026-09-14 for frames with BOTH a\n"
            "   camera change and >= 10 polygons gives THREE, in 18,733 frames:\n"
            "   when the camera moves there are no units on screen, and when\n"
            "   units are on screen the camera is still. No choice of\n"
            "   LB_DUMP_START finds a window that does not exist.\n"
            "\n"
            "   So stop choosing a window. This makes the dump seek its own\n"
            "   frames: the sequence fills only from frames that can actually\n"
            "   show the effect, however far apart they are.\n"
            "\n"
            "   NOT CONSECUTIVE ANY MORE, and that is the trade. A per-frame\n"
            "   delta needs adjacent frames, so a gap between two recorded\n"
            "   frames makes their difference meaningless -- which is why the\n"
            "   frame number goes in [lb_dumpidx] and the analysis must check\n"
            "   adjacency rather than assume it. */\n"
            "static bool lb_dump_wanted(void) {\n"
            "    static int on = -1;\n"
            "    if (on < 0) {\n"
            "        const char* v = std::getenv(\"LB_DUMP_ONMOVE\");\n"
            "        on = v ? std::atoi(v) : 0;\n"
            "        if (on < 0) on = 0;\n"
            "    }\n"
            "    if (!on) return true;\n"
            "    /* LB_DUMP_ONMOVE=2 drops the geometry requirement. For the\n"
            "       docs/337 re-present correction only CAMERA MOTION matters:\n"
            "       the raster persists between submissions, so a frame that\n"
            "       submitted nothing still shows sprites and is exactly the\n"
            "       frame the correction acts on. Demanding >= 10 polygons as\n"
            "       well yielded 2-3 frames per run, which cannot grade\n"
            "       anything. */\n"
            "    if (on < 2 && nds_gpu3d_render_polygon_count() < 10u)\n"
            "        return false;\n"
            "    int cx = 0, cy = 0;\n"
            "    if (!lb_widemap_cam_px(&cx, &cy)) return false;\n"
            "    static int px = INT_MIN, py = INT_MIN;\n"
            "    const bool moved = (px != INT_MIN) && (cx != px || cy != py);\n"
            "    px = cx;\n"
            "    py = cy;\n"
            "    /* LB_DUMP_ONMOVE=3: LATCH on the first motion, then record\n"
            "       every frame. docs/342: the camera steps on alternate\n"
            "       updates (16, 0, 16, 0), so a one-frame lag between ground\n"
            "       and sprites shows ONLY between adjacent frames and cancels\n"
            "       over any even interval. Mode 2 records motion frames\n"
            "       alone, which on that cadence are never adjacent. */\n"
            "    if (on >= 3) {\n"
            "        static bool latched = false;\n"
            "        if (moved) latched = true;\n"
            "        return latched;\n"
            "    }\n"
            "    return moved;\n"
            "}\n"
            "\n"
            "void destroy_presentation(FrontendPresentation& presentation) {\n"
            "    /* legobattles-decomp: see apply_hooks.py. Before the\n"
            "       renderer that owns the overlay's textures. */\n"
            "    lb_overlay_shutdown();")
    done.append("frontend.cpp: the overlay teardown finally has a caller")

    # 2. Event forwarding. This must come first inside the poll loop so a click
    #    on a panel is not also delivered to the guest as a stylus tap. The
    #    port makes the main window the touch screen and clickable, so this
    #    ordering is a correctness requirement rather than a preference.
    edit(front, "while (SDL_PollEvent(&event)) {",
         ["if (lb_overlay_event(&event)) continue;  /* legobattles-decomp */"],
         indent="    ")
    done.append("frontend.cpp: event forwarding")

    # 3. Belt and braces on the guest touch feed. lb_overlay_event already
    #    swallows captured mouse events, but set_touch_from_mouse is reachable
    #    from three call sites and from replay/self-test paths, so gate the
    #    function itself as well. Releasing the touch rather than simply
    #    returning avoids leaving a stylus stuck down when the pointer moves
    #    onto a panel mid-drag.
    edit(front, "NdsScreenLayout layout, int logical_width) {",
         ["if (lb_overlay_wants_mouse()) {  /* legobattles-decomp */",
          "    nds_set_touch(0, 0, false);",
          "    return;",
          "}",
          "// ENHANCED KEEPS ONE COORDINATE SPACE: WINDOW PIXELS.",
          "//",
          "// SDL_RenderSetLogicalSize scales mouse events as well as drawing,",
          "// so while it was set three parties disagreed about what an (x, y)",
          "// meant. SDL delivered logical coordinates and the guest mapping",
          "// expected them, but ImGui got them too while believing its display",
          "// was the whole window. That put the Top Screen panel off the bottom",
          "// right of the window, and made ImGui believe the cursor was over a",
          "// panel when it was over the game -- so the gate above swallowed the",
          "// click and the touch screen went dead. Enhanced now sets no logical",
          "// size and maps through the rect the field was actually drawn into.",
          "// Where the last press landed, so a release can be delivered there",
          "// rather than at the origin. See the note on the release path.",
          "static int lb_last_touch_x = 0;",
          "static int lb_last_touch_y = 0;",
          "static int lb_last_touch_ext = 0;",
          "if (lb_setting_enhanced()) {",
          "    int fx = 0, fy = 0, fw = 0, fh = 0, gw = kScreenWidth;",
          "    int gh = kScreenHeight;",
          "    if (lb_overlay_field_rect(&fx, &fy, &fw, &fh, &gw, &gh) &&",
          "        fw > 0 && fh > 0) {",
          "        // A RELEASE KEEPS THE POSITION IT WAS RELEASED AT.",
          "        //",
          "        // This used to release with nds_set_touch(0, 0, false), and",
          "        // that one detail is why nothing could be selected through",
          "        // the frontend. Measured, one target, one frame: the debug",
          "        // `touch` command delivered (147, 147, 95) and SELECTED,",
          "        // while this path delivered (146, 146, 94) -- the same",
          "        // point to a pixel -- and selected nothing. The difference",
          "        // is not what arrives while the pen is down, it is what",
          "        // arrives when it lifts: `touch` releases at the same x, y",
          "        // and this zeroed them. The cartridge acts on the release",
          "        // edge, so it was reading a tap at the top-left corner.",
          "        //",
          "        // The extended coordinate is kept for the same reason. It",
          "        // is the whole out-of-band feature: clearing it on release",
          "        // would hand the game the CLAMPED x on precisely the frame",
          "        // it reads one, so a click in the widened band would arrive",
          "        // as a click at native 0. The next press overwrites it, so",
          "        // nothing goes stale.",
          "        if (!down || window_x < fx || window_x >= fx + fw ||",
          "            window_y < fy || window_y >= fy + fh) {",
          "            // The port's own selection box ends with the press.",
          "            // Same edge the cartridge acts on, so the two cannot",
          "            // disagree about when the drag stopped.",
          "            lb_marquee_release();",
          "            lb_touch_set_extended(lb_last_touch_ext);",
          "            nds_set_touch((uint16_t)lb_last_touch_x,",
          "                          (uint16_t)lb_last_touch_y, false);",
          "            return;",
          "        }",
          "        const float gx =",
          "            (float)(window_x - fx) * (float)gw / (float)fw;",
          "        // THE GUEST SURFACE'S OWN HEIGHT, not the DS's 192.",
          "        //",
          "        // This divided by kScreenHeight, which was right only while",
          "        // the surface was always 192 rows tall. Once the window buys",
          "        // rows too (docs/215) a 256-row surface mapped every click",
          "        // 33% too far down the field, so a tap landed a third of a",
          "        // screen below where the player aimed.",
          "        const float gy =",
          "            (float)(window_y - fy) * (float)gh / (float)fh;",
          "        // THE BOX THE PLAYER SEES, IN SURFACE COORDINATES.",
          "        //",
          "        // Published from gx/gy rather than from the native",
          "        // coordinate handed to the cartridge, and BEFORE the HUD",
          "        // unmapper touches them: the box has to be drawn where the",
          "        // cursor is, and the cartridge's own copy cannot leave the",
          "        // native 256 columns at all -- it is an 8x8 tilemap on a",
          "        // 32x24 grid. See lb_patches.cpp and docs/276.",
          "        lb_marquee_move((int)gx, (int)gy);",
          "        // The guest still believes its screen is 256x192: extra",
          "        // adaptive columns and rows are extra VIEW, not extra touch,",
          "        // so a band has to come off before the guest sees the click.",
          "        // WHICH BAND IS NOT A CHOICE -- IT IS WHEREVER THE PICTURE",
          "        // WAS DRAWN. This subtracted the CENTRED split, (gw-256)/2,",
          "        // and the compositor draws the guest's columns at",
          "        // lb_band_left(), which is clamped by the map edges and is",
          "        // only the centred value when the camera is far from both.",
          "        // Measured on a 1024-wide surface with the camera near the",
          "        // left edge: the picture is drawn at 132 and this subtracted",
          "        // 384, so every click arrived 252 px from where it was",
          "        // aimed. A probe could not see it because the probe aimed",
          "        // through the same wrong band it was testing (docs/274);",
          "        // pressing at a chosen pixel and measuring where the game",
          "        // DREW the box is what caught it. See docs/275.",
          "        const float band = (float)(gw - kScreenWidth) / 2.0f;",
          "        const float bandy = (float)lb_band_top((int)gh);",
          "        // ...but a HUD button this port MOVED has to be clickable",
          "        // where it now is. lb_hud_unmap_touch reports the click at",
          "        // the element's original position, so the cartridge's own",
          "        // hit test finds it unchanged. Without this, anchoring the",
          "        // HUD to the widened edges made it unclickable -- the click",
          "        // arrived outside the panel and was discarded.",
          "        int gxi = (int)gx;",
          "        int gyi = (int)gy;",
          "        if (!lb_hud_unmap_touch(&gxi, &gyi) &&",
          "            !lb_touch_is_down() &&",
          "            lb_hud_touch_phantom(gxi, gyi)) {",
          "            /* legobattles-decomp, phantom presses: this point is",
          "               the empty patch a MOVED element used to occupy. The",
          "               cartridge's hit test is still there and would press a",
          "               button the player cannot see. Drop the delivery: a",
          "               touch that did not happen is what their eyes already",
          "               believe. Checked only when the unmap found nothing,",
          "               so a real click on a moved element still wins, and",
          "               only on the PRESS edge -- dropping a drag sample",
          "               would freeze the guest's touch partway through a",
          "               selection rectangle rather than refuse a click. The",
          "               release path returns above this, so a release is",
          "               never dropped and a press can never stick. */",
          "            return;",
          "        }",
          "        const float gxa = (float)gxi;",
          "        const float gya = (float)gyi;",
          "        // THE CLICK IS NO LONGER THROWN AWAY OUT HERE.",
          "        //",
          "        // A unit standing in a widened column is at a screen x the",
          "        // DS panel cannot express, so this used to drop the click",
          "        // and the extra field of view was look-only. The true",
          "        // coordinate - negative left of the native band, past 255",
          "        // right of it - is published for the guest to read through",
          "        // the repointed literal in lb_patches.cpp, while the touch",
          "        // controller itself still gets a value inside the panel so",
          "        // pen-down state and everything vanilla stays correct.",
          "        // SHOW THE ARITHMETIC. LB_TOUCHLOG=1.",
          "        //",
          "        // A probe measured this mapping delivering x = 0 for a",
          "        // click that should have been x = 146, with the y correct",
          "        // -- so every click through the frontend was landing in the",
          "        // far-left column, and a target whose hit area covers x = 0",
          "        // was being 'selected' no matter where the click was aimed.",
          "        // Each term was guessed at in turn and none of the guesses",
          "        // held, so print them instead: the window pixel, the rect",
          "        // it is measured against, the surface coordinate, what the",
          "        // HUD unmapper did to it, and the band that is subtracted.",
          "        if (std::getenv(\"LB_TOUCHLOG\")) {",
          "            std::fprintf(stderr,",
          "                \"[lb_map] win=%d,%d field=%d,%d %dx%d guest=%dx%d\"",
          "                \" gx=%.1f gy=%.1f unmapped=%d,%d band=%.1f,%.1f\"",
          "                \" -> ext=%d native=%d,%d\\n\",",
          "                window_x, window_y, fx, fy, fw, fh, gw, gh,",
          "                (double)gx, (double)gy, gxi, gyi, (double)band,",
          "                (double)bandy, (int)(gxa - band),",
          "                (int)(gxa - band) < 0 ? 0 : (int)(gxa - band),",
          "                (int)(gya - bandy));",
          "            std::fflush(stderr);",
          "        }",
          "        lb_last_touch_ext = (int)(gxa - band);",
          "        lb_touch_set_extended(lb_last_touch_ext);",
          "        /* THE VERTICAL TWIN. The clamped value below still goes to",
          "           nds_set_touch, because the emulated panel only accepts",
          "           0..191 -- exactly as X does. The TRUE row is published",
          "           here instead, and the guest reads it from the recogniser",
          "           shadow at +0x12/+0x14 rather than from the panel. Without",
          "           this the clamp threw the out-of-band remainder away and a",
          "           click above the band arrived as row 0. */",
          "        lb_touch_set_extended_y((int)(gya - bandy));",
          "        lb_last_touch_x = std::clamp<int>((int)(gxa - band), 0,",
          "                                          kScreenWidth - 1);",
          "        lb_last_touch_y = std::clamp<int>((int)(gya - bandy), 0,",
          "                                          kScreenHeight - 1);",
          "        nds_set_touch((uint16_t)lb_last_touch_x,",
          "                      (uint16_t)lb_last_touch_y, true);",
          "        return;",
          "    }",
          "}"],
         indent="    ")
    done.append("frontend.cpp: guest touch gate + enhanced touch mapping")

    # 3a2. Route the MOUSEBUTTONDOWN gate through the drawn field rect.
    #
    #      The shipped gate accepts a 256-column band at bottom_content_left
    #      and, in the stacked layout, only y below the top screen. Both are
    #      LOGICAL space tests. Enhanced no longer sets a logical size, so
    #      events arrive in window pixels and the field spans the whole
    #      window: the band then accepts a narrow strip and silently rejects
    #      every click outside it, which is why the touch screen looked dead.
    #      Vanilla is unchanged -- lb_overlay_mouse_in_touch_area reproduces
    #      the original test exactly when Enhanced is off.
    # Present only the columns the guest actually rendered.
    #
    # The texture is created once at the MAXIMUM adaptive width, but the surface
    # is now sized to the window's aspect each frame, so the columns past it hold
    # whatever was there last. Copying the whole texture would smear those onto
    # the screen; a source rect shows exactly what was drawn.
    replace(front,
            "void render_screen(FrontendPresentation& presentation, int screen,\n"
            "                   const SDL_Rect& destination) {",
            "int lb_present_src_w[2] = {0, 0};   /* legobattles-decomp */\n"
            "int lb_present_src_h[2] = {0, 0};   /* legobattles-decomp */\n"
            "void render_screen(FrontendPresentation& presentation, int screen,\n"
            "                   const SDL_Rect& destination) {")
    replace(front,
            "    SDL_RenderCopy(renderer, source, nullptr, &destination);\n"
            "}",
            "    /* legobattles-decomp: only the part the guest actually drew.\n"
            "       The texture is the MAXIMUM size and SDL_UpdateTexture is\n"
            "       given the guest's own pitch, so texture row r begins exactly\n"
            "       at source row r and the first lb_present_src_w columns of\n"
            "       every row are correct; everything past them is overlap from\n"
            "       the next row, and everything below src_h is last frame's.\n"
            "       This rect is what keeps both out of the picture. */\n"
            "    /* BOTH SCREENS, NOT JUST THE FIELD.\n"
            "       This read `screen == 1`, so the top screen was copied\n"
            "       whole. Every texture is created screen_widths x 768\n"
            "       (kMaxAdaptiveHeight) while the top LCD only ever fills\n"
            "       192 rows, so the whole-texture copy squeezed those 192\n"
            "       rows into the top quarter of the destination and filled\n"
            "       the remaining 576 with never-written memory. In Enhanced\n"
            "       the top screen goes to the ImGui panel and nobody saw it;\n"
            "       with Enhanced OFF both screens are drawn inline and it is\n"
            "       the whole picture -- HUD strip, field strip, black band\n"
            "       and a block of RGB noise crammed into one 192-row rect.\n"
            "       That was reported as \"vanilla rendering is broken\", and\n"
            "       the guest was never at fault: the port's own 256x192 dumps\n"
            "       from the same frames are clean. */\n"
            "    if (screen >= 0 && screen < 2 && lb_present_src_w[screen] > 0) {\n"
            "        int lb_tw = 0, lb_th = 0;\n"
            "        SDL_QueryTexture(source, nullptr, nullptr, &lb_tw, &lb_th);\n"
            "        const int lb_sh = lb_present_src_h[screen] > 0\n"
            "                              ? lb_present_src_h[screen] : lb_th;\n"
            "        if (lb_present_src_w[screen] < lb_tw || lb_sh < lb_th) {\n"
            "            const SDL_Rect lb_src{0, 0,\n"
            "                                  lb_present_src_w[screen], lb_sh};\n"
            "            SDL_RenderCopy(renderer, source, &lb_src, &destination);\n"
            "            return;\n"
            "        }\n"
            "    }\n"
            "    SDL_RenderCopy(renderer, source, nullptr, &destination);\n"
            "}")
    done.append("frontend.cpp: present only the rendered columns")

    # ...and say how much of EACH texture was actually written.
    #
    # The Enhanced path sets index 1 from nds_gpu3d_output_*, which describes
    # the field surface and nothing else. The top screen had no source extent
    # at all, so render_screen fell through to a whole-texture copy of a
    # 768-row texture holding 192 rows of picture.
    #
    # The height rule is not invented here: the dump code a few lines above
    # already states it, because it hit the same trap --
    #
    #     "The height is only the adaptive one when the surface is genuinely
    #      wide; a native 256 buffer is a 192-row Frame and reading lb_dh rows
    #      from it would run off the end -- the same fault as the top_width
    #      lie."
    #
    # so width > 256 ? adaptive height : 192, per screen, from the same
    # top_width/bottom_width the uploads are given.
    replace(front,
            "    uint64_t start = SDL_GetPerformanceCounter();\n"
            "    SDL_UpdateTexture(presentation.textures[0], nullptr, top_pixels,\n"
            "                      top_width * sizeof(uint32_t));\n"
            "    SDL_UpdateTexture(presentation.textures[1], nullptr, bottom_pixels,\n"
            "                      bottom_width * sizeof(uint32_t));",
            "    {   /* legobattles-decomp: how much of each texture is real */\n"
            "        const int lb_ah = (int)nds_gpu3d_output_height();\n"
            "        lb_present_src_w[0] = (int)top_width;\n"
            "        lb_present_src_h[0] = (int)top_width > 256 ? lb_ah : 192;\n"
            "        lb_present_src_w[1] = (int)bottom_width;\n"
            "        lb_present_src_h[1] = (int)bottom_width > 256 ? lb_ah : 192;\n"
            "    }\n"
            "    uint64_t start = SDL_GetPerformanceCounter();\n"
            "    SDL_UpdateTexture(presentation.textures[0], nullptr, top_pixels,\n"
            "                      top_width * sizeof(uint32_t));\n"
            "    SDL_UpdateTexture(presentation.textures[1], nullptr, bottom_pixels,\n"
            "                      bottom_width * sizeof(uint32_t));")
    done.append("frontend.cpp: per-screen source extents")

    # ...and size the VANILLA destination rects to what was drawn, not to the
    # adaptive maximum.
    #
    # presentation.screen_widths[] is assigned once, at presentation creation,
    # from options.adaptive_max_width[] -- 512 here, which is the
    # "adaptive_width=512" the banner reports. It is an option, not the
    # Enhanced setting, so with Enhanced OFF the bottom screen still got a
    # 512-wide destination rect while the guest was drawing 256 columns into
    # it. The source rect fix above stopped the garbage; this is the other half
    # of the same report, the bit that made the two screens come out at
    # different widths (measured 520 and 1010 window px for the same 256-column
    # source, exactly 2:1).
    #
    # Only the non-Enhanced branch uses these two rects -- the Enhanced path a
    # few lines below computes its own -- so this cannot disturb the widened
    # layout.
    replace(front,
            "        const SDL_Rect top_rect{\n"
            "            (presentation.canvas_width -\n"
            "             presentation.screen_widths[0]) / 2,\n"
            "            0, presentation.screen_widths[0], kScreenHeight};\n"
            "        const SDL_Rect bottom_rect{\n"
            "            (presentation.canvas_width -\n"
            "             presentation.screen_widths[1]) / 2,\n"
            "            kScreenHeight, presentation.screen_widths[1], kScreenHeight};",
            "        /* legobattles-decomp: the width the guest actually drew,\n"
            "           falling back to the configured one before the first\n"
            "           frame has set it. */\n"
            "        const int lb_w0 = lb_present_src_w[0] > 0\n"
            "            ? lb_present_src_w[0] : presentation.screen_widths[0];\n"
            "        const int lb_w1 = lb_present_src_w[1] > 0\n"
            "            ? lb_present_src_w[1] : presentation.screen_widths[1];\n"
            "        const SDL_Rect top_rect{\n"
            "            (presentation.canvas_width - lb_w0) / 2,\n"
            "            0, lb_w0, kScreenHeight};\n"
            "        const SDL_Rect bottom_rect{\n"
            "            (presentation.canvas_width - lb_w1) / 2,\n"
            "            kScreenHeight, lb_w1, kScreenHeight};")
    done.append("frontend.cpp: vanilla destination rects match the source")

    # Note the NATIVE touch x AND y the runner is delivering, from any source.
    #
    # The touch patches in lb_patches.cpp repoint the game's own sampler at a
    # slot the port writes, so the sampled value no longer reaches the struct by
    # itself -- which makes "fall back to reading the struct" circular: it
    # re-publishes the last value forever and the coordinate freezes. The first
    # build did exactly that and every tap in the route landed at a stale x.
    #
    # Noting it here covers both the frontend mapping and the debug `touch`
    # command, because both end up in nds_set_touch.
    replace(io,
            "void nds_set_touch(uint16_t x, uint16_t y, bool down) {",
            "extern \"C\" void lb_touch_note_native(int x, int y, int down);\n"
            "extern \"C\" int lb_demo_intercept_touch(uint16_t* x, uint16_t* y,\n"
            "                                        int* down);\n"
            "void nds_set_touch(uint16_t x, uint16_t y, bool down) {\n"
            "    /* legobattles-decomp: record or replay. This is the boundary\n"
            "       a demo is taken at -- what the CONSOLE received -- because\n"
            "       window pixels would tie a recording to the window size it\n"
            "       was made at and SDL events would tie it to the layout, the\n"
            "       HUD anchor and the panel positions. See src/lb_demo.cpp. */\n"
            "    {\n"
            "        int lb_down = down ? 1 : 0;\n"
            "        if (!lb_demo_intercept_touch(&x, &y, &lb_down)) return;\n"
            "        down = lb_down != 0;\n"
            "    }\n"
            "    /* legobattles-decomp docs/344: THE PANEL IS 256x192, ALWAYS.\n"
            "       A real touch screen cannot report a pixel it does not\n"
            "       have, and the cartridge assumes it: several routines use\n"
            "       the touch row as a tile-row index into 24-row tables.\n"
            "       Clamped here, after the demo intercept, so every path that\n"
            "       reaches the console -- mouse, debug `touch`, replays of\n"
            "       older demos -- obeys the hardware's range.\n"
            "\n"
            "       NOT THE DRAG-SELECT CRASH FIX, although it was written as\n"
            "       one. Measured: with only this clamp, a drag to native y 230\n"
            "       still corrupted ITCM. The out-of-range row reaches the game\n"
            "       through the port's extended-Y channel, which bypasses the\n"
            "       panel; the fix is in marquee_observer (lb_patches.cpp). This\n"
            "       stays because it is correct on its own terms. */\n"
            "    if (x > 255u) x = 255u;\n"
            "    if (y > 191u) y = 191u;\n"
            "    lb_touch_note_native((int)x, (int)y, down ? 1 : 0);"
            "   /* legobattles-decomp */")
    replace(io,
            "void nds_set_key_mask(uint32_t mask) {",
            "extern \"C\" int lb_demo_intercept_keys(uint32_t* mask);\n"
            "void nds_set_key_mask(uint32_t mask) {\n"
            "    /* legobattles-decomp: see above. */\n"
            "    if (!lb_demo_intercept_keys(&mask)) return;")
    done.append("io.cpp: note the native touch x, and record/replay input")

    replace(front,
            "                event.button.x >= bottom_content_left &&\n"
            "                event.button.x < bottom_content_left + kScreenWidth &&\n"
            "                (presentation.separate ||\n"
            "                 event.button.y >= kScreenHeight)) {",
            "                lb_overlay_mouse_in_touch_area(  /* legobattles-decomp */\n"
            "                    event.button.x, event.button.y,\n"
            "                    bottom_content_left,\n"
            "                    presentation.separate ? 1 : 0)) {")
    done.append("frontend.cpp: mouse-down gate uses the field rect")

    # 4a. gpu2d.cpp: the 2D counterpart of the adaptive compositor.
    #
    #      nds_gpu2d_adaptive_framebuffer returns early for any engine but A,
    #      centring the native image and leaving the extra columns black, and
    #      its supported-scene test additionally requires bg0_3d. It is a
    #      3D-plus-HUD widener. LEGO Battles' field is a 2D tilemap on the
    #      BOTTOM screen -- engine B, which has no 3D -- so the screen the
    #      port needs widened took the letterbox branch every frame.
    #
    #      lb_wide2d.inc is included rather than pasted here because it needs
    #      gpu2d.cpp's private helpers (Unit, decode_bg_line, compose).
    edit(gpu, "void render_engine_line(int engine, int y) {",
         ['#include "lb_wide2d.inc"  /* legobattles-decomp */', ""],
         after=False)
    done.append("gpu2d.cpp: include the 2D widener")

    replace(gpu,
            "    if (engine != 0) {\n"
            "        for (int y = 0; y < 192; ++y)\n"
            "            std::copy_n(native + y * 256, 256,\n"
            "                        adaptive.data() + y * output_width + extra);\n"
            "        return adaptive.data();\n"
            "    }",
            "    if (engine != 0) {\n"
            "        // legobattles-decomp: widen a pure-2D scene rather than\n"
            "        // letterboxing it. See src/lb_wide2d.inc and docs/107.\n"
            "        Unit& lb_u = g_unit[engine];\n"
            "        const uint8_t* const lb_pal = nds_vram_renderer_palette(engine);\n"
            "        const uint8_t* const lb_oam = nds_vram_renderer_oam(engine);\n"
            "        const NdsVramRendererView* const lb_vram =\n"
            "            nds_vram_renderer_view(engine);\n"
            "        if (lb_wide2d_supported(engine, lb_u, lb_pal, lb_oam, lb_vram)) {\n"
            "            // CENTRED VERTICALLY, not parked at the top. The guest\n"
            "            // authors 192 rows; a taller output puts them in the\n"
            "            // middle and carries the edge rows outward, exactly as\n"
            "            // the widened columns carry the edge columns. extra_y\n"
            "            // is 0 at height 192, so this is the same picture as\n"
            "            // before until the window asks for more.\n"
            "            for (int y = 0; y < 192; ++y)\n"
            "                lb_wide2d_line(engine, y, lb_u, lb_pal, lb_oam, *lb_vram,\n"
            "                               adaptive.data() +\n"
            "                                   (y + extra_y) * output_width,\n"
            "                               output_width, extra);\n"
            "            lb_fill_rows_outward(adaptive.data(), output_width,\n"
            "                                 output_height, extra_y);\n"
            "            return adaptive.data();\n"
            "        }\n"
            "        // legobattles-decomp: menus cannot be widened truthfully;\n"
            "        // fill rather than letterbox. See lb_menu_fill.\n"
            "        lb_menu_fill(native, adaptive.data(), output_width,\n"
            "                     output_height, extra, extra_y);\n"
            "        return adaptive.data();\n"
            "    }"
            )
    # 4c. Raise the adaptive width ceiling from 448 to 1024.
    #
    #     docs/166 found three independent 448s: kMaxAdaptiveWidth (which SIZES
    #     the adaptive framebuffer arrays, so it is an allocation and not a
    #     policy knob), the 3D output-width validator, and the config parser.
    #     The battlefield is at most 96x96 tiles = 768x768 pixels (docs/15), so
    #     1728 now, raised again so a 3440-wide window is not clamped;
    #     see the note beside lb_want for why that number.
    replace(gpu, "constexpr int kMaxAdaptiveWidth = 448;",
                 "constexpr int kMaxAdaptiveWidth = 1728;"
                 "   /* legobattles-decomp: docs/166 */")
    done.append("gpu2d.cpp: adaptive width ceiling 448 -> 1728")
    # 0f. The adaptive widener must honour WHICH SCREENS the title allows.
    #
    #     port/game.toml says `adaptive_widescreen = "bottom"` because in LEGO
    #     Battles the battlefield is the touch screen. frontend.cpp respects
    #     that, but nds_gpu2d_adaptive_framebuffer() itself does not -- it
    #     widens whichever screen it is handed. Any OTHER caller therefore
    #     widens the top screen too, and the debug server's `framebuffer`
    #     command is exactly such a caller.
    #
    #     That produced screenshots of a stretched top-screen menu -- the LEGO
    #     BATTLES banner pulled across 448 columns with its panels smeared --
    #     which looked like a rendering defect in the port and was not: the game
    #     never draws that. The capture did.
    #
    #     Enforcing the mask inside the widener makes "bottom only" a property
    #     of the renderer rather than a convention each caller has to remember.
    # 0e2. THE SCREEN SWAP IS DISALLOWED IN ENHANCED.
    #
    #      POWCNT1 bit 15 routes engine A to the top LCD when set and the
    #      bottom when clear, and LEGO BATTLES FLIPS IT BETWEEN STATES:
    #      measured 0x820F at the title and 0x020F in a battle. On a DS that
    #      is correct and invisible -- both screens are in front of you, and
    #      the picture simply moves to the other one.
    #
    #      In the enhanced single-window layout it is NOT invisible. The
    #      window draws one physical screen, so the moment the guest flips
    #      that bit the window shows the other engine: the field is replaced
    #      by the HUD, or a menu by the minimap: the screens appear to switch
    #      places. The swap is therefore disallowed outright while adaptive
    #      screen size is on.
    #
    #      So in enhanced mode the routing is PINNED: engine A always lands on
    #      the buffer the window presents. Nothing writes to POWCNT1 and the
    #      guest's own view of its registers is untouched -- this changes only
    #      which physical buffer each engine's scanlines are filed under, which
    #      is a presentation choice the host is entitled to make. Vanilla is
    #      unaffected and still follows the bit exactly.
    #
    #      Both routing sites go through one helper so they cannot disagree:
    #      the scanline write that files a rendered line under a screen, and
    #      the adaptive compositor that maps a screen back to its engine.
    replace(gpu, "const uint32_t* nds_gpu2d_adaptive_framebuffer(int screen, uint16_t* width) {\n"
                 "    const uint32_t* native = nds_gpu2d_framebuffer(screen);",
            "static uint8_t g_lb_adaptive_screens = 0xFFu;   /* legobattles-decomp */\n"
            "void nds_gpu2d_set_adaptive_screens(uint8_t mask) {\n"
            "    g_lb_adaptive_screens = mask;\n"
            "}\n"
            "/* legobattles-decomp: the band the LAST composite actually used. */\n"
            "int g_lb_used_extra = 0, g_lb_used_extra_y = 0;\n"
            "/* docs/348: for the touch mapping, which must take off the band of\n"
            "   the frame on screen (frontend.cpp is in its own namespace, so it\n"
            "   reads them through C linkage rather than naming them). */\n"
            "extern \"C\" int lb_shown_band_x(void) { return g_lb_used_extra; }\n"
            "extern \"C\" int lb_shown_band_y(void) { return g_lb_used_extra_y; }\n"
            "\n"
            "/* THE BAND THE SPRITES MUST BE PLACED AGAINST -- item #5.\n"
            "\n"
            "   render_obj_line() used to compute its own band with\n"
            "   lb_band_left(out_width). That is the LATCH, and the compositor\n"
            "   shifts the 2D layers by g_wide_3d_band[g_front] -- the band that\n"
            "   came back WITH the 3D snapshot, two hand-offs further down the\n"
            "   chain (latch -> for_geometry -> promote -> stored beside the\n"
            "   snapshot). The chain is deliberately built that way so the 2D\n"
            "   layers cannot shear away from the 3D lines they are composited\n"
            "   with; the sprite path simply never joined it.\n"
            "\n"
            "   Measured with LB_DRIFTLOG over 8,697 settled frames of a pan:\n"
            "   the two disagree on 13.2% of them. At the 946-wide gameplay\n"
            "   surface it is 5.3% of frames, and the difference is +-345 -- the\n"
            "   FULL half-overhang, so a sprite lands a whole band away from the\n"
            "   ground it is standing on. At 512 wide it is every single frame,\n"
            "   always -128. That is 'the sprites of the base and units shifted\n"
            "   with the camera, identifiable by overlapping the crystal trees'.\n"
            "\n"
            "   Valid only while a composite is actually in progress. Cleared\n"
            "   afterwards so prepare_direct_frame(), which renders objects at a\n"
            "   different moment with a band of its own, keeps its own answer\n"
            "   instead of inheriting a stale one -- the exact class of bug this\n"
            "   is fixing. */\n"
            "int g_lb_obj_band = 0;\n"
            "bool g_lb_obj_band_valid = false;\n"
            "\n"
            "/* THE DEBUG ASPECT BOX (LB_ASPECTBOX=1).\n"
            "\n"
            "   A temporary yellow outline of the original DS touch screen's\n"
            "   aspect, to identify and measure where, why and how entities\n"
            "   outside it do not render correctly.\n"
            "\n"
            "   It is drawn LAST, over the finished surface, so it marks the\n"
            "   guest's own 256x192 in the coordinates the picture is actually\n"
            "   presented in. Anything that draws only inside this rectangle is\n"
            "   clipped to the DS view; anything that crosses it is genuinely\n"
            "   reaching the widened columns. That is the measurement, and it is\n"
            "   one no log line can give -- every previous attempt to settle\n"
            "   \"do sprites reach past the native aspect\" argued from polygon\n"
            "   extents and layer dumps and produced four retracted answers.\n"
            "\n"
            "   Off unless the variable is set: it is a diagnostic overlay, not\n"
            "   part of the picture. */\n"
            "static bool lb_aspect_box_on() {\n"
            "    static const bool on = [] {\n"
            "        const char* v = std::getenv(\"LB_ASPECTBOX\");\n"
            "        return v && v[0] != '0';\n"
            "    }();\n"
            "    return on;\n"
            "}\n"
            "static void lb_draw_aspect_box(uint32_t* px, int w, int h) {\n"
            "    if (!px || w <= 256 || !lb_aspect_box_on()) return;\n"
            "    const int x0 = g_lb_used_extra;\n"
            "    const int y0 = g_lb_used_extra_y;\n"
            "    const int x1 = x0 + 255, y1 = y0 + 191;\n"
            "    const uint32_t c = 0xFFFFFF00u;   /* opaque yellow */\n"
            "    /* Clipped on every edge: at a map corner the band is pinned to\n"
            "       0 and the box can sit flush with the surface edge. */\n"
            "    for (int x = x0; x <= x1; ++x) {\n"
            "        if (x < 0 || x >= w) continue;\n"
            "        if (y0 >= 0 && y0 < h) px[y0 * w + x] = c;\n"
            "        if (y1 >= 0 && y1 < h) px[y1 * w + x] = c;\n"
            "    }\n"
            "    for (int y = y0; y <= y1; ++y) {\n"
            "        if (y < 0 || y >= h) continue;\n"
            "        if (x0 >= 0 && x0 < w) px[y * w + x0] = c;\n"
            "        if (x1 >= 0 && x1 < w) px[y * w + x1] = c;\n"
            "    }\n"
            "}\n"
            "/* THE DRAG SELECTION BOX.\n"
            "\n"
            "   Drawn here, beside the aspect box, for the same reason: this is\n"
            "   the finished surface, in the coordinates the picture is actually\n"
            "   presented in, so a rectangle put here lands exactly where the\n"
            "   player pointed with no band to get wrong.\n"
            "\n"
            "   The cartridge's own box cannot be used. It is an 8x8 TILEMAP on\n"
            "   the guest's 32x24 screen grid, clamped into 0..255 before it is\n"
            "   drawn, so it can never leave the native columns however correct\n"
            "   the coordinates fed to it are -- measured in docs/275, where the\n"
            "   drawn anchor tracked the extended x modulo 256 exactly. lb_patches\n"
            "   blanks it and publishes when it would have been up. */\n"
            "extern \"C\" int lb_marquee_get(int* x0, int* y0, int* x1, int* y1);\n"
            "static void lb_draw_marquee(uint32_t* px, int w, int h) {\n"
            "    if (!px || w <= 256) return;\n"
            "    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;\n"
            "    if (!lb_marquee_get(&x0, &y0, &x1, &y1)) return;\n"
            "    /* A press with no drag yet is not a box. The cartridge has its\n"
            "       own threshold before it shows one; this only has to avoid\n"
            "       drawing a single pixel on every tap. */\n"
            "    if (x1 - x0 < 2 && y1 - y0 < 2) return;\n"
            "    /* The cartridge's own box is YELLOW -- read off a capture\n"
            "       taken while both were on screen, the guest's clamped one\n"
            "       inside the band beside this one at the press. Matching it\n"
            "       is the difference between replacing the box and adding a\n"
            "       second, differently-coloured one. */\n"
            "    const uint32_t c = 0xFFFFFF00u;   /* opaque yellow */\n"
            "    for (int x = x0; x <= x1; ++x) {\n"
            "        if (x < 0 || x >= w) continue;\n"
            "        if (y0 >= 0 && y0 < h) px[y0 * w + x] = c;\n"
            "        if (y1 >= 0 && y1 < h) px[y1 * w + x] = c;\n"
            "    }\n"
            "    for (int y = y0; y <= y1; ++y) {\n"
            "        if (y < 0 || y >= h) continue;\n"
            "        if (x0 >= 0 && x0 < w) px[y * w + x0] = c;\n"
            "        if (x1 >= 0 && x1 < w) px[y * w + x1] = c;\n"
            "    }\n"
            "}\n"
            "static const uint32_t* lb_adaptive_fb_inner(int screen,\n"
            "                                           uint16_t* width);\n"
            "const uint32_t* nds_gpu2d_adaptive_framebuffer(int screen, uint16_t* width) {\n"
            "    uint16_t w = 256;\n"
            "    const uint32_t* r = lb_adaptive_fb_inner(screen, &w);\n"
            "    if (width) *width = w;\n"
            "    /* NEVER on the native buffer: that pointer is g_fb itself, the\n"
            "       guest's own framebuffer, and drawing into it would corrupt\n"
            "       the frame rather than annotate a copy of it. A widened\n"
            "       surface is always the scratch AdaptiveFrame. */\n"
            "    if (w > 256) {\n"
            "        lb_draw_aspect_box(const_cast<uint32_t*>(r), (int)w,\n"
            "                           (int)nds_gpu3d_output_height());\n"
            "        lb_draw_marquee(const_cast<uint32_t*>(r), (int)w,\n"
            "                        (int)nds_gpu3d_output_height());\n"
            "    }\n"
            "    return r;\n"
            "}\n"
            "static const uint32_t* lb_adaptive_fb_inner(int screen, uint16_t* width) {\n"
            "    const uint32_t* native = nds_gpu2d_framebuffer(screen);\n"
            "    /* legobattles-decomp: a screen the title does not widen must\n"
            "       come back native, whoever asked. */\n"
            "    if ((g_lb_adaptive_screens & (1u << (screen & 1))) == 0u) {\n"
            "        if (width) *width = 256;\n"
            "        return native;\n"
            "    }")
    done.append("gpu2d.cpp: adaptive widener honours the configured screens")

    # The helper itself, defined ahead of BOTH routing sites -- the scanline
    # write is far earlier in the file than the compositor.
    replace(gpu,
            "std::array<std::array<Frame, 2>, 2> g_fb{}; // [buffer][screen], 0xFFRRGGBB",
            "std::array<std::array<Frame, 2>, 2> g_fb{}; // [buffer][screen], 0xFFRRGGBB\n"
            "/* legobattles-decomp: see apply_hooks 0e2. Enhanced pins engine A\n"
            "   to the bottom screen, so the guest flipping POWCNT1 bit 15 cannot\n"
            "   swap what the single window is showing. Vanilla follows the bit. */\n"
            "extern \"C\" int lb_setting_enhanced(void);\n"
            "extern \"C\" int lb_screens_swapped(void);   /* SG-0012 */\n"
            "static bool lb_engine_a_on_top(void) {\n"
            "    const bool swapped = lb_screens_swapped() != 0;\n"
            "    if (lb_setting_enhanced()) return swapped;\n"
            "    return ((nds_powercontrol9() & 0x8000u) != 0) != swapped;\n"
            "}")

    # Both routing sites, through the one helper. See 0e2.
    replace(gpu,
            "    const bool engine_a_on_top = (nds_powercontrol9() & 0x8000u) != 0;\n"
            "    const int screen = engine_a_on_top ? engine : (engine ^ 1);",
            "    const bool engine_a_on_top = lb_engine_a_on_top();"
            "   /* legobattles-decomp */\n"
            "    const int screen = engine_a_on_top ? engine : (engine ^ 1);")
    replace(gpu,
            "    const bool engine_a_on_top = (nds_powercontrol9() & 0x8000u) != 0;\n"
            "    const int engine = engine_a_on_top ? screen : (screen ^ 1);",
            "    const bool engine_a_on_top = lb_engine_a_on_top();"
            "   /* legobattles-decomp */\n"
            "    const int engine = engine_a_on_top ? screen : (screen ^ 1);")
    done.append("gpu2d.cpp: enhanced pins the engine/screen routing")

    # And tell it what the title allows, once the config and CLI have both
    # had their say. serve mode reaches the widener through the debug
    # server rather than the frontend, so this must not sit in the
    # interactive path.
    edit(os.path.join(runner_dir, "src", "main.cpp"),
         "    bool adaptive_sky_repair = true;",
         ["    nds_gpu2d_set_adaptive_screens(",
          "        (uint8_t)frontend_options.adaptive_screens);"],
         after=False)

    edit(os.path.join(runner_dir, "src", "gpu2d.h"),
         "const uint32_t* nds_gpu2d_adaptive_framebuffer(int screen, uint16_t* width);",
         ["void nds_gpu2d_set_adaptive_screens(uint8_t mask);"])

    # 0g. The OBJ HUD anchor is enabled only for TOP-screen wideners.
    #
    #     main.cpp gates nds_gpu2d_set_adaptive_hud_anchor() on
    #     NDS_ADAPTIVE_TOP. Both upstream titles that use it -- Metroid Prime
    #     Hunters and SM64DS -- widen the top screen, so the condition reads as
    #     "is widening on" there. LEGO Battles widens the BOTTOM screen, so the
    #     option could never take effect: measured with the config key set true
    #     and false, the HUD sprites landed at identical columns both times.
    #
    #     The rendering side is already correct for this title. That path is
    #     gated on engine == 0, and engine A is the one with the widened
    #     surface whichever screen it happens to drive -- here the touch
    #     screen, in MPH the top one. Only the enable condition assumed a
    #     screen.
    replace(mainc,
            "    nds_gpu2d_set_adaptive_hud_anchor(\n"
            "        frontend_options.adaptive_hud_anchor &&\n"
            "        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u,",
            "    nds_gpu2d_set_adaptive_hud_anchor(\n"
            "        frontend_options.adaptive_hud_anchor &&\n"
            "        (frontend_options.adaptive_screens &\n"
            "         (NDS_ADAPTIVE_TOP | NDS_ADAPTIVE_BOTTOM)) != 0u,")
    done.append("main.cpp: HUD anchor follows whichever screen is widened")

    # 0h. Double-clicking the executable ran ANOTHER PORT'S launcher.
    #
    #     main.cpp has a no-argument path so the exe works on its own, but it is
    #     the shared runner's, written for a sibling port whose layout is
    #     <root>/port/... . It resolved `root` as the PARENT of the executable's
    #     folder, looked for game.toml and saves under root/port, forced
    #     --screen-layout separate over whatever the config asked for, and told
    #     the player to supply a DIFFERENT GAME's cartridge by name.
    #
    #     So running build/LegoBattles.exe searched the repository root for a
    #     ROM, found none, printed the wrong title and exited -- which from the
    #     outside is indistinguishable from the executable instantly closing.
    #
    #     Everything now resolves BESIDE the executable, which is what the
    #     build/ deployment actually looks like.
    replace(mainc,
            "        const fs::path root = exe.parent_path();\n"
            "        const fs::path port = root / \"port\";",
            "        const fs::path root = exe;  /* legobattles-decomp: beside\n"
            "                                       the executable, not above it */\n"
            "        const fs::path port = root;")

    replace(mainc,
            "                \"Put a LEGO Star Wars II: The Original Trilogy (USA) cartridge\\n\"\n"
            "                \"dump there and run this again. That is the only thing this\\n\"\n"
            "                \"port needs.\\n\",",
            "                \"Put your own LEGO Battles cartridge dump there and run this\\n\"\n"
            "                \"again. It is the only thing this port needs. mods/ is for\\n\"\n"
            "                \"custom map data, not for ROMs to run.\\n\",")

    #     Named after the cartridge, so a romhack cannot overwrite the vanilla
    #     save -- the same rule play.bat follows.
    replace(mainc,
            "        const char* save_name =\n"
            "            lsw2_fork_build ? \"bountyhunter.sav\" : \"baseline.sav\";",
            "        static std::string lb_save = rom.stem().string() + \".sav\";\n"
            "        const char* save_name = lb_save.c_str();"
            "  /* legobattles-decomp */")

    #     game.toml asks for one stacked window; the sibling port wanted two.
    #     Forcing it here overrode the config the player is actually running.
    replace(mainc,
            "        synthesized.push_back(\"--screen-layout\");\n"
            "        synthesized.push_back(\"separate\");\n",
            "")
    done.append("main.cpp: no-argument launch resolves beside the executable")

    # 0h2. THE PORT KEEPS ITS OWN LOG, because the launcher cannot be relied on.
    #
    #      Every diagnostic in this port writes to stderr -- the ITCM dispatch
    #      probes, the HUD decline counters, the placement footprint, the camera
    #      clamp. Launching `LegoBattles.exe` from build/ directly rather
    #      than through play.bat, a GUI process started that way has nowhere
    #      for stderr to go, so a crashed session produced NO log at all and the
    #      only evidence was one stray dispatch_misses.log. Redirecting in
    #      play.bat fixed only one launch path.
    #
    #      ONLY WHEN STDERR HAS NOWHERE TO GO. If it is already a disk file or a
    #      pipe, something upstream is capturing it deliberately -- the harness
    #      runs with stderr on a file and would lose every measurement if this
    #      stole it. GetFileType tells the two apart: FILE_TYPE_DISK and
    #      FILE_TYPE_PIPE are left alone, a console or no handle at all is not.
    replace(mainc,
            "int main(int argc, char** argv) {",
            "/* legobattles-decomp: implemented in lb_overlay.cpp, which\n"
            "   already includes windows.h -- main.cpp deliberately does\n"
            "   not, and putting the body here cost a build. */\n"
            "extern \"C\" void lb_open_session_log(void);\n"
            "/* legobattles-decomp: arms the ITCM byte-writer probe in\n"
            "   runtime_note_code_write. See apply_hooks 0zz and docs/326. */\n"
            "extern \"C\" unsigned g_lb_itcmw;\n"
            "\n"
            "int main(int argc, char** argv) {\n"
            "    lb_open_session_log();  /* legobattles-decomp */\n"
            "    if (const char* lb_iw = std::getenv(\"LB_ITCMW\"))\n"
            "        g_lb_itcmw = (lb_iw[0] && lb_iw[0] != '0') ? 1u : 0u;")
    done.append("main.cpp: the port writes its own session log when stderr has nowhere to go")

    # 0i. Touch has to follow the picture that is actually on screen.
    #
    #     With the wide field up the window shows a region of the MAP, not the
    #     stock 256x192 screen, so the shipped window-to-guest mapping sent every
    #     click somewhere unrelated -- and the band test below rejected anything
    #     outside the centre 256 columns outright. Both are right for the stock
    #     view and wrong for this one.
    #
    #     Tried first, and only claims the event when the wide field is up, so
    #     the stock path is untouched otherwise.
    replace(front,
            "                                      const float band = "
            "(float)(gw - kScreenWidth) / 2.0f;",
            "                                      {  /* legobattles-decomp */\n"
            "                                          int wtx = 0, wty = 0;\n"
            "                                          if (gw > 0 &&\n"
            "                                              lb_overlay_wide_touch(\n"
            "                                                  gx / (float)gw,\n"
            "                                                  gy / (float)kScreenHeight,\n"
            "                                                  &wtx, &wty)) {\n"
            "                                              nds_set_touch((uint16_t)wtx,\n"
            "                                                            (uint16_t)wty, true);\n"
            "                                              return;\n"
            "                                          }\n"
            "                                      }\n"
            "                                      const float band = "
            "(float)lb_band_left((int)gw);")
    done.append("frontend.cpp: touch follows the wide field")
    # docs/348: ...and the band it takes off is the one the picture ON SCREEN
    # was composited with. lb_band_left/lb_band_top are the values latched at
    # the last VBlank from the camera read there; the frame the player is
    # clicking on was composited with the band of the 3D raster paired with its
    # ground (g_lb_used_extra/_y, gpu2d.cpp). Mid-map both are the centred
    # split; near a map edge, while the camera moves, they differ by a pan step,
    # and every click -- and every corner of a drag box -- landed that far from
    # where it was aimed.
    replace(front,
            "                                      const float band = "
            "(float)lb_band_left((int)gw);\n"
            "                                      const float bandy = "
            "(float)lb_band_top((int)gh);",
            "                                      /* docs/348: the band of the frame on\n"
            "                                         screen, clamped into this surface. */\n"
            "                                      const float band = (float)(\n"
            "                                          gw > kScreenWidth\n"
            "                                              ? std::clamp<int>(lb_shown_band_x(), 0,\n"
            "                                                    (int)gw - kScreenWidth)\n"
            "                                              : 0);\n"
            "                                      const float bandy = (float)(\n"
            "                                          gh > kScreenHeight\n"
            "                                              ? std::clamp<int>(lb_shown_band_y(), 0,\n"
            "                                                    (int)gh - kScreenHeight)\n"
            "                                              : 0);")
    done.append("frontend.cpp: touch takes off the band of the frame on screen")

    # 0i2. The window says what game it is.
    #
    #      "ndsrecomp firmware preview" is the recompiler's own placeholder,
    #      correct for a tool that boots any cartridge and wrong for a build
    #      that boots exactly one. Both sites: the title the window is created
    #      with, and the one rewritten every second with the FPS.
    replace(front,
            "        presentation.separate ? \"ndsrecomp - Top Screen\"\n"
            "                              : \"ndsrecomp firmware preview\",",
            "        presentation.separate ? \"LEGO Battles - Top Screen\"\n"
            "                              : \"LEGO Battles\",")
    replace(front,
            "                ? \"ndsrecomp - Top Screen - \" + fps_text\n"
            "                : \"ndsrecomp firmware preview - \" + fps_text;",
            "                ? \"LEGO Battles - Top Screen - \" + fps_text\n"
            "                : \"LEGO Battles - \" + fps_text;")
    done.append("frontend.cpp: window title is the game's name")

    # 0j. An `oam` debug command.
    #
    #     read_mem returns zeros for 0x07000000: OAM belongs to the video module,
    #     not the CPU bus, exactly as VRAM and palette RAM do (the trap recorded
    #     at the top of lb_field.cpp). Reading it through the bus produced 1024
    #     zero bytes, which decode as 128 perfectly valid enabled sprites at the
    #     origin -- a wrong answer that looks like data.
    dbg = os.path.join(runner_dir, "src", "debug_server.cpp")
    edit(dbg, "#include \"gpu2d.h\"", ["#include \"vram.h\"  /* legobattles-decomp */"])
    replace(dbg,
            "    if (cmd == \"keys\") {",
            "    if (cmd == \"oam\") {  /* legobattles-decomp */\n"
            "        const std::string eng = json_str(line, \"engine\", \"A\");\n"
            "        const int engine = (eng == \"B\" || eng == \"b\") ? 1 : 0;\n"
            "        const uint8_t* oam = nds_vram_renderer_oam(engine);\n"
            "        if (!oam) return \"{\\\"error\\\":\\\"oam unavailable\\\"}\";\n"
            "        std::string hex;\n"
            "        hex.reserve(2048);\n"
            "        static const char* kHex = \"0123456789abcdef\";\n"
            "        for (int i = 0; i < 1024; ++i) {\n"
            "            hex += kHex[(oam[i] >> 4) & 0xF];\n"
            "            hex += kHex[oam[i] & 0xF];\n"
            "        }\n"
            "        return \"{\\\"hex\\\":\\\"\" + hex + \"\\\"}\";\n"
            "    }\n"
            "    if (cmd == \"keys\") {")
    done.append("debug_server.cpp: oam command")

    # 0s. A `vram` debug command: read raw bytes out of one VRAM bank.
    #
    #     Same trap as `oam` above -- read_mem cannot see 0x06000000, because
    #     VRAM belongs to the video module rather than the CPU bus. That cost
    #     more than an inconvenience here: the shroud was recorded as ABSENT
    #     from these matches (docs/202) on the strength of two all-zero main-RAM
    #     probes, while a screenshot of the very same frame is wall-to-wall fog.
    #     Reading the picture's actual source settles what a symbol-based guess
    #     could not.
    replace(dbg,
            "    if (cmd == \"keys\") {",
            "    if (cmd == \"vram\") {  /* legobattles-decomp */\n"
            "        const int bank = (int)json_u64(line, \"bank\", 4);\n"
            "        const unsigned off = (unsigned)json_u64(line, \"off\", 0);\n"
            "        int len = (int)json_u64(line, \"len\", 256);\n"
            "        if (len < 1) len = 1;\n"
            "        if (len > 8192) len = 8192;\n"
            "        uint8_t* v = nds_vram_bank_data((unsigned)bank);\n"
            "        if (!v) return \"{\\\"error\\\":\\\"no such bank\\\"}\";\n"
            "        std::string hex;\n"
            "        hex.reserve((size_t)len * 2);\n"
            "        static const char* kHexV = \"0123456789abcdef\";\n"
            "        for (int i = 0; i < len; ++i) {\n"
            "            const uint8_t b = v[off + (unsigned)i];\n"
            "            hex += kHexV[(b >> 4) & 0xF];\n"
            "            hex += kHexV[b & 0xF];\n"
            "        }\n"
            "        return \"{\\\"hex\\\":\\\"\" + hex + \"\\\"}\";\n"
            "    }\n"
            "    if (cmd == \"keys\") {")
    done.append("debug_server.cpp: vram command")

    # 0u. NDS_ITER_CAP: make the scheduler's rendezvous granularity tunable.
    #
    #     DIAGNOSTIC, and it defaults to the stock 64 so an unset environment
    #     behaves exactly as before.
    #
    #     The scheduler rendezvouses both CPUs every kIterCap system cycles, so
    #     a frame costs about 8,800 rounds, and the per-round work is not small:
    #     NDS_PROFILE_SCHED attributes 23.9% of emulation to ticking devices and
    #     2.6% to computing the next event, both of which are paid once per
    #     round regardless of how much guest code ran in it (docs/204). Doubling
    #     the cap halves that share.
    #
    #     It is NOT free accuracy-wise, which is why this is a knob and not a
    #     new default: the surrounding code defines when a timer overflow's IRQ
    #     becomes visible in terms of "the first kIterCap step at or after the
    #     overflow", so widening the grid coarsens exactly that. Scheduled
    #     events still snap (`if (ev < planned + 8) planned = ev`), so display,
    #     SPU, card and DMA deadlines are unaffected.
    sched = os.path.join(runner_dir, "src", "scheduler.cpp")
    replace(sched,
            "constexpr uint64_t kIterCap        = 64;   "
            "// system cycles per outer iteration",
            "/* legobattles-decomp: NDS_ITER_CAP overrides the rendezvous\n"
            "   granularity for A/B measurement; unset keeps the stock 64. */\n"
            "uint64_t lb_iter_cap() {\n"
            "    static const uint64_t v = [] {\n"
            "        const char* s = std::getenv(\"NDS_ITER_CAP\");\n"
            "        /* legobattles-decomp: 256, not melonDS's 64.\n"
            "\n"
            "           The rendezvous is the fixed cost of a scheduler round --\n"
            "           next-event selection, the CPU slice entry and exit, and\n"
            "           five device ticks -- and at 64 system cycles a frame\n"
            "           pays it ~8750 times. Measured on a display at\n"
            "           1920x1080, four runs per arm:\n"
            "\n"
            "               64    34.4  32.6  33.6  34.1     mean 33.7\n"
            "               256   37.0  36.5  35.9  36.0     mean 36.4\n"
            "\n"
            "           Non-overlapping ranges, +8%. An earlier one-run-per-arm\n"
            "           A/B of this was RETRACTED as noise and was right to be:\n"
            "           the harness spread 29-38 fps at the time. It resolves\n"
            "           now only because the spread is ~2 fps.\n"
            "\n"
            "           512 was tried and is NOT taken. It gains nothing more\n"
            "           (36.9 37.8 35.8, the same as 256) and one of its four\n"
            "           runs collapsed to 9.4 fps with emu_ticks at 46 ms and no\n"
            "           halt logged. Unexplained, and a config that occasionally\n"
            "           drops to 9 fps fails 'stable' whatever the cause.\n"
            "\n"
            "           Fidelity checked at 256: no halt, desync or assert in\n"
            "           any run, and map coverage identical to the 64 arm\n"
            "           (23418/24576). The comment above warns that a wider\n"
            "           round once desynced the IPCSYNC handshake, so this is\n"
            "           the thing to suspect first if wireless play regresses --\n"
            "           NDS_ITER_CAP=64 restores the old behaviour exactly. */\n"
            "        if (!s) return uint64_t{256};\n"
            "        const long long n = std::atoll(s);\n"
            "        if (n < 8 || n > 4096) {\n"
            "            std::fprintf(stderr,\n"
            "                \"[lb] NDS_ITER_CAP %lld out of range 8..4096; \"\n"
            "                \"using 64\\n\", n);\n"
            "            return uint64_t{64};\n"
            "        }\n"
            "        std::fprintf(stderr, \"[lb] NDS_ITER_CAP=%lld\\n\", n);\n"
            "        return uint64_t(n);\n"
            "    }();\n"
            "    return v;\n"
            "}\n"
            "#define kIterCap (lb_iter_cap())")
    done.append("scheduler.cpp: NDS_ITER_CAP rendezvous knob")


    # 0k. LB_BGMASK: clear DISPCNT bits at RENDER time.
    #
    #     Identifying which layer draws something means turning layers off, and
    #     writing DISPCNT through the debug server does not work: the game
    #     rewrites it every frame, so the write never survives to the next
    #     capture. That produced five identical brightness readings which were
    #     one broken experiment, not five negative results (docs/190).
    #
    #     render_engine_line is where the scanline is actually composed, so a
    #     mask applied here cannot be outrun. Diagnostic only, off unless the
    #     variable is set.
    gpu = os.path.join(runner_dir, "src", "gpu2d.cpp")
    replace(gpu,
            "void render_engine_line(int engine, int y) {\n"
            "    Unit& u=g_unit[engine];",
            "void render_engine_line(int engine, int y) {\n"
            "    Unit& u=g_unit[engine];\n"
            "    {   /* legobattles-decomp: LB_BGMASK=<hex> clears DISPCNT bits */\n"
            "        static int probe = -1;\n"
            "        static unsigned mask = 0u;\n"
            "        if (probe < 0) {\n"
            "            const char* v = std::getenv(\"LB_BGMASK\");\n"
            "            probe = v ? 1 : 0;\n"
            "            if (v) mask = (unsigned)std::strtoul(v, nullptr, 0);\n"
            "        }\n"
            "        /* Only in the field. Masking layers everywhere changed\n"
            "           the menus enough that the route's colour-census screen\n"
            "           detection failed and the run never reached gameplay --\n"
            "           the probe destroyed the path to the thing it probes.\n"
            "           The battle is display mode 1 with BG0 as the 3D layer\n"
            "           (measured DISPCNT 00010F08, docs/193). */\n"
            "        const bool lb_field = ((u.dispcnt >> 16) & 3u) == 1u &&\n"
            "                              (u.dispcnt & 0x8u) &&\n"
            "                              (u.dispcnt & 0x100u);\n"
            "        if (probe && engine == 0 && lb_field) u.dispcnt &= ~mask;\n"
            "    }")
    done.append("gpu2d.cpp: LB_BGMASK render-time layer probe")

    # 0q REMOVED: relocating BG1's screen base at render time.
    #
    #     It worked, in that the widened columns finally drew real terrain.
    #     But mutating u.bgcnt[1] mutates the register the GUEST also reads:
    #     func_0210BC2C recomputes the tilemap DMA destination from BG1CNT
    #     on every call, so the guest's writes split across both bases --
    #     measured, 0800=18928 alongside 2800=35712 -- and the native view
    #     came out half correct. The widened entries live in a host array
    #     now (src/lb_widemap.cpp), which cannot race the guest at all.


    # 0q2. Serve the widened columns from the host table.
    #
    #      decode_text_line fetches each tile entry from VRAM. For the columns
    #      the guest maintains -- hardware tile columns 0..31 -- that stays
    #      exactly as it was, so the game's own picture is untouched. Outside
    #      them there is nothing truthful in VRAM to fetch: the layer's second
    #      screen block is another background's tilemap (docs/199). Those
    #      columns come from lb_widemap's host array instead.
    #
    #      The row index is the decoder's own: the guest writes its 26-row
    #      window starting at block row 0, so `sy >> 3` is the same row the
    #      table is filled by.
    # The declaration goes at NAMESPACE scope; `extern "C"` is not allowed
    # inside a function body, which cost one build.
    replace(gpu,
            "void decode_text_line(int engine, int bg, int y, const uint8_t* palette,",
            "extern \"C\" int lb_widemap_tile(int htile, int trow, int gy,\n"
            "                               uint16_t* out);\n"
            "extern \"C\" int lb_widemap_shroud(int htile, int trow, int gy,\n"
            "                                 uint16_t* out);\n"
            "extern \"C\" int lb_widemap_fog(int htile, int trow, int gy,\n"
            "                              uint16_t* out);\n"
            "extern \"C\" int lb_widemap_clear_tile(int bg, uint16_t* out);\n"
            "extern \"C\" int lb_widemap_offmap(int htile, int trow);\n"
            "/* legobattles-decomp docs/325 phase 2: the field's base pixel,\n"
            "   drawn from the level grid instead of a 256-px hardware layer. */\n"
            "extern \"C\" int lb_hostterrain_prepare(void);\n"
            "extern \"C\" int lb_hostterrain_pixel(int dx, int dy,\n"
            "                                     uint32_t* packed6);\n"
            "extern \"C\" void lb_hostterrain_note(int row, int in_band,\n"
            "                                      uint32_t out_rgb32,\n"
            "                                      uint32_t ht_rgb32);\n"
            "extern \"C\" void lb_hostterrain_dump_once(const uint32_t* px,\n"
            "                                          int w, int h);\n"
            "extern \"C\" int lb_hostterrain_row(int dy, const uint32_t** row,\n"
            "                                   int* dx_lo, int* dx_hi);\n"
            "extern \"C\" int lb_fp_band_tile(int htile, int trow,\n"
            "                                uint16_t* out);\n"
            "extern \"C\" int lb_fp_bands_active(void);\n"
            "extern \"C\" int lb_band_left(int output_width);\n"
            "extern \"C\" int lb_band_right(int output_width);\n"
            "extern \"C\" int lb_band_top(int output_height);\n"
            "extern \"C\" int lb_band_render(void);\n"
            "extern \"C\" int lb_band_render_y(void);\n"
            "extern \"C\" int lb_lag_dx(void);   /* docs/335 */\n"
            "\n"
            "void decode_text_line(int engine, int bg, int y, const uint8_t* palette,")
    #      THE DECODED LINE IS NO LONGER SYMMETRIC.
    #
    #      Upstream builds it as `256 + left_extra * 2`, which says the same
    #      thing twice: how many columns sit left of the guest's own 256, and
    #      how wide the line is. That identity holds only while the band is
    #      split evenly, and lb_band_left now slides it at a map edge so the
    #      whole overhang can sit on one side. Left as it was, a line clamped
    #      hard left would decode 256 columns and be sampled out to the full
    #      surface width -- reading past what was written.
    #
    #      So the two quantities are passed separately. `right_extra < 0` keeps
    #      the old meaning ("same as left"), which is what every native and
    #      engine-B caller wants and is the default, so this changes nothing
    #      for any caller that does not ask.
    replace(gpu,
            "                      const NdsVramRendererView& vram, BgLine& out,\n"
            "                      int left_extra = 0) {",
            "                      const NdsVramRendererView& vram, BgLine& out,\n"
            "                      int left_extra = 0, int right_extra = -1) {")
    replace(gpu,
            "    out.left_extra = left_extra;\n"
            "    out.decoded_width = 256 + left_extra * 2;",
            "    out.left_extra = left_extra;\n"
            "    out.decoded_width =\n"
            "        256 + left_extra + (right_extra < 0 ? left_extra : right_extra);")
    replace(gpu,
            "                    int left_extra = 0) {\n"
            "    out.left_extra = 0;",
            "                    int left_extra = 0, int right_extra = -1) {\n"
            "    out.left_extra = 0;")
    replace(gpu,
            "            decode_text_line(engine, bg, y, palette, vram, out, left_extra);",
            "            decode_text_line(engine, bg, y, palette, vram, out,\n"
            "                             left_extra, right_extra);")
    done.append("gpu2d.cpp: decoded lines may be asymmetric about the guest window")

    #      THE BAND TRAVELS WITH THE 3D LINES IT BELONGS TO.
    #
    #      The band is a translation of the whole picture, so the 2D layers and
    #      the 3D geometry must apply the SAME one or the units slide off the
    #      terrain. They are computed at different moments -- the geometry is
    #      transformed as the guest submits it, the 2D layers when the frontend
    #      composites -- and near a map edge the clamp changes the band every
    #      frame, so "both read the current value" is not good enough.
    #
    #      There is already a mechanism that pairs a rendered 3D frame with the
    #      composite that consumes it: the wide-line snapshot, double buffered
    #      and flipped by nds_gpu2d_finish_frame. The band rides along in it.
    #      Whatever the ordering between the guest's vblank IRQ, GPU3D's own
    #      VBlank and the present, the composite uses the band the geometry it
    #      is compositing was actually transformed with.
    replace(gpu,
            "std::array<uint16_t, 2> g_wide_3d_width{};",
            "std::array<uint16_t, 2> g_wide_3d_width{};\n"
            "/* legobattles-decomp: the band that the 3D lines in the matching\n"
            "   slot of g_wide_3d_frame were transformed with, both axes. */\n"
            "std::array<int, 2> g_wide_3d_band{};\n"
            "std::array<int, 2> g_wide_3d_band_y{};\n"
            "/* docs/348: and the camera that raster's list was built with,\n"
            "   and the height it was copied at, so the composite can pair\n"
            "   it with the ground committed from the same camera. */\n"
            "std::array<int, 2> g_wide_3d_camx{};\n"
            "std::array<int, 2> g_wide_3d_camy{};\n"
            "std::array<int, 2> g_wide_3d_camok{};\n"
            "std::array<int, 2> g_wide_3d_height{};\n"
            "extern \"C\" int lb_band_render(void);\n"
            "extern \"C\" int lb_band_render_y(void);\n"
            "extern \"C\" int lb_tag_render_get(int* x, int* y);")
    replace(gpu,
            "                g_wide_3d_width[g_front ^ 1] = wide_width;",
            "                g_wide_3d_width[g_front ^ 1] = wide_width;\n"
            "                g_wide_3d_band[g_front ^ 1] = lb_band_render();\n"
            "                g_wide_3d_band_y[g_front ^ 1] = lb_band_render_y();\n"
            "                g_wide_3d_camok[g_front ^ 1] = lb_tag_render_get(\n"
            "                    &g_wide_3d_camx[g_front ^ 1],\n"
            "                    &g_wide_3d_camy[g_front ^ 1]);\n"
            "                g_wide_3d_height[g_front ^ 1] =\n"
            "                    (int)nds_gpu3d_output_height();\n"
            "                /* legobattles-decomp: THE ROWS THE DISPLAY NEVER\n"
            "                   SCANS. See apply_hooks.py. */\n"
            "                if (y == 191) {\n"
            "                    const int lb_oh =\n"
            "                        (int)nds_gpu3d_output_height();\n"
            "                    const int lb_cap =\n"
            "                        lb_oh < kMaxAdaptiveHeight\n"
            "                            ? lb_oh : kMaxAdaptiveHeight;\n"
            "                    for (int lb_y = 192; lb_y < lb_cap; ++lb_y) {\n"
            "                        const size_t lb_off =\n"
            "                            static_cast<size_t>(lb_y) * wide_width;\n"
            "                        std::copy_n(nds_gpu3d_wide_line(lb_y),\n"
            "                                    wide_width,\n"
            "                                    g_wide_3d_frame[g_front ^ 1]\n"
            "                                        .data() + lb_off);\n"
            "                        if (g_adaptive_skybox_fill) {\n"
            "                            const uint32_t* const lb_attr =\n"
            "                                nds_gpu3d_wide_attr_line(lb_y);\n"
            "                            if (lb_attr)\n"
            "                                std::copy_n(\n"
            "                                    lb_attr, wide_width,\n"
            "                                    g_wide_3d_attr_frame[g_front ^ 1]\n"
            "                                        .data() + lb_off);\n"
            "                        }\n"
            "                    }\n"
            "                }")
    replace(gpu,
            "    g_wide_3d_width.fill(0u);",
            "    g_wide_3d_width.fill(0u);\n"
            "    g_wide_3d_band.fill(0);\n"
            "    g_wide_3d_band_y.fill(0);\n"
            "    g_wide_3d_camok.fill(0);\n"
            "    g_wide_3d_height.fill(0);")
    done.append("gpu2d.cpp: the band is snapshotted with the 3D lines")
    done.append("gpu2d.cpp: the 3D snapshot covers the rows below the DS's 192")

    #      THE ROWS THE DISPLAY NEVER SCANS -- and this was why the lower
    #      pixels of the earth builder were culled.
    #
    #      The snapshot above runs inside render_engine_line, which the port
    #      calls once per DS SCANLINE: y = 0..191, because that is how many
    #      rows the hardware displays. It writes g_wide_3d_frame row y for each
    #      of those. The compositor then reads that buffer for every row of the
    #      surface -- 426 of them at 1600x900, 696 at 3440x1440 -- so every row
    #      from 192 down held whatever was left in the buffer, which is zero.
    #
    #      GPU3D itself renders all RenderHeight rows; nothing was missing from
    #      the renderer. Only the copy was short.
    #
    #      MEASURED before the fix, layer dump at 786x426, camera parked:
    #
    #          3D pixels exist on surface rows 131..191, and nowhere below
    #          a 23x24 unit at rows 173..197  -> 19 rows drawn, 5 lost
    #          a 24x24 unit at rows 205..229  -> 0 rows drawn, gone entirely
    #
    #      which is exactly the two symptoms reported, from one cause: a sprite
    #      STRADDLING row 191 loses its bottom, and one wholly below it
    #      disappears. The guest's own 192 rows sit at surface 117..309 on that
    #      surface, so this was eating the bottom 118 rows of the player's own
    #      picture, not merely the widened band.
    #
    #      WHY IT LOOKED INTERMITTENT, and why smaller windows escaped: the
    #      compositor only reads the snapshot when its width matches the
    #      surface, and falls back to nds_gpu3d_wide_line() live otherwise. The
    #      live path has every row. So the size sweep's transient surfaces
    #      showed 3D down to rows 274, 352 and 203, while the steady-state
    #      786x426, 946x516 and 1706x696 all stopped dead at 191 -- the
    #      signature that made this findable.
    #
    #      Done at y == 191 rather than per row, because by the last scanline
    #      GPU3D has finished the frame and every line below is ready to read;
    #      and capped at kMaxAdaptiveHeight so it can never run off the array.

    #      ...and the field composite reads it back.
    #
    #      ENGINE B KEEPS THE EVEN SPLIT. The clamp is about a battlefield's
    #      edge; engine B in a battle is the other screen and in a menu it is a
    #      title card, neither of which has a map edge to be confined to.
    #      Sliding them would move a picture that has nothing to gain by it, so
    #      the branch that widens them is left reading exactly what it read
    #      before -- which is also why this is written as one conditional rather
    #      than by shadowing `extra` further down: the engine-B widener
    #      (lb_wide2d.inc) and lb_menu_fill both consume it above that point.
    #
    #      The snapshot test is spelled out again here rather than reusing
    #      `snapshot_matches`, which upstream declares about ninety lines below
    #      this one. Same expression, same two operands.
    replace(gpu,
            "    const int extra = (output_width - 256) / 2;",
            "    const int extra = std::clamp(\n"
            "        (engine != 0)\n"
            "            ? (output_width - 256) / 2\n"
            "            : lb_pair_hold ? lb_pair_band()\n"
            "            : lb_use_snap ? g_wide_3d_band[g_front]\n"
            "            : lb_band_render(),\n"
            "        0, output_width > 256 ? output_width - 256 : 0);\n"
            "    /* The other side of the same overhang. Not `extra` again. */\n"
            "    const int extra_r = output_width - 256 - extra;\n"
            "    /* legobattles-decomp: what the DEBUG ASPECT BOX must use. The\n"
            "       box marks where the guest's own 256x192 sits in the widened\n"
            "       surface, so it has to be drawn with the band this composite\n"
            "       actually used -- not a fresh lb_band_render(), which can\n"
            "       have moved on by the time the frame is presented. */\n"
            "    g_lb_used_extra = extra;\n"
            "    /* legobattles-decomp: and the same value for the sprites, so\n"
            "       they are placed against the ground they stand on. Item #5. */\n"
            "    g_lb_obj_band = extra;\n"
            "    g_lb_obj_band_valid = (engine == 0);\n"
            "    /* legobattles-decomp: DRIFT PROBE, LB_DRIFTLOG=1.\n"
            "\n"
            "       Items #5 and the fog drift are both 'one layer is placed\n"
            "       against a different moment than the layer under it'. Three\n"
            "       'when' values meet in this one frame and nothing has ever\n"
            "       printed them TOGETHER:\n"
            "\n"
            "         extra      what THIS composite shifts the 2D layers by --\n"
            "                    the band that came back with the 3D snapshot\n"
            "         band_left  what render_obj_line will use for the SPRITES,\n"
            "                    two hand-offs earlier in the latch chain\n"
            "         fill_cam   the camera fill_terrain built the extension\n"
            "                    tables from, which is what indexes the fog\n"
            "         cam_tx     the live camera right now\n"
            "\n"
            "       Printed from inside the consumer, on one line, so no two\n"
            "       numbers can come from different moments -- three earlier\n"
            "       attempts at #5 died measuring quantities upstream of the\n"
            "       code that actually places the pixels. A difference here is\n"
            "       the drift; equality here means the cause is elsewhere and\n"
            "       this probe has earned its keep by saying so. */\n"
            "    if (engine == 0 && output_width > 256) {\n"
            "        static const bool lb_driftlog =\n"
            "            std::getenv(\"LB_DRIFTLOG\") != nullptr;\n"
            "        if (lb_driftlog) {\n"
            "            /* NO PRIVATE COUNTER HERE, and no separate line.\n"
            "\n"
            "               This block used to print its own [lb_drift] line\n"
            "               numbered by a static of its own, which was then\n"
            "               joined against [lb_drift3d]'s f= on the assumption\n"
            "               both were the shared frame. They are not: this runs\n"
            "               BEFORE lb_layer_begin() ticks lb_dump_frame, so the\n"
            "               two were a frame apart and reported DIFFERENT SURFACE\n"
            "               WIDTHS for the same f (1712 here against 1572 there).\n"
            "               Two clocks joined as one, for the third time in this\n"
            "               hunt. So the values are parked and printed by\n"
            "               lb_drift_3d_extent() after the rows, on ONE line, with\n"
            "               the frame number the rest of the dumps use. */\n"
            "            const int live = lb_band_left(output_width);\n"
            "            int fill_tx = 0, cam_tx = 0;\n"
            "            lb_widemap_drift_cams(&fill_tx, &cam_tx);\n"
            "            /* THE SECOND CAMERA, read at the SAME INSTANT.\n"
            "\n"
            "               choose_camera's g_cam_tx is validated against the\n"
            "               BG1 tilemap the guest actually drew; lb_field_camera\n"
            "               reads the game's own view object (+0x5E/+0x60, found\n"
            "               via the scroll callback, docs/174). The ground is\n"
            "               drawn from the first and entities are placed from the\n"
            "               second, so a difference between them IS a Euclidean\n"
            "               offset between units and the ground under them.\n"
            "\n"
            "               Interleaved log lines showed them 48 px apart, but\n"
            "               adjacent lines are not the same instant and the game\n"
            "               scrolls by rewriting its tilemap, so that gap could\n"
            "               have been the two probes firing at different points\n"
            "               in the frame. Reading both HERE, in the consumer, in\n"
            "               one breath, is the only version of the question that\n"
            "               can be answered. */\n"
            "            int field_x = 0, field_y = 0;\n"
            "            const bool field_ok =\n"
            "                lb_field_camera(&field_x, &field_y);\n"
            "            lb_drift_park(output_width, extra, live,\n"
            "                          g_wide_3d_band[g_front],\n"
            "                          lb_band_render(), fill_tx, cam_tx,\n"
            "                          (unsigned)g_unit[0].dispcnt,\n"
            "                          field_ok ? field_x : -999999,\n"
            "                          field_ok ? field_y : -999999);\n"
            "        }\n"
            "    }")
    done.append("gpu2d.cpp: the field composite uses the band its 3D frame used")

    #      The OBJ layer's own copy of the split.
    #
    #      This is the one band consumer that is NOT given the snapshot-paired
    #      value, and the reason is that it cannot observe the difference: OBJ
    #      is OFF for the whole battle. DISPCNT is 00010F08 in the field
    #      (measured, docs/193) and bit 12 is clear, so render_obj_line returns
    #      at its second line without placing anything. The composite already
    #      relies on that -- it skips the per-row Pixel clear entirely when the
    #      bit is down.
    #
    #      Where OBJ IS on -- menus, title cards -- there is no map, so
    #      lb_band_left returns the same even split this expression computed
    #      before. Identical output, one accessor. If OBJ ever comes up in the
    #      field, this is the seam: it would then be a frame behind the layers.
    replace(gpu,
            "                const int extra = (out_width - 256) / 2;",
            "                const int extra = lb_band_left(out_width);")
    done.append("gpu2d.cpp: OBJ placement goes through the band accessor")

    #      "WAS THIS LINE WIDENED?" IS NOW A QUESTION ABOUT ITS WIDTH.
    #
    #      Upstream asks `left_extra > 0`, which was the same question while
    #      the band was split evenly: a widened line always had columns to the
    #      left of the guest's own. Clamped hard left at the map's west edge it
    #      no longer does -- left_extra is 0 and the whole overhang is on the
    #      right -- and the old test would send a fully widened line back to the
    #      native `hud_x` mapping, drawing the guest's 256 columns into the
    #      middle of the surface and nothing either side of them.
    #
    #      decoded_width is the direct question and is set on the same two
    #      paths, including the mosaic bail-out that resets both fields.
    replace(gpu,
            "                const int src = line.left_extra > 0 ? x : hud_x;",
            "                const int src = line.decoded_width > 256 ? x : hud_x;")
    done.append("gpu2d.cpp: a widened line is recognised by its width, not its left edge")

    replace(gpu,
            "        const uint16_t tile = static_cast<uint16_t>(\n"
            "            bg_view16(vram, engine, map_addr));",
            "        uint16_t tile = static_cast<uint16_t>(\n"
            "            bg_view16(vram, engine, map_addr));\n"
            "        bool lb_offmap = false;  /* past the world's own edge */\n"
            "        {   /* legobattles-decomp: widened columns */\n"
            "            /* Key the substitution on the tilemap column the\n"
            "               HARDWARE samples, (bgx + hw_x) >> 3, and not on the\n"
            "               screen column hw_x >> 3. The two differ by the\n"
            "               scroll bias: func_0202d1b4 writes BGxOFS as\n"
            "               (-(4 - camx % 8)) & 0x1FF, so it is small but not\n"
            "               zero. Keying on the screen column put the supplied\n"
            "               tiles up to 7 px out of step with the guest's own,\n"
            "               and at the left boundary left the first few pixels\n"
            "               sampling the tilemap column just OUTSIDE the window\n"
            "               the game maintains -- stale data the game never\n"
            "               wrote. That is the 4-px stripe at surface x 96..99:\n"
            "               a vanilla artefact that sat harmlessly in the screen\n"
            "               edge until widening moved it into the middle of the\n"
            "               picture. Measured, not assumed: with the map\n"
            "               revealed the port read the guest's BG2 window as\n"
            "               768/768 clear tiles, yet its decode was opaque at\n"
            "               native x 0..3 -- so those pixels cannot have come\n"
            "               from inside that window. */\n"
            "            const int lb_span = (int)sx_mask + 1;\n"
            "            int lb_bias = (int)(u.bgx[bg] & sx_mask);\n"
            "            if (lb_bias >= lb_span / 2) lb_bias -= lb_span;\n"
            "            const int lb_hwx = (x - left_extra) + lb_bias;\n"
            "            const int lb_ht = (lb_hwx >= 0) ? (lb_hwx >> 3)\n"
            "                              : -((-lb_hwx + 7) >> 3);\n"
            "            uint16_t lb_e = 0;\n"
            "            bool lb_done = false;\n"
            "            /* The tile ROW, signed, by exactly the same argument\n"
            "               as the column above. This used to be\n"
            "               (int)(line.sy >> 3), which is the WRAPPED hardware\n"
            "               row 0..31: fine while every row asked for was inside\n"
            "               the guest's own window, and wrong the moment the\n"
            "               vertical field of view asks for a row above it. Guest\n"
            "               row -1 arrives as line.sy 0x1F8 and keys to row 31 --\n"
            "               a row the game never maintains -- which is the band\n"
            "               of stale sprite tiles across the top of the screen.\n"
            "               Unwrapping it the way the column is unwrapped lets\n"
            "               lb_widemap answer for the row instead. */\n"
            "            const uint32_t lb_sy_mask = (size & 2u) ? 0x1FFu\n"
            "                                                    : 0xFFu;\n"
            "            const int lb_yspan = (int)lb_sy_mask + 1;\n"
            "            int lb_ybias = (int)(u.bgy[bg] & lb_sy_mask);\n"
            "            if (lb_ybias >= lb_yspan / 2) lb_ybias -= lb_yspan;\n"
            "            const int lb_hwy = y + lb_ybias;\n"
            "            const int lb_tr = (lb_hwy >= 0) ? (lb_hwy >> 3)\n"
            "                              : -((-lb_hwy + 7) >> 3);\n"
            "            const uint16_t lb_hw_tile = tile;   /* before any"
            " substitution */\n"
            "            if (engine == 0 && bg == 1 &&\n"
            "                lb_widemap_tile(lb_ht, lb_tr, y, &lb_e))\n"
            "                { tile = lb_e; lb_done = true; }\n"
            "            /* legobattles-decomp: LB_TILECHK=<guest row>.\n"
            "\n"
            "               tools/lb_bandaudit.py says the RIGHT band matches\n"
            "               the map on 12-23% of its detailed pixels and the\n"
            "               BOTTOM band on 36-55%, while the guest window is\n"
            "               100.0% and the LEFT band 99.9%. Reading the code\n"
            "               says that cannot happen: decoded_width is\n"
            "               256 + left_extra*2 so both bands are decoded,\n"
            "               lb_ht is the true absolute column, and\n"
            "               lb_widemap_tile has no decline path that applies\n"
            "               out there (guest_cell claims only 0..31,\n"
            "               declined_out_of_table is 0). One of those is wrong,\n"
            "               so record what the decoder ACTUALLY did per tile\n"
            "               instead of arguing about it: the screen column, the\n"
            "               absolute tile column it keyed on, whether the\n"
            "               substitution fired, the tile it used, and the tile\n"
            "               the hardware would have given. */\n"
            "            {\n"
            "                static int lb_chk = -2;\n"
            "                if (lb_chk == -2) {\n"
            "                    const char* lb_v = std::getenv(\"LB_TILECHK\");\n"
            "                    lb_chk = lb_v ? std::atoi(lb_v) : -1;\n"
            "                }\n"
            "                /* A WIDENED LINE OR THIS PROVES NOTHING. The first\n"
            "                   attempt traced only unwidened frames -- x stepped\n"
            "                   0,4,12,20 and ht ran -1,0,1,2, i.e. 33 tiles for a\n"
            "                   256-wide decode -- where sub=0 across columns\n"
            "                   0..31 is CORRECT, because guest_cell rightly\n"
            "                   declines the guest's own cells. The question is\n"
            "                   only meaningful where the bands actually exist. */\n"
            "                if (lb_chk >= 0 && engine == 0 && bg == 1 &&\n"
            "                    y == lb_chk && out.decoded_width > 256) {\n"
            "                    static unsigned lb_pass = 0;\n"
            "                    if (x == 0) ++lb_pass;\n"
            "                    if ((lb_pass % 240u) == 0u)\n"
            "                        std::fprintf(stderr,\n"
            "                            \"[lb_tilechk] le=%4d x=%4d ht=%4d\"\n"
            "                            \" tr=%4d sub=%d tile=%03X hw=%03X\\n\",\n"
            "                            left_extra, x, lb_ht, lb_tr,\n"
            "                            lb_done ? 1 : 0,\n"
            "                            unsigned(tile) & 0x3FFu,\n"
            "                            unsigned(lb_hw_tile) & 0x3FFu);\n"
            "                }\n"
            "            }\n"
            "            /* BG2 is the fog. Without it the widened columns show\n"
            "               revealed ground where the native ones show shroud,\n"
            "               which is a gameplay change and not a wider window.\n"
            "               BG3 is a transparent overlay-clear over the same\n"
            "               area; supplying it keeps the widened columns from\n"
            "               decoding BG3 through the hardware wrap. */\n"
            "            if (engine == 0 && bg == 2 &&\n"
            "                lb_widemap_fog(lb_ht, lb_tr, y, &lb_e))\n"
            "                { tile = lb_e; lb_done = true; }\n"
            "            /* BG3 ONLY OUTSIDE THE NATIVE VIEWPORT. docs/205 found\n"
            "               this port's model of BG3 paints an opaque band --\n"
            "               that is why lb_widemap_extends declines it -- and\n"
            "               the bias correction above quietly handed it a new\n"
            "               place to do so: with a -4 bias the first four screen\n"
            "               columns key to tile column -1, which BG3 was then\n"
            "               happy to serve. BG2's garbage went away and BG3's\n"
            "               shroud tile took its place at the very same pixels,\n"
            "               at priority 0, so the stripe survived a fix that had\n"
            "               genuinely worked. Gate it on the UNBIASED screen\n"
            "               column so BG3 is substituted only where it is\n"
            "               actually being widened, which today is nowhere. */\n"
            "            /* ...and outside it VERTICALLY too, now that there are\n"
            "               rows outside it. A row above or below the guest's\n"
            "               26-row window crosses straight through the native\n"
            "               columns, so gating on the column alone left those\n"
            "               pixels sampling the stale part of the tilemap. */\n"
            "            const int lb_scr = x - left_extra;\n"
            "            const bool lb_extra_row = y < 0 || y >= 192;\n"
            "            const bool lb_outside =\n"
            "                lb_scr < 0 || lb_scr >= 256 || lb_extra_row;\n"
            "            /* PAST THE WORLD'S EDGE DRAWS NOTHING.\n"
            "               A widened column beyond the map has no terrain at\n"
            "               any visibility. Declining sends it to the hardware\n"
            "               tilemap, which is 32 entries wide and WRAPS, so the\n"
            "               columns west of the map redrew the map's own first\n"
            "               32 columns over and over -- reported as 'one tiny\n"
            "               snippet of the terrrain texture tiling outside the\n"
            "               game's own rendering'. Serving entry 0 is no better:\n"
            "               tile index 0 is grass in this tileset, which is the\n"
            "               'flat green next to fully detailed terrain' already\n"
            "               recorded in lb_widemap.cpp.\n"
            "               Gated on lb_outside, so the guest's own 32 columns\n"
            "               can never be blanked by this.\n"
            "\n"
            "               NOT BG2, WHICH IS THE FOG. This had no bg test, so\n"
            "               past the world's edge it blanked every layer and the\n"
            "               BACKDROP showed through -- no terrain and no cloud.\n"
            "               Measured on a 1266x696 surface: 40% of the whole\n"
            "               picture was backdrop, as a 69-px stripe down the\n"
            "               west side and every row below the map's south edge,\n"
            "               both matching the off-map extent exactly (cam_tx 55,\n"
            "               a 63-tile band, so absolute column -8 = 64 px). The\n"
            "               user sees that as the fog being spotty, and it is a\n"
            "               HOLE over the most untraversed ground there is.\n"
            "               The cartridge draws cloud at a map edge, and\n"
            "               fill_terrain already stores the fog tile for these\n"
            "               cells -- off the map leaves `s` at its shrouded\n"
            "               default -- so BG2 only has to be allowed to draw\n"
            "               it. Terrain still draws nothing out there. */\n"
            "            if (engine == 0 && bg != 2 && lb_outside &&\n"
            "                lb_widemap_offmap(lb_ht, lb_tr))\n"
            "                lb_offmap = true;\n"
            "            /* BG3 DRAWS NOTHING OUTSIDE THE GUEST'S OWN ROWS,\n"
            "               because that is what it does INSIDE them.\n"
            "\n"
            "               Measured: the cartridge writes 001F into every one\n"
            "               of BG3's own 32 columns, on every row, and draws\n"
            "               nothing -- full 16-bit entries, no palette or flip\n"
            "               bits, identical to the bare constant this port\n"
            "               substitutes. And the shroud does not live here at\n"
            "               all: scan_shroud decides `shrouded` by testing BG2\n"
            "               against 0x064, so BG3 is a layer the game keeps\n"
            "               uniformly inert.\n"
            "\n"
            "               For BG3 lb_widemap_extends() returns 0, so\n"
            "               left_extra is always 0, lb_scr is always inside\n"
            "               0..255, and lb_outside reduces EXACTLY to\n"
            "               lb_extra_row. So this substitution never fired in a\n"
            "               guest row -- which is why the guest's own 192 rows\n"
            "               were always right -- and always fired across all\n"
            "               256 native columns of every BAND row. BG3 is\n"
            "               priority 0, frontmost (docs/215: BG0 2 BG1 3 BG2 1\n"
            "               BG3 0), and the substituted tile decodes to 0x9665,\n"
            "               (5,19,5) in BGR555 -- the same green as the terrain\n"
            "               -- composited about 1.45x brighter by BLDCNT.\n"
            "\n"
            "               That was the visible defect: a flat green\n"
            "               rectangle whose HEIGHT is extra_y and whose WIDTH is\n"
            "               the guest's own column range, sitting directly above\n"
            "               and below their picture, over perfectly good\n"
            "               terrain. The side bands escaped only because an\n"
            "               unextended layer is indexed by hud_x, which is -1\n"
            "               out there.\n"
            "\n"
            "               Emitting transparent is not a licence to skip the\n"
            "               substitution and let the fetch stand: rows 24..31 of\n"
            "               the guest's tilemap are stale, so an un-substituted\n"
            "               band row would sample data the game never wrote. */\n"
            "            if (engine == 0 && bg == 3 && lb_extra_row)\n"
            "                lb_offmap = true;\n"
            "            /* THE PLACEMENT FOOTPRINT, item #4. BG3 carries\n"
            "               nothing else, and while a building is being placed\n"
            "               lb_widemap_extends(3) turns on so these columns\n"
            "               exist at all. The cells come from the GUEST'S OWN\n"
            "               painter run under a shifted camera (docs/306\n"
            "               design 3, working and measured in docs/320), so\n"
            "               every per-cell verdict out here is the\n"
            "               cartridge's.\n"
            "\n"
            "               The decline below it is the other half. An\n"
            "               extended BG3 column with no footprint must draw\n"
            "               NOTHING -- not the inert 0x001F, which is opaque\n"
            "               and is exactly the band this layer painted the\n"
            "               last time it was extended. */\n"
            "            else if (engine == 0 && bg == 3 && lb_outside &&\n"
            "                lb_fp_band_tile(lb_ht, lb_tr, &lb_e))\n"
            "                { tile = lb_e; lb_done = true; }\n"
            "            else if (engine == 0 && bg == 3 && lb_outside &&\n"
            "                lb_fp_bands_active())\n"
            "                lb_offmap = true;\n"
            "            else if (engine == 0 && bg == 3 && lb_outside &&\n"
            "                lb_widemap_shroud(lb_ht, lb_tr, y, &lb_e))\n"
            "                { tile = lb_e; lb_done = true; }\n"
            "            /* Nothing truthful to draw is still not a licence to\n"
            "               draw the stale column. Whenever the hardware would\n"
            "               sample OUTSIDE the 32 tilemap columns the game keeps\n"
            "               up to date, an overlay layer gets its own clear\n"
            "               entry instead of whatever was last left in memory.\n"
            "               Without this the 4-px sliver simply changes which\n"
            "               layer draws it: fixing BG2 handed it to BG3, and\n"
            "               with the map revealed g_ext2_valid is false so BG2\n"
            "               declines and takes it straight back. */\n"
            "            if (!lb_done && !lb_offmap && engine == 0 &&\n"
            "                (bg == 2 || bg == 3) &&\n"
            "                (lb_ht < 0 || lb_ht >= 32 || lb_extra_row) &&\n"
            "                lb_widemap_clear_tile(bg, &lb_e))\n"
            "                tile = lb_e;\n"
            "        }")
    done.append("gpu2d.cpp: widened columns come from the host table")

    # docs/325 phase 2: the field's BASE pixel comes from the level grid.
    #
    #     The compositor starts every pixel at the palette backdrop and pushes
    #     BG layers over it. Replacing that base with the host-rendered map
    #     gives the bands real terrain without touching what BG1 draws inside
    #     the guest's own 256 columns -- so the native picture is unchanged and
    #     only the sides differ, which is what makes this step checkable by
    #     screenshot against the current build.
    #
    #     Prepared ONCE PER SCANLINE ROW BLOCK, not per pixel: the prepare call
    #     renders the map and is idempotent within a frame, and the sampler is
    #     a bounds check plus one array read.
    replace(gpu,
            "        for (int x = 0; x < output_width; ++x) {\n"
            "            Pixel top = backdrop;\n"
            "            Pixel below = backdrop;",
            "        /* legobattles-decomp docs/325 phase 2: the host map row\n"
            "           for this output row, fetched ONCE. Was a call per\n"
            "           pixel -- 98,304 a frame through an extern \"C\"\n"
            "           boundary for two bounds checks and an array read. */\n"
            "        const uint32_t* lb_ht_row = nullptr;\n"
            "        int lb_ht_lo = 0, lb_ht_hi = 0;\n"
            "        const bool lb_ht_have =\n"
            "            lb_hostterrain_row(y - extra_y, &lb_ht_row,\n"
            "                               &lb_ht_lo, &lb_ht_hi) != 0;\n"
            "        for (int x = 0; x < output_width; ++x) {\n"
            "            /* legobattles-decomp docs/325 phase 2 */\n"
            "            Pixel lb_base = backdrop;\n"
            "            uint32_t lb_ht = 0;\n"
            "            bool lb_ht_ok = false;\n"
            "            if (lb_ht_have) {\n"
            "                const int lb_dx = x - extra;\n"
            "                if (lb_dx >= lb_ht_lo && lb_dx < lb_ht_hi) {\n"
            "                    const uint32_t lb_c =\n"
            "                        lb_ht_row[lb_dx - lb_ht_lo];\n"
            "                    lb_ht = ((lb_c >> 18) & 0x3Fu)\n"
            "                          | (((lb_c >> 10) & 0x3Fu) << 8)\n"
            "                          | (((lb_c >> 2) & 0x3Fu) << 16);\n"
            "                    lb_base = Pixel{lb_ht, 0x20u, 3, 4, 5, true};\n"
            "                    lb_ht_ok = true;\n"
            "                }\n"
            "            }\n"
            "            Pixel top = lb_base;\n"
            "            Pixel below = lb_base;")
    #     The prepare call is hoisted beside lb_hud_scan, in the worker stanza
    #     further down -- it renders the whole map (1.256 ms, Phase 1) and the
    #     row body is fanned out across threads, so it must run once per frame
    #     BEFORE the fan-out. The sampler itself only reads, so the workers can
    #     call it concurrently.
    done.append("gpu2d.cpp: the field's base pixel comes from the level grid")

    # Emit transparent pixels for a column past the world's own edge, on every
    # layer, so the backdrop shows through and the area is BLACK -- which is
    # exactly the requirement: the only black space is that outside the
    # actual world boundary. Palette index 0 is already this
    # decoder's transparency (`if (!index) { out.color[x + k] = 0u; ... }`), so
    # this writes precisely what an index-0 pixel would have written.
    edit(gpu, "        const bool hflip = (tile & 0x0400u) != 0;",
         ["if (lb_offmap) {",
          "    for (int k = 0; k < run; ++k) out.color[x + k] = 0u;",
          "    x += run;",
          "    continue;",
          "}"],
         after=False, indent="        ")
    done.append("gpu2d.cpp: off-map columns draw nothing, not the wrapped tilemap")

    # 0t. Extend the SHROUD layer as well as the terrain layer.
    #
    #     Upstream widens exactly one layer: `wide_bg_slot`, the furthest back
    #     (largest BGCNT priority). Its reasoning is sound for a title it knows
    #     nothing about -- reaching a foreground layer into the new space prints
    #     the logo three times across the frame -- and it picks BG1 here, prio 3,
    #     which is the terrain and is what the terrain widener wants.
    #
    #     But this port does not need to guess. The field's layers are known:
    #     BG1 is the terrain and BG3, at priority 0, is the shroud. Widening
    #     only BG1 draws revealed ground in the widened columns while the native
    #     columns beside them are under fog -- a gameplay change, and the one
    #     thing the widening was not allowed to do. So the port names the layers
    #     it can supply entries for, and upstream's rule still governs every
    #     layer and every title that does not.
    #
    #     NEGATIVE RESULT, recorded so it is not tried a second time. Dropping
    #     the `wide_bg_slot` half of this condition -- so a layer is extended
    #     only when lb_widemap vouches for the columns -- was tried as a fix for
    #     the army-select streaking and changed NOTHING: the two captures
    #     (screenshots/exp-army and exp-army2, same build otherwise) came out
    #     byte-identical, bbox of difference None. That is itself the useful
    #     finding. `g_adaptive_bg_extend` is false on that screen, so
    #     `wide_bg_slot` is already -1 and NO 2D layer is extended there at all,
    #     which rules the tilemap out as the source of those streaks and points
    #     at the 3D layer or OBJ instead. The change was reverted rather than
    #     kept: it fixed nothing measurable and would have removed upstream's
    #     rule from menus that legitimately want it.
    replace(gpu,
            "            decode_bg_line(0, hud_bgs[i], y, palette, *vram,\n"
            "                           hud_bg_lines[i],\n"
            "                           static_cast<int>(i) == wide_bg_slot ? extra : 0);",
            "            /* legobattles-decomp docs/325 phase 3: lb_widemap's\n"
            "               decline was NOT ENOUGH, and a photograph is what\n"
            "               showed it. With host terrain on, lb_widemap_extends(1)\n"
            "               returns 0 -- but BG1 is also `wide_bg_slot` (it is\n"
            "               the furthest-back layer), and this condition is an\n"
            "               OR, so upstream's rule kept extending it anyway. The\n"
            "               band columns therefore got BG1 content that painted\n"
            "               over the host field, and the host field is the only\n"
            "               thing carrying the shroud: measured, 88.4%% of it is\n"
            "               fog-coloured while the frame showed revealed terrain\n"
            "               across the full 1536 px, and `band 151/1276 kept`\n"
            "               said only 12% of band pixels came from the host.\n"
            "               That is docs/247's \"fog view is square around the\n"
            "               original aspect\", reintroduced by the base-pixel\n"
            "               path rather than by the fog search.\n"
            "\n"
            "               lb_widemap_hostowns() overrides BOTH halves of the\n"
            "               OR, because the host owning a layer is a stronger\n"
            "               statement than either rule for extending it.\n"
            "\n"
            "               BRACES ADDED DELIBERATELY: the loop this sits in is\n"
            "               brace-less, so introducing a declaration without\n"
            "               them made the declaration the whole body and left\n"
            "               decode_bg_line outside the loop. */\n"
            "        {\n"
            "            const bool lb_wide_i =\n"
            "                (static_cast<int>(i) == wide_bg_slot ||\n"
            "                 lb_widemap_extends(hud_bgs[i])) &&\n"
            "                !lb_widemap_hostowns(hud_bgs[i]);\n"
            "            decode_bg_line(0, hud_bgs[i], gy, palette, *vram,\n"
            "                           hud_bg_lines[i],\n"
            "                           lb_wide_i ? extra : 0,\n"
            "                           lb_wide_i ? extra_r : -1);\n"
            "        }")
    replace(gpu,
            "int wide_bg_slot = -1;",
            "int wide_bg_slot = -1;\n"
            "    /* legobattles-decomp: layers this port supplies widened\n"
            "       entries for -- see lb_widemap.cpp. */")
    replace(gpu,
            "bool g_adaptive_bg_extend = false;",
            "bool g_adaptive_bg_extend = false;\n"
            "extern \"C\" int lb_widemap_hostowns(int bg);\n"
            "extern \"C\" int lb_widemap_extends(int bg);  "
            "/* legobattles-decomp */\n"
            "extern \"C\" void lb_widemap_drift_cams(int* fill_tx, int* cam_tx);"
            "  /* legobattles-decomp: LB_DRIFTLOG */\n"
            "extern \"C\" bool lb_field_camera(int* map_x, int* map_y);"
            "  /* legobattles-decomp: LB_DRIFTLOG */")
    done.append("gpu2d.cpp: the shroud layer is widened too")

    # 0v. Scan the guest's fog at RENDER time, not at vblank.
    #
    #     The scan used to run from lb_patches_on_vblank, and read VRAM the
    #     guest had not finished writing. Both candidate shroud indicators came
    #     back saturated there -- BG2 non-zero on all 768 visible tiles and BG3
    #     holding 0x1F on all 768 -- in frames whose captures plainly show
    #     revealed terrain across the middle of the screen. A staging buffer
    #     read mid-update cannot answer "what is on screen".
    #
    #     render_engine_line at y == 0 is the first moment in the frame where
    #     VRAM is exactly the picture about to be drawn, and it is before any
    #     line has been decoded, so the tables are ready for the same frame.
    replace(gpu,
            "void render_engine_line(int engine, int y) {\n"
            "    Unit& u=g_unit[engine];",
            "extern \"C\" void lb_widemap_scan(void);  /* legobattles-decomp */\n"
            "\n"
            "/* legobattles-decomp: WHICH LINE THE SCAN READS VRAM ON.\n"
            "\n"
            "   The comment above argues for y == 0 and the code said 191, so\n"
            "   the two had drifted apart and neither was measured. It matters:\n"
            "   the fog-tile learner drops any frame whose six phases are not\n"
            "   distinct, because such a frame caught BG2 still holding the raw\n"
            "   index before the game's expansion pass replaced it. In one run\n"
            "   of a fixed route that filter dropped 1680 frames out of 1680 --\n"
            "   nothing was ever learned, so lb_widemap_fog declined every call\n"
            "   and the widened band drew NO FOG AT ALL, a hole in the fog over\n"
            "   ground the game is hiding. Another run of the same route on the\n"
            "   same binary used 1454 and dropped 226.\n"
            "\n"
            "   LB_SCANLINE=<n> so the two placements can be compared in one\n"
            "   binary instead of rebuilt and argued about. */\n"
            "static int lb_scan_line(void) {\n"
            "    static int n = -1;\n"
            "    if (n < 0) {\n"
            "        const char* v = std::getenv(\"LB_SCANLINE\");\n"
            "        n = v ? std::atoi(v) : 191;\n"
            "        if (n < 0 || n > 191) n = 191;\n"
            "    }\n"
            "    return n;\n"
            "}\n"
            "\n"
            "void render_engine_line(int engine, int y) {\n"
            "    Unit& u=g_unit[engine];\n"
            "    if (engine == 0 && y == lb_scan_line()) lb_widemap_scan();")
    done.append("gpu2d.cpp: shroud scanned at render time")

    # 10. HUD repositioning: move corner HUD quads out to the widened edges.
    #
    #     HUD repositioning. The in-field HUD is 3D geometry --
    #     OBJ is disabled outright in gameplay -- so it is drawn where the
    #     ORIGINAL 256-wide screen's corner was and sits stranded in the middle
    #     of a wide picture. Measured: the objective button draws at surface
    #     x 323..347 when the widened right edge is 447, so it is 100 px short
    #     of its own corner (docs/212).
    #
    #     Applied to the RASTERISED line rather than to vertices. The polygon
    #     list is already in widened-surface coordinates and the line is flat
    #     RGBA by this point, so there is no depth buffer to fight; translating
    #     vertices would mean patching melonDS's transform, which this build
    #     treats as read-only. See lb_hud.cpp.
    replace(gpu,
            "        const uint32_t* const line3d = snapshot_matches\n"
            "            ? g_wide_3d_frame[g_front].data() +\n"
            "                  static_cast<size_t>(y) * output_width\n"
            "            : nds_gpu3d_wide_line(y);",
            "        if (y == 0) lb_hud_scan(output_width, output_height);"
            "   /* legobattles-decomp */\n"
            "        const uint32_t* const line3d_raw = lb_pair_hold\n"
            "            ? lb_pair_row(y, output_width)\n"
            "            : snapshot_matches\n"
            "            ? g_wide_3d_frame[g_front].data() +\n"
            "                  static_cast<size_t>(y) * output_width\n"
            "            : nds_gpu3d_wide_line(y);\n"
            "        /* docs/343: remember the raster actually paired with this\n"
            "           frame's scroll commit, for the frame that must hold it. */\n"
            "        if (!lb_pair_hold)\n"
            "            lb_pair_store(y, line3d_raw, output_width, output_height,\n"
            "                          extra, extra_y);\n"
            "        /* legobattles-decomp docs/337: re-present a stale 3D\n"
            "           raster at the CURRENT camera. The guest submits\n"
            "           geometry at ~30 Hz -- 42% of still frames and 88% of\n"
            "           moving ones submit none -- so on those frames the\n"
            "           sprites sit where they were when the raster was made\n"
            "           while the ground, decoded live from VRAM, has already\n"
            "           scrolled. lb_reshift_dx() is the camera's travel since\n"
            "           the raster was rendered, and is 0 unless LB_RESHIFT is\n"
            "           set.\n"
            "\n"
            "           NOT ON HUD ROWS: the in-field HUD is 3D geometry in\n"
            "           this same raster and is screen-locked, which is what\n"
            "           made the earlier whole-row shift wrong.\n"
            "\n"
            "           lb_reshift_line() does the shift with the row's ends\n"
            "           handled. This was `line3d_raw + lb_rs`, which reads the\n"
            "           NEXT ROW's opening pixels into this row's tail. */\n"
            "        const int lb_rs =\n"
            "            lb_hud_row_has_box(y) ? 0 : lb_reshift_dx();\n"
            "        const uint32_t* const line3d =\n"
            "            lb_hud_shift_line(\n"
            "                lb_reshift_line(line3d_raw, output_width, lb_rs),\n"
            "                y, output_width);\n"
            "        /* docs/342: keep the row the composite really used. */\n"
            "        if (lb_dump3d_on())\n"
            "            lb_used3d_store(y, line3d, output_width, output_height);\n"
            "        /* docs/342 PROBE: LB_LIVE3D changed nothing measurable.\n"
            "           Either the live raster already equals the snapshot at\n"
            "           this point, or this composite does not run where the\n"
            "           reasoning placed it (after VCount215). Measure both:\n"
            "           the scanline the emulator is on, and how many pixels\n"
            "           of the whole live raster differ from the snapshot. */\n"
            "        if (y == 0 && lb_live3d_probe()) {\n"
            "            int lb_nd = -1;\n"
            "            if (g_wide_3d_width[g_front] == output_width) {\n"
            "                lb_nd = 0;\n"
            "                for (int lb_r = 0; lb_r < output_height; ++lb_r) {\n"
            "                    const uint32_t* lb_s =\n"
            "                        g_wide_3d_frame[g_front].data() +\n"
            "                        static_cast<size_t>(lb_r) * output_width;\n"
            "                    const uint32_t* lb_l = nds_gpu3d_wide_line(lb_r);\n"
            "                    for (int lb_x = 0; lb_x < output_width; ++lb_x)\n"
            "                        if (lb_s[lb_x] != lb_l[lb_x]) ++lb_nd;\n"
            "                }\n"
            "            }\n"
            "            std::fprintf(stderr,\n"
            "                \"[lb_live3d] frame %u vcount %u live_vs_snap %d \"\n"
            "                \"live %d\\n\", lb_dump_frame_get(),\n"
            "                (unsigned)nds_io_debug_read(9, 0x04000006u, 16),\n"
            "                lb_nd, lb_live3d());\n"
            "        }")
    replace(gpu,
            "extern \"C\" int lb_widemap_clear_tile(int bg, uint16_t* out);\n",
            "extern \"C\" int lb_widemap_clear_tile(int bg, uint16_t* out);\n"
            "extern \"C\" void lb_hud_scan(int output_width,\n"
            "                            int output_height, int band,\n"
            "                            int band_y);\n"
            "extern \"C\" const uint32_t* lb_hud_shift_line(\n"
            "    const uint32_t* line, int y, int width);\n"
            "extern \"C\" const uint8_t* lb_hud_cover_row(int y, int width);\n"
            "extern \"C\" int lb_hud_row_has_box(int y);\n"
            "extern \"C\" int lb_reshift_dx(void);   /* docs/337 */\n"
            "extern \"C\" int lb_live3d(void);       /* docs/342 */\n"
            "extern \"C\" int lb_dump3d_on(void);    /* docs/342 */\n"
            "extern \"C\" int lb_live3d_probe(void); /* docs/342 */\n"
            "extern \"C\" void lb_evt(int kind);     /* docs/342 */\n"
            "extern \"C\" int lb_pair_decide(int w, int h);          /* 343 */\n"
            "extern \"C\" int lb_pair_band(void);\n"
            "extern \"C\" int lb_pair_band_y(void);\n"
            "extern \"C\" const uint32_t* lb_pair_row(int y, int w);\n"
            "extern \"C\" void lb_pair_store(int y, const uint32_t* line, int w,\n"
            "                              int h, int band, int band_y);\n"
            "extern \"C\" void lb_pair_on_bg(void);\n"
            "/* docs/348: pair the 3D raster and the widened tables with the\n"
            "   camera the ground in VRAM was committed with. */\n"
            "extern \"C\" int lb_pair_pick(int snap_ok, int snap_x, int snap_y,\n"
            "                            int snap_tag_ok);\n"
            "extern \"C\" void lb_tag_on_bg1(void);\n"
            "extern \"C\" void lb_tag_on_bg2(void);\n"
            "extern \"C\" void lb_widemap_compose_prep(int output_width,\n"
            "                                        int output_height);\n"
            "extern \"C\" void lb_hud_set_list_band(int band, int band_y);\n"
            "extern \"C\" int lb_oam_trace_on(void);  /* docs/347 */\n"
            "extern \"C\" unsigned lb_dump_frame_get(void);\n"
            "extern \"C\" void lb_used3d_store(int y, const uint32_t* line,\n"
            "                                int w, int h);\n"
            "extern \"C\" const uint32_t* lb_reshift_line(\n"
            "    const uint32_t* line, int width, int dx);\n")
    done.append("gpu2d.cpp: HUD anchored to the widened edges")

    # 0h2. Keep the SHROUD off a HUD element this port has moved.
    #
    #      Measured on the running game: engine A's field runs BG3 at priority
    #      0, BG2 (the shroud) at 1, the 3D layer at 2 and BG1 (terrain) at 3.
    #      The in-game HUD is 3D geometry, so the shroud is drawn IN FRONT OF
    #      IT. On the DS that never shows -- the HUD's corners sit over the
    #      player's own ground -- but anchoring a corner into a widened band
    #      puts it on cloud, and it disappears entirely: the bottom-left
    #      element at surface y 232..255 measured 100% fog with not one pixel
    #      of the button in it.
    #
    #      The fog out there is not the game's. It is this port's conservative
    #      default for ground the guest's own 32x24 window has never covered,
    #      so declining to draw it under an element this port has RELOCATED
    #      reveals nothing: the element is opaque and covers those pixels
    #      anyway. lb_hud_cover_row marks only the pixels an opaque HUD pixel
    #      was actually written to, so a transparent margin still shows the
    #      band exactly as it was.
    replace(gpu,
            "        const int hud_center_left =\n"
            "            (256 - g_adaptive_hud_center_width) / 2;",
            "        /* legobattles-decomp: pixels a moved HUD element drew */\n"
            "        const uint8_t* const lb_cover =\n"
            "            lb_hud_cover_row(y, output_width);\n"
            "        const int hud_center_left =\n"
            "            (256 - g_adaptive_hud_center_width) / 2;")
    replace(gpu,
            "                const uint16_t c = line.color[src];\n"
            "                if (!(c & 0x8000u)) continue;",
            "                const uint16_t c = line.color[src];\n"
            "                if (!(c & 0x8000u)) continue;\n"
            "                /* legobattles-decomp: BG2 is the shroud and BG3\n"
            "                   its overlay, and both sit in front of the 3D\n"
            "                   layer the HUD lives on. Neither may cover a\n"
            "                   button this port moved out here. */\n"
            "                if (lb_cover && lb_cover[x] &&\n"
            "                    (line.target & 0x0Cu)) continue;")
    done.append("gpu2d.cpp: the shroud does not cover a moved HUD element")

    # 0w. LB_3DEDGE: dump the widened 3D line across the native boundary.
    #
    #     The left-edge band is the 3D layer (docs/205) -- masking BG0 removes
    #     it, masking BG2 does not -- and it sits at surface columns 96..111,
    #     exactly two tile columns at the start of the native region. What is
    #     still unknown is whether it is already in the 3D buffer or is
    #     introduced when that buffer is composited, and those point at
    #     completely different fixes. Printing the source line across the
    #     boundary answers it directly instead of by argument.
    replace(gpu,
            "        const uint32_t* const attr3d =",
            "        {   /* legobattles-decomp: LB_3DEDGE=1 */\n"
            "            static int lb_edge = -1;\n"
            "            if (lb_edge < 0)\n"
            "                lb_edge = std::getenv(\"LB_3DEDGE\") ? 1 : 0;\n"
            "            static unsigned lb_edge_n = 0;\n"
            "            if (lb_edge && engine == 0 && y == 96 &&\n"
            "                (++lb_edge_n % 240u) == 0u) {\n"
            "                std::fprintf(stderr,\n"
            "                    \"[lb_3dedge] w=%d extra=%d snapshot=%d y=96\\n\",\n"
            "                    output_width, extra, snapshot_matches ? 1 : 0);\n"
            "                /* The scroll bias, from the renderer's own copy\n"
            "                   rather than from a debug read of BGxOFS -- those\n"
            "                   registers are write-only, and reading them back\n"
            "                   returned 0 while the picture plainly showed a\n"
            "                   non-zero offset. This line is what says whether\n"
            "                   the bias correction above has anything to do. */\n"
            "                std::fprintf(stderr,\n"
            "                    \"[lb_3dedge] bgx %u %u %u %u  bgy %u %u %u %u\\n\",\n"
            "                    g_unit[0].bgx[0], g_unit[0].bgx[1],\n"
            "                    g_unit[0].bgx[2], g_unit[0].bgx[3],\n"
            "                    g_unit[0].bgy[0], g_unit[0].bgy[1],\n"
            "                    g_unit[0].bgy[2], g_unit[0].bgy[3]);\n"
            "                for (int lb_b = 80; lb_b < 104; lb_b += 8) {\n"
            "                    char lb_s[200]; int lb_k = 0;\n"
            "                    for (int lb_i = 0; lb_i < 8; ++lb_i)\n"
            "                        lb_k += std::snprintf(lb_s + lb_k,\n"
            "                            sizeof(lb_s) - (size_t)lb_k, \"%06X \",\n"
            "                            line3d ? (line3d[lb_b + lb_i] & 0xFFFFFFu)\n"
            "                                   : 0u);\n"
            "                    std::fprintf(stderr, \"[lb_3dedge] 3d  %3d: %s\\n\",\n"
            "                                 lb_b, lb_s);\n"
            "                }\n"
            "                /* Each 2D layer's own decoded colour at the same\n"
            "                   columns, with the index it was sampled from --\n"
            "                   an extended layer is indexed by output column,\n"
            "                   a native-width one through hud_x. */\n"
            "                for (size_t lb_l = 0; lb_l < hud_bg_count; ++lb_l) {\n"
            "                    const BgLine& lb_ln = hud_bg_lines[lb_l];\n"
            "                    char lb_s[240]; int lb_k = 0;\n"
            "                    for (int lb_i = 80; lb_i < 104; lb_i += 2) {\n"
            "                        const int lb_src = lb_ln.decoded_width > 256\n"
            "                            ? lb_i : (lb_i - extra);\n"
            "                        const uint16_t lb_c =\n"
            "                            (lb_src >= 0 &&\n"
            "                             lb_src < lb_ln.decoded_width)\n"
            "                                ? lb_ln.color[lb_src] : 0u;\n"
            "                        lb_k += std::snprintf(lb_s + lb_k,\n"
            "                            sizeof(lb_s) - (size_t)lb_k, \"%04X \",\n"
            "                            lb_c);\n"
            "                    }\n"
            "                    std::fprintf(stderr,\n"
            "                        \"[lb_3dedge] bg%d le=%d dw=%d prio=%d: %s\\n\",\n"
            "                        hud_bgs[lb_l], lb_ln.left_extra,\n"
            "                        lb_ln.decoded_width, lb_ln.prio, lb_s);\n"
            "                }\n"
            "                std::fflush(stderr);\n"
            "            }\n"
            "        }\n"
            "        const uint32_t* const attr3d =")
    done.append("gpu2d.cpp: LB_3DEDGE boundary dump")

    # 0x. A write path into the live save image.
    #
    #     nds_io_cartridge_save_snapshot already exposes the backup chip's SRAM
    #     for reading, which is all export needs. Import needs the other
    #     direction, and there was none: the image is only ever loaded from disk
    #     at start-up. This adds the minimum -- overwrite the image and mark it
    #     dirty so the runner's own flush writes it out.
    #
    #     It deliberately refuses a size mismatch rather than padding. A LEGO
    #     Battles save is 512 bytes and a file that is not is not one; silently
    #     accepting it would produce a save the game rejects with empty profile
    #     slots, which looks like the import feature is broken rather than like
    #     the file was wrong.
    io_cpp = os.path.join(runner_dir, "src", "io.cpp")
    replace(io_cpp,
            "void nds_set_touch(uint16_t x, uint16_t y, bool down) {",
            "bool nds_io_cartridge_save_restore(const uint8_t* data,\n"
            "                                   uint32_t size) {\n"
            "    /* legobattles-decomp: the save manager's import path. */\n"
            "    if (!data || g_cart_backup.sram.empty()) return false;\n"
            "    if (size != g_cart_backup.sram.size()) return false;\n"
            "    std::memcpy(g_cart_backup.sram.data(), data, size);\n"
            "    g_cart_backup.dirty = true;\n"
            "    return true;\n"
            "}\n"
            "\n"
            "void nds_set_touch(uint16_t x, uint16_t y, bool down) {")
    io_h = os.path.join(runner_dir, "src", "io.h")
    replace(io_h,
            "void     nds_note_insn_retired(int cpu);",
            "/* legobattles-decomp: overwrite the live save image (save\n"
            "   manager import). Refuses a size mismatch. */\n"
            "bool     nds_io_cartridge_save_restore(const uint8_t* data,\n"
            "                                       uint32_t size);\n"
            "void     nds_note_insn_retired(int cpu);")
    done.append("io.cpp: save-image restore for the save manager")

    # 0y. LB_BANK_OPT: the optimisation level for the generated game banks.
    #
    #     The banks are the hot code -- NDS_PROFILE_SCHED puts 56% of emulation
    #     in the ARM9 (docs/204) and that is almost entirely recompiled bank
    #     C -- and they were built at -O2. Whether -O3 helps a body of code this
    #     shape is not something to guess at: every guess this project has made
    #     about the frame budget has been wrong, so it got an A/B instead.
    #
    #     MEASURED, four runs per arm at 1920x1080 on the post-263 lift, which
    #     is the first build where the banks are the whole picture:
    #
    #       emu_ticks    -O2  10.925 10.990 10.963 10.929   mean 10.952
    #                    -O3  10.669 10.796 10.682 10.574   mean 10.680
    #       frame total  -O2  16.337 16.439 16.376 16.319   mean 16.368
    #                    -O3  16.004 16.207 16.104 15.921   mean 16.059
    #       drain (idle) -O2   0.520  0.681  0.547  0.503
    #                    -O3   0.788  0.728  0.844  0.928
    #
    #     Non-overlapping on all three: -O2's best emu (10.925) is worse than
    #     -O3's worst (10.796). -0.27 ms/frame of emulation, -0.31 ms of frame
    #     time, +0.26 ms of idle headroom.
    #
    #     FPS DID NOT MOVE (58.81 -> 59.00, overlapping) and that is the
    #     expected shape, not a contradiction: the port now runs at the DS's
    #     own 59.826 Hz, so time saved shows up as drain_ticks -- the limiter
    #     idling -- and not as more frames. Read the frame budget, not the rate,
    #     once the rate is capped. The headroom is the point: it is what keeps a
    #     frame that runs long from missing its deadline.
    #
    #     Costs a slower, hungrier build over 551 MB of generated C. Build with
    #     LB_JOBS=2. LB_BANK_OPT=-O2 restores the old behaviour exactly.
    cml = os.path.join(runner_dir, "CMakeLists.txt")
    replace(cml,
            "if(TITLE_BANK_SOURCES AND NOT MSVC)\n"
            "    set_source_files_properties(${TITLE_BANK_SOURCES}\n"
            "        PROPERTIES COMPILE_OPTIONS \"-w;-O2;-g0\")\n"
            "endif()",
            "if(NOT DEFINED LB_BANK_OPT)\n"
            "    set(LB_BANK_OPT \"$ENV{LB_BANK_OPT}\")\n"
            "endif()\n"
            "if(NOT LB_BANK_OPT)\n"
            "    set(LB_BANK_OPT \"-O3\")\n"
            "endif()\n"
            "message(STATUS \"legobattles-decomp: game banks at ${LB_BANK_OPT}\")\n"
            "if(TITLE_BANK_SOURCES AND NOT MSVC)\n"
            "    set_source_files_properties(${TITLE_BANK_SOURCES}\n"
            "        PROPERTIES COMPILE_OPTIONS \"-w;${LB_BANK_OPT};-g0\")\n"
            "endif()")
    done.append("CMakeLists.txt: LB_BANK_OPT for the generated banks")

    # 0z. Inline the two per-instruction predicates, in generated banks only.
    #
    #     Every emitted guest instruction calls runtime_should_yield() and
    #     runtime_unwinding(), both out-of-line and in another translation unit.
    #     At 468 M ARM9 instructions a run that is ~1 G calls, and the cost is
    #     not only the call: an opaque call forces guest state out of host
    #     registers at every instruction boundary (docs/204).
    #
    #     Both are dominated by a fast path. runtime_unwinding() is a single
    #     global read. runtime_should_yield() returns false after two cheap
    #     predicates whenever the fast-poll hint is clear, and its own comment
    #     says so. The slow paths are left exactly as they are and are still
    #     called whenever the fast path does not settle it, so behaviour is
    #     unchanged -- this only removes a function call from the common case.
    #
    #     Guarded on NDS_STATIC_CPU so ONLY generated bank translation units see
    #     it. The runner's own callers keep the ordinary functions, which is
    #     what makes this safe to do to shared runtime code.
    rt_cpp = os.path.join(runner_dir, "src", "runtime_arm.cpp")
    # The generated banks compile as C, so every global the inline fast paths
    # touch needs C linkage on its DEFINITION too -- otherwise the bank objects
    # reference an unmangled name that the C++ definition never provides, and
    # the failure is at link time with "undefined reference to g_cycle_cap".
    replace(rt_cpp, "bool g_cpu_fast_poll = true;",
            "extern \"C\" bool g_cpu_fast_poll = true;")
    replace(rt_cpp, "uint32_t g_yield_poll_hint = 1u;",
            "extern \"C\" uint32_t g_yield_poll_hint = 1u;")
    replace(rt_cpp,
            "unsigned long long g_cycle_cap = 0;",
            "extern \"C\" unsigned long long g_cycle_cap = 0;")
    # g_deferred_cycles is TU-private too, and lb_tick needs it.
    replace(rt_cpp,
            "uint32_t g_deferred_cycles = 0;\n}",
            "/* legobattles-decomp: MOVED out of the anonymous namespace, with C\n"
            "   linkage, so the banks' inline runtime_tick can reach it. Same\n"
            "   variable and same writers -- leaving the original in place as\n"
            "   well made every reference ambiguous. */\n"
            "}\n"
            "extern \"C\" uint32_t g_deferred_cycles = 0;")
    # The IRQ-pending cache is defined in io.cpp as C++; the C banks need it
    # under an unmangled name for the same reason as the globals above.
    io_cpp2 = os.path.join(runner_dir, "src", "io.cpp")
    replace(io_cpp2, "uint32_t g_nds_irq_pending_cache[2] = {0, 0};",
            "extern \"C\" uint32_t g_nds_irq_pending_cache[2] = {0, 0};")
    # ... and its existing declaration has to agree, or the definition is a
    # "conflicting declaration with C linkage" against io.h's C++ one.
    replace(io_h, "extern uint32_t g_nds_irq_pending_cache[2];",
            "extern \"C\" uint32_t g_nds_irq_pending_cache[2];")
    # g_unwinding is TU-private; the inline form needs it visible.
    replace(rt_cpp,
            "namespace {\nbool g_unwinding = false;",
            "/* legobattles-decomp: external linkage so the banks can inline\n"
            "   runtime_unwinding(). Same variable, same writers. */\n"
            "extern \"C\" bool g_unwinding = false;\n"
            "namespace {")
    # runtime_arm.h is the recompiler's, a sibling of the runner in the tree.
    rt_h = os.path.join(os.path.dirname(runner_dir), "recompiler", "armv4t",
                        "runtime_arm.h")
    # At the END of the header. The fast paths reference globals and the real
    # predicates, all of which must be declared first -- and a function-like
    # macro named runtime_unwinding() placed earlier mangles that function's
    # own later declaration, which is exactly what the first attempt did.
    replace(rt_h,
            "void runtime_init(void* bus_handle);",
            "void runtime_init(void* bus_handle);\n"
            "\n"
            "#ifdef NDS_STATIC_CPU\n"
            "/* legobattles-decomp: per-instruction predicates, inlined for the\n"
            "   generated banks. See apply_hooks 0z. */\n"
            "extern bool g_unwinding;\n"
            "extern uint32_t g_runtime_break_pc;\n"
            "extern bool g_cpu_fast_poll;\n"
            "extern uint32_t g_yield_poll_hint;\n"
            "extern unsigned long long g_cycle_cap;\n"
            "\n"
            "static inline bool lb_should_yield(void) {\n"
            "    if (g_cpu_fast_poll && !g_yield_poll_hint) {\n"
            "        const bool bp = g_runtime_break_pc &&\n"
            "            (g_cpu.R[15] & ~1u) == (g_runtime_break_pc & ~1u);\n"
            "        const bool cc = g_cycle_cap != 0 &&\n"
            "            g_runtime_cycles >= g_cycle_cap;\n"
            "        if (!bp && !cc) return false;\n"
            "    }\n"
            "    /* Parenthesised so the macro below does not expand here and\n"
            "       recurse: this is the real out-of-line predicate. */\n"
            "    return (runtime_should_yield)();\n"
            "}\n"
            "#define runtime_should_yield() lb_should_yield()\n"
            "#define runtime_unwinding() (g_unwinding)\n"
            "\n"
            "/* THE TRACE GUARD WAS TRIED HERE AND MADE IT WORSE. Measured,\n"
            "   512x384, same route:\n"
            "\n"
            "     without   44.07 fps   20.614 ms emu/frame\n"
            "     with      41.72 fps   21.818 ms\n"
            "               40.96 fps   22.223 ms\n"
            "               41.50 fps   21.940 ms\n"
            "\n"
            "   runtime_trace_event is emitted on every memory access and its\n"
            "   first statement is `if (!g_runtime_deep_trace) return;`, so in\n"
            "   play each one is a cross-TU call made only to find there is\n"
            "   nothing to do. Hoisting that guard inline looked free --\n"
            "   g_runtime_deep_trace is already extern in this header, so\n"
            "   unlike the helpers docs/204 lists it needs no linkage surgery.\n"
            "\n"
            "   It is not free. Memory accesses are far more numerous than the\n"
            "   two predicates above, and a load plus a branch at every one of\n"
            "   them costs more in code size than the call it removes -- the\n"
            "   same outcome as runtime_tick, for the same reason. Kept as a\n"
            "   measurement so it is not tried a third time. */\n"
            "\n"
            "/* runtime_tick was tried here too and MADE IT SLIGHTLY WORSE:\n"
            "   44.14 / 43.43 fps inlined against 44.31 / 44.44 / 44.15\n"
            "   without. Its body is far larger than the two predicates above,\n"
            "   so expanding it at every instruction site costs more in code\n"
            "   size than it saves in call overhead. Not every per-instruction\n"
            "   call is worth inlining, and measuring is the only way to tell\n"
            "   which. See docs/204. */\n"
            "\n"
            "/* runtime_code_cycles was tried here as well, with always_inline on\n"
            "   it and on lb_should_yield (docs/349): exact by the LB_COUNTS_AT\n"
            "   gate, but the binary grew 154.8 -> 256.4 MB, a full build took\n"
            "   ~110 min, and emulation measured 9.1 -> 9.3..11.1 ms/frame.\n"
            "   Withdrawn; see apply_hooks 0z2. */\n"
            "#endif")
    done.append("runtime_arm.h: inline per-instruction predicates in banks")

    # 0z2. TRIED AND WITHDRAWN (docs/349): runtime_code_cycles inline in the
    #      banks, plus always_inline on 0z's lb_should_yield.
    #
    #      runtime_code_cycles was the largest runtime helper in an LB_SAMPLE
    #      profile of a recorded battle (8.5% of the main thread), and at each
    #      call site `pc` is a compile-time constant, so an inline form over two
    #      C mirrors of the latched code timing looked like a clean win. It
    #      passed the LB_COUNTS_AT exactness gate (identical instruction counts
    #      at five frames of a recorded replay). It did NOT pay:
    #
    #        binary       154.8 MB -> 256.4 MB  (always_inline at ~400k sites)
    #        full build   ~40 min  -> ~110 min  (cc1 at 2-3 GB per shard)
    #        emu, pan     9.107 / 9.165 ms -> 11.130 / 9.312 ms per frame
    #
    #      No faster at best, 2 ms slower at worst -- the verdict docs/204
    #      recorded for inlining runtime_tick and the trace guard, for the same
    #      reason: the instruction-cache cost of expanding code at every guest
    #      instruction outweighs the calls it removes. Recorded here and in
    #      runtime_arm.h so it is not tried a fourth time.

    # 0z3. The MPU cacheability answers, memoised per 4 KiB page (docs/349).
    #
    #      runtime_mem_cycles asks cp15_data_cacheable on every ARM9 data access
    #      outside the TCMs, and the answer is a scan of all eight protection
    #      regions: 1.5% of the main thread in a recorded late battle, with
    #      cp15_code_cacheable (asked on every code-timing page change) another
    #      0.6%. Both answers depend only on the address and on CP15 state
    #      (control bits, c2 cacheability, the c6 regions), and every write of
    #      that state bumps g_cp15_timing_generation (runtime_coproc_write for
    #      c1/c2/c6/c9, and cp15_reset), which is already how bus.cpp keys its
    #      own code-timing page cache. So each answer is kept per page, keyed on
    #      the page AND the generation.
    #
    #      A page is only cached when no enabled region's base or end falls
    #      strictly inside it -- then every address in it has the same answer.
    #      Architecturally a region is at least 4 KiB and size-aligned, so that
    #      is every page; the check keeps it exact even for a guest that
    #      programs a smaller (UNPREDICTABLE) region, which set_mpu_region
    #      would model rather than reject.
    cp15_cpp = os.path.join(runner_dir, "src", "cp15.cpp")
    replace(cp15_cpp,
            "bool cp15_code_cacheable(uint32_t addr) {\n"
            "    if (!(g_cp15.control & (1u << 12))) return false;      // I-cache disabled\n"
            "    if (!(g_cp15.control & 1u)) return true;               // MPU disabled: global cache bit\n"
            "    const uint32_t icache_bits = g_cache_cfg[1];           // c2,c0,1 = instr cacheable\n"
            "    for (int i = 7; i >= 0; --i) {\n"
            "        if (mpu_region_contains(static_cast<uint32_t>(i), addr))\n"
            "            return (icache_bits >> i) & 1u;\n"
            "    }\n"
            "    return false;\n"
            "}\n"
            "\n"
            "bool cp15_data_cacheable(uint32_t addr) {\n"
            "    if (!(g_cp15.control & (1u << 2))) return false;       // D-cache disabled\n"
            "    if (!(g_cp15.control & 1u)) return true;               // MPU disabled: global cache bit\n"
            "    const uint32_t dcache_bits = g_cache_cfg[0];           // c2,c0,0 = data cacheable\n"
            "    for (int i = 7; i >= 0; --i) {\n"
            "        if (mpu_region_contains(static_cast<uint32_t>(i), addr))\n"
            "            return (dcache_bits >> i) & 1u;\n"
            "    }\n"
            "    return false;\n"
            "}\n",
            "/* legobattles-decomp 0z3: see apply_hooks 0z3. The scan, unchanged,\n"
            "   then the per-page memo in front of it. */\n"
            "static bool lb_region_cacheable(uint32_t bits, uint32_t addr) {\n"
            "    for (int i = 7; i >= 0; --i) {\n"
            "        if (mpu_region_contains(static_cast<uint32_t>(i), addr))\n"
            "            return (bits >> i) & 1u;\n"
            "    }\n"
            "    return false;\n"
            "}\n"
            "static bool lb_page_has_one_answer(uint32_t page) {\n"
            "    const uint64_t lo = uint64_t{page} << 12, hi = lo + 0x1000u;\n"
            "    for (unsigned i = 0; i < 8u; ++i) {\n"
            "        if (!g_mpu_region_end[i]) continue;\n"
            "        const uint64_t b = g_mpu_region_base[i], e = g_mpu_region_end[i];\n"
            "        if ((b > lo && b < hi) || (e > lo && e < hi)) return false;\n"
            "    }\n"
            "    return true;\n"
            "}\n"
            "struct LbCacheMemo { uint32_t page, gen; bool v; };\n"
            "static bool lb_memo_cacheable(LbCacheMemo* memo, uint32_t bits,\n"
            "                              uint32_t addr) {\n"
            "    const uint32_t page = addr >> 12;\n"
            "    LbCacheMemo& m = memo[page & 63u];\n"
            "    if (m.gen == g_cp15_timing_generation && m.page == page) return m.v;\n"
            "    const bool v = lb_region_cacheable(bits, addr);\n"
            "    if (lb_page_has_one_answer(page))\n"
            "        m = LbCacheMemo{page, g_cp15_timing_generation, v};\n"
            "    return v;\n"
            "}\n"
            "\n"
            "bool cp15_code_cacheable(uint32_t addr) {\n"
            "    if (!(g_cp15.control & (1u << 12))) return false;      // I-cache disabled\n"
            "    if (!(g_cp15.control & 1u)) return true;               // MPU disabled: global cache bit\n"
            "    static LbCacheMemo memo[64];\n"
            "    return lb_memo_cacheable(memo, g_cache_cfg[1], addr);  // c2,c0,1 = instr cacheable\n"
            "}\n"
            "\n"
            "bool cp15_data_cacheable(uint32_t addr) {\n"
            "    if (!(g_cp15.control & (1u << 2))) return false;       // D-cache disabled\n"
            "    if (!(g_cp15.control & 1u)) return true;               // MPU disabled: global cache bit\n"
            "    static LbCacheMemo memo[64];\n"
            "    return lb_memo_cacheable(memo, g_cache_cfg[0], addr);  // c2,c0,0 = data cacheable\n"
            "}\n")
    done.append("cp15.cpp: MPU cacheability memoised per page (0z3)")

    # 0r. Report which layer, if any, the engine-A widener extends.
    #
    #     The widened columns render black although the tile entries are
    #     correct, land exactly where the renderer computes, reference tiles
    #     whose graphics are resident, and width_extra really is 0x100. Every
    #     input checks out, so the remaining candidate is that no layer is being
    #     decoded with `extra` at all -- wide_bg_slot staying -1 because
    #     g_adaptive_bg_extend was never set. Guessing at that has already cost
    #     two runs; this asks.
    replace(gpu,
            "    const bool snapshot_matches =\n"
            "        g_wide_3d_width[g_front] == output_width;",
            "    {   /* legobattles-decomp: which layer extends? */\n"
            "        static int lb_said = 0;\n"
            "        if (!lb_said && std::getenv(\"LB_WIDEMAP\")) {\n"
            "            lb_said = 1;\n"
            "            std::fprintf(stderr,\n"
            "                \"[lb_wideslot] bg_extend=%d slot=%d of %u bgs\"\n"
            "                \" (bg1cnt %04X prio %u)\\n\",\n"
            "                g_adaptive_bg_extend ? 1 : 0, wide_bg_slot,\n"
            "                (unsigned)hud_bg_count, (unsigned)u.bgcnt[1],\n"
            "                (unsigned)(u.bgcnt[1] & 3u));\n"
            "            std::fflush(stderr);\n"
            "        }\n"
            "    }\n"
            "    /* legobattles-decomp docs/342: LB_LIVE3D composites the LIVE\n"
            "       raster. This composite runs at the 263->0 wrap, after the\n"
            "       VBlank handler wrote this frame's BG scroll and after\n"
            "       VCount215 rasterised the list latched at that same VBlank.\n"
            "       The live raster is therefore the 3D the DS pairs with this\n"
            "       2D; the snapshot is the one displayed during the frame that\n"
            "       just ended -- one guest update older than the ground.\n"
            "\n"
            "       docs/348: neither is right on every frame -- the game's own\n"
            "       pacing puts the 3D list one VBlank ahead of its ground on\n"
            "       some updates -- so the choice is made per frame by camera,\n"
            "       above, and this merely names it. */\n"
            "    const bool snapshot_matches = lb_use_snap != 0;\n"
            "    /* docs/348: THE LIVE RASTER MUST BE FINISHED BEFORE THE ROWS FAN\n"
            "       OUT. SoftRenderer::GetLine waits on a counting semaphore the\n"
            "       render thread posts once per scanline and ONCE at the end;\n"
            "       several workers blocked in it when that last post arrives\n"
            "       would leave all but one blocked. One wait here, on this\n"
            "       thread, for the last row -- after it every worker's GetLine\n"
            "       returns without waiting. */\n"
            "    if (!snapshot_matches && output_height > 0)\n"
            "        (void)nds_gpu3d_wide_line(output_height - 1);")
    done.append("gpu2d.cpp: report the extended layer choice")

    # 0l. THE BANDING. Texture width is fixed at setup; upload width is not.
    #
    #     presentation.textures[] is created once at screen_widths[], which comes
    #     from adaptive_max_width. The per-frame upload passes a pitch of
    #     `<screen>_width * 4`, where that width is whatever
    #     nds_gpu2d_adaptive_framebuffer returned THIS FRAME -- and it returns
    #     the native 256 whenever widening is not engaged for that screen.
    #
    #     Upload 256-pitch pixels into a 448-wide texture and SDL reads 448
    #     pixels per row from a 256-pixel stride: every row starts 192 pixels
    #     before it should, and the picture shears into alternating bands of
    #     content and black. That is the reported corruption, and it happens with
    #     the host compositor switched OFF, which is why three strides spent on
    #     the compositor never touched it.
    #
    #     Recreating the texture when the width changes keeps the pitch and the
    #     texture agreeing. Width changes are rare -- engaging or disengaging the
    #     widened path -- so this is not a per-frame cost.
    edit(front, "struct FrontendPresentation {",
         ["    int texture_widths[2]{kScreenWidth, kScreenWidth};"
          "  /* legobattles-decomp */"],
         indent="")
    edit(front, "void destroy_presentation(FrontendPresentation& presentation) {",
         ["// legobattles-decomp: keep a screen's texture the same width as the",
          "// pixels being uploaded into it. See the note at the call sites.",
          "void lb_fit_texture(FrontendPresentation& presentation, int screen, int width) {",
          "    if (width <= 0 || screen < 0 || screen > 1) return;",
          "    if (presentation.texture_widths[screen] == width) return;",
          "    // HEIGHT IS THE MAXIMUM, NOT kScreenHeight. render_screen()'s own",
          "    // comment says the texture is 'the MAXIMUM size' and that a source",
          "    // rect selects the part the guest actually drew -- but this created",
          "    // it 192 tall, so for a 512-row surface its guard",
          "    // `lb_sh < lb_th` was 512 < 192 = false, no source rect was used,",
          "    // and SDL stretched the TOP 192 rows over the whole window.",
          "    // Measured from the shroud metatile's period in window captures:",
          "    // horizontal scale client_w/surface_w = 2.0, vertical scale",
          "    // client_h/192 = 2.25 / 3.5 / 5.375 at 640x480 / 1280x720 /",
          "    // 1920x1080 -- an anisotropic stretch of up to 2.7:1, with rows",
          "    // 192..511 never reaching the screen: the UI and world did",
          "    // not scale with the window size.",
          "    // 768 here MUST equal kMaxAdaptiveHeight in gpu2d.cpp and the",
          "    // height at create_presentation; all three are patched together.",
          "    SDL_Texture* tex = SDL_CreateTexture(",
          "        presentation.renderers[presentation.separate ? screen : 0],",
          "        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,",
          "        width, 768);",
          "    if (!tex) return;   // keep the old one rather than lose the screen",
          "    if (presentation.textures[screen])",
          "        SDL_DestroyTexture(presentation.textures[screen]);",
          "    presentation.textures[screen] = tex;",
          "    presentation.texture_widths[screen] = width;",
          "    std::fprintf(stderr, \"[lb] screen %d texture -> %d px\\n\",",
          "                 screen, width);",
          "}",
          ""],
         after=False, indent="")
    replace(front,
            "    uint64_t start = SDL_GetPerformanceCounter();\n"
            "    SDL_UpdateTexture(presentation.textures[0], nullptr, top_pixels,\n"
            "                      top_width * sizeof(uint32_t));\n"
            "    SDL_UpdateTexture(presentation.textures[1], nullptr, bottom_pixels,\n"
            "                      bottom_width * sizeof(uint32_t));",
            "    lb_fit_texture(presentation, 0, top_width);     /* legobattles */\n"
            "    lb_fit_texture(presentation, 1, bottom_width);  /* legobattles */\n"
            "    uint64_t start = SDL_GetPerformanceCounter();\n"
            "    /* UPLOAD ONLY THE ROWS THAT EXIST.\n"
            "\n"
            "       A null rect means the WHOLE texture, and these textures are\n"
            "       created at the adaptive maximum height. The top screen's\n"
            "       source is a 256x192 Frame -- std::array<uint32_t, 256*192>,\n"
            "       196,608 bytes -- so this asked SDL to read 256 x 768 x 4 =\n"
            "       786,432 bytes from it: 589,824 bytes past the end of the\n"
            "       object every frame, and past the end of g_fb entirely when\n"
            "       g_front is 1.\n"
            "\n"
            "       The HEIGHT twin of the WIDTH bug hook 3c2 already fixed --\n"
            "       same cause, the texture being bigger than the picture --\n"
            "       and it survived because a stray READ has no symptom until\n"
            "       the allocation moves.\n"
            "\n"
            "       lb_present_src_h[] is set just above this and is exactly the\n"
            "       real height of each surface. Nothing drawn changes:\n"
            "       render_screen already crops with its own source rect, and\n"
            "       the rows this stops writing held whatever the out-of-bounds\n"
            "       read produced. */\n"
            "    {\n"
            "        const SDL_Rect lb_top_src{0, 0, (int)top_width,\n"
            "                                  lb_present_src_h[0] > 0\n"
            "                                      ? lb_present_src_h[0] : 192};\n"
            "        SDL_UpdateTexture(presentation.textures[0], &lb_top_src,\n"
            "                          top_pixels,\n"
            "                          top_width * sizeof(uint32_t));\n"
            "        const SDL_Rect lb_bot_src{0, 0, (int)bottom_width,\n"
            "                                  lb_present_src_h[1] > 0\n"
            "                                      ? lb_present_src_h[1] : 192};\n"
            "        SDL_UpdateTexture(presentation.textures[1], &lb_bot_src,\n"
            "                          bottom_pixels,\n"
            "                          bottom_width * sizeof(uint32_t));\n"
            "    }")
    replace(front,
            "        SDL_UpdateTexture(presentation.textures[1], nullptr, bottom_pixels,\n"
            "                          bottom_width * sizeof(uint32_t));",
            "        lb_fit_texture(presentation, 1, bottom_width);  /* legobattles */\n"
            "        SDL_UpdateTexture(presentation.textures[1], nullptr, bottom_pixels,\n"
            "                          bottom_width * sizeof(uint32_t));")
    done.append("frontend.cpp: texture width follows the upload width")

    # 0m. LB_DUMP_ADAPTIVE: write the surface that is actually presented.
    #
    #     Every reading of the black column bands so far came from a screenshot
    #     of the window, where the surface has already been scaled by a
    #     non-integer factor -- so band edges could not be counted in guest
    #     columns and three different explanations all fitted the same picture.
    #     This dumps `bottom_pixels` itself, which is the exact buffer
    #     nds_gpu2d_adaptive_framebuffer returned, at its real width.
    #
    #     Diagnostic only, and off unless the variable is set.
    replace(front,
            "    lb_fit_texture(presentation, 0, top_width);     /* legobattles */\n"
            "    lb_fit_texture(presentation, 1, bottom_width);  /* legobattles */\n"
            "    uint64_t start = SDL_GetPerformanceCounter();",
            "    lb_fit_texture(presentation, 0, top_width);     /* legobattles */\n"
            "    lb_fit_texture(presentation, 1, bottom_width);  /* legobattles */\n"
            "    {   /* legobattles-decomp: LB_DUMP_ADAPTIVE=<dir> */\n"
            "        static const char* lb_dir = std::getenv(\"LB_DUMP_ADAPTIVE\");\n"
            "        static int lb_seq = 0, lb_tick = 0;\n"
            "        /* LB_DUMP_EVERY=N: frames between ring writes, default 30.\n"
            "           A run measuring the frame budget sets it high and pays\n"
            "           nothing, which is the only way to see the budget the\n"
            "           PLAYER gets: play.bat never sets LB_DUMP_ADAPTIVE at all,\n"
            "           while lb_exp.py sets it on every run. */\n"
            "        static const int lb_every = [] {\n"
            "            const char* v = std::getenv(\"LB_DUMP_EVERY\");\n"
            "            const int n = v ? std::atoi(v) : 0;\n"
            "            return n > 0 ? n : 30;\n"
            "        }();\n"
            "        static const int lb_seqmax = [] {\n"
            "            const char* v = std::getenv(\"LB_DUMP_SEQ\");\n"
            "            const int n = v ? std::atoi(v) : 0;\n"
            "            return n > 0 ? n : 0;\n"
            "        }();\n"
            "        /* LB_DUMP_START=M: skip M presents before recording.\n"
            "           Without it a sequence captures frames 0..N-1, which is\n"
            "           BOOT AND MENUS -- the note above this block already said\n"
            "           a one-shot prefix only ever captures boot, and the first\n"
            "           recording taken here did exactly that. Measured on it:\n"
            "           96 of 178 pairs were static menu frames and the rest were\n"
            "           screen transitions whose apparent shift saturated any\n"
            "           search window, which looks like violent jitter and is\n"
            "           nothing of the kind. */\n"
            "        static const int lb_seqstart = [] {\n"
            "            const char* v = std::getenv(\"LB_DUMP_START\");\n"
            "            const int n = v ? std::atoi(v) : 0;\n"
            "            return n > 0 ? n : 0;\n"
            "        }();\n"
            "        /* docs/348: LB_DUMP_FRAME=N records nothing until the\n"
            "           WIDENED-COMPOSITE frame counter (lb_dump_frame_get, the\n"
            "           number every trace line carries) reaches N. LB_DUMP_START\n"
            "           counts presents, and a boot's menus are ~1,150 of them, so\n"
            "           a start chosen from a trace's frame numbers landed in the\n"
            "           menus and recorded 40 frames of 'frame 0'. */\n"
            "        static const unsigned lb_seqframe = [] {\n"
            "            const char* v = std::getenv(\"LB_DUMP_FRAME\");\n"
            "            const int n = v ? std::atoi(v) : 0;\n"
            "            return n > 0 ? (unsigned)n : 0u;\n"
            "        }();\n"
            "        /* docs/348: LB_DUMP_EDGE=1 records only frames whose\n"
            "           VERTICAL band is off-centre -- the camera near the map's\n"
            "           north or south edge, where the clip-space vertical band\n"
            "           and the old post-clip one differ. A replay does not land\n"
            "           on the same frame numbers twice, so the frames are\n"
            "           chosen by what is on screen instead. */\n"
            "        static const bool lb_seqedge = [] {\n"
            "            const char* v = std::getenv(\"LB_DUMP_EDGE\");\n"
            "            return v && v[0] == '1';\n"
            "        }();\n"
            "        const int lb_edge_h = (int)nds_gpu3d_output_height();\n"
            "        const bool lb_edge_ok = !lb_seqedge ||\n"
            "            (lb_edge_h > 192 &&\n"
            "             lb_shown_band_y() != (lb_edge_h - 192) / 2);\n"
            "        static int lb_skipped = 0;\n"
            "        /* A RING, not the first 16 frames. A one-shot prefix only\n"
            "           ever captures boot, which is the one part of the run\n"
            "           nobody is asking about; the harness needs whatever was\n"
            "           on screen when it confirmed it had reached gameplay. */\n"
            "        /* THE SURFACE'S REAL HEIGHT, not 192. The dump was written\n"
            "           when every surface was 192 rows tall; once the window\n"
            "           buys rows as well as columns it silently cropped the\n"
            "           bottom quarter off the very picture it exists to\n"
            "           measure. */\n"
            "        /* BOTH PHYSICAL SCREENS, EACH NAMED.\n"
            "\n"
            "           This wrote only bottom_pixels, and numbered the files with\n"
            "           the RING SLOT: `adaptive-0-...` and `adaptive-1-...` were\n"
            "           two moments of the SAME screen, 30 frames apart. Reading\n"
            "           that pair as screen 0 and screen 1 is what produced the\n"
            "           false report that \"the top screen mirrors the field, there\n"
            "           is no minimap\" -- the top screen had never been dumped at\n"
            "           all. RETRACTED; the minimap is on the top screen.\n"
            "\n"
            "           The per-screen shots come from the NATIVE accessor because\n"
            "           g_fb is indexed by PHYSICAL screen with the POWCNT1 bit-15\n"
            "           routing already applied per scanline, so screen 0 is the\n"
            "           top LCD whichever engine drew it -- and a Frame is exactly\n"
            "           256x192, so no height guess can read out of bounds. */\n"
            "        const int lb_dh = (int)nds_gpu3d_output_height();\n"
            "        if (lb_dir && lb_dh > 0 &&\n"
            "            (!lb_seqstart || ++lb_skipped > lb_seqstart) &&\n"
            "            (!lb_seqframe || lb_dump_frame_get() >= lb_seqframe) &&\n"
            "            lb_edge_ok &&\n"
            "            ((lb_tick++ % lb_every) == 0) &&\n"
            "            lb_dump_wanted() &&\n"
            "            (!lb_seqmax || lb_seq < lb_seqmax)) {\n"
            "            /* LB_DUMP_SEQ=N turns the 4-slot ring into a RECORDING\n"
            "               of N consecutive frames, numbered 0..N-1.\n"
            "\n"
            "               The ring answers \"what was on screen when the\n"
            "               harness reached gameplay\", which is the right\n"
            "               question for a still. It cannot answer the ones the\n"
            "               project owner is actually reporting -- jitter, shake,\n"
            "               a shroud that flickers -- because every one of those\n"
            "               is a property of the DIFFERENCE between consecutive\n"
            "               frames, and the ring overwrites three of every four.\n"
            "               Grading those from stills is what let a build ship\n"
            "               that shakes.\n"
            "\n"
            "               With LB_DUMP_SEQ set, LB_DUMP_EVERY should be 1: the\n"
            "               frames must be adjacent for a per-frame delta to mean\n"
            "               anything. */\n"
            "            const int lb_slot = lb_seqmax ? lb_seq++ : (lb_seq++ & 3);\n"
            "            /* PAIR THE PICTURE TO THE NUMBERS. The dump is named\n"
            "               by ring SLOT and every other trace is keyed by the\n"
            "               frame counter, and nothing recorded the mapping --\n"
            "               so a pixel measurement could not be joined to the\n"
            "               polygon list for the same instant at all.\n"
            "               tools/lb_groundshift.py paired them by index and\n"
            "               matched 0 of 60 pairs, because those are different\n"
            "               numbers. One line per written frame. */\n"
            "            std::fprintf(stderr,\n"
            "                         \"[lb_dumpidx] slot %d frame %d\\n\",\n"
            "                         lb_slot, (int)lb_dump_frame_get());\n"
            "            std::fflush(stderr);\n"
            "            struct LbDump {\n"
            "                static void go(const char* dir, const char* tag,\n"
            "                               int slot, const uint32_t* px,\n"
            "                               int w, int h) {\n"
            "                    if (!px || w <= 0 || h <= 0) return;\n"
            "                    char path[512];\n"
            "                    std::snprintf(path, sizeof path,\n"
            "                                  \"%s/%s-%d-%dx%d.ppm\",\n"
            "                                  dir, tag, slot, w, h);\n"
            "                    FILE* fh = std::fopen(path, \"wb\");\n"
            "                    if (!fh) return;\n"
            "                    std::fprintf(fh, \"P6\\n%d %d\\n255\\n\", w, h);\n"
            "                    /* ONE fwrite PER ROW, NOT PER PIXEL.\n"
            "\n"
            "                       THIS IS THE ~2 ms THAT docs/264 COULD NOT\n"
            "                       ACCOUNT FOR. The block sits inside\n"
            "                       phase_present_ticks but ahead of the\n"
            "                       upload/draw/swap timers, so no phase counter\n"
            "                       could see it, and lb_exp.py arms\n"
            "                       LB_DUMP_ADAPTIVE on EVERY run while play.bat\n"
            "                       never does -- so every fps figure this\n"
            "                       project has recorded is depressed by it and\n"
            "                       the player has never paid it.\n"
            "\n"
            "                       Three bytes per stdio call, measured in\n"
            "                       isolation at 45.3 ns a pixel:\n"
            "                       55 ms for a 1712x710 surface, which over the\n"
            "                       30-frame ring is ~1.8 ms on every frame. The\n"
            "                       prediction was checked against the observed\n"
            "                       gap at two sizes -- 2.37 predicted / 2.005\n"
            "                       measured at 1712x710, 1.30 / 0.985 at 946x516\n"
            "                       -- and the gap held near 2.0 across four runs\n"
            "                       while adaptive_ticks varied 5.42..7.94, so it\n"
            "                       tracks SURFACE SIZE, not compositor cost.\n"
            "\n"
            "                       Same bytes in the same order; only the call\n"
            "                       count changes. 2048 columns at a time keeps\n"
            "                       the buffer a bounded 6 KB at any width. */\n"
            "                    unsigned char row[2048 * 3];\n"
            "                    for (int y = 0; y < h; ++y) {\n"
            "                        const uint32_t* src =\n"
            "                            px + (size_t)y * (size_t)w;\n"
            "                        int x = 0;\n"
            "                        while (x < w) {\n"
            "                            const int n =\n"
            "                                (w - x) > 2048 ? 2048 : (w - x);\n"
            "                            unsigned char* dst = row;\n"
            "                            for (int i = 0; i < n; ++i) {\n"
            "                                const uint32_t p = src[x + i];\n"
            "                                *dst++ =\n"
            "                                    (unsigned char)((p >> 16) & 0xFFu);\n"
            "                                *dst++ =\n"
            "                                    (unsigned char)((p >> 8) & 0xFFu);\n"
            "                                *dst++ =\n"
            "                                    (unsigned char)(p & 0xFFu);\n"
            "                            }\n"
            "                            std::fwrite(row, 1, (size_t)n * 3u, fh);\n"
            "                            x += n;\n"
            "                        }\n"
            "                    }\n"
            "                    std::fclose(fh);\n"
            "                }\n"
            "            };\n"
            "            /* What is actually presented into each texture. The\n"
            "               height is only the adaptive one when the surface is\n"
            "               genuinely wide; a native 256 buffer is a 192-row\n"
            "               Frame and reading lb_dh rows from it would run off\n"
            "               the end -- the same fault as the top_width lie. */\n"
            "            LbDump::go(lb_dir, \"presented-top\", lb_slot, top_pixels,\n"
            "                       (int)top_width,\n"
            "                       (int)top_width > 256 ? lb_dh : 192);\n"
            "            LbDump::go(lb_dir, \"presented-bot\", lb_slot,\n"
            "                       bottom_pixels, (int)bottom_width,\n"
            "                       (int)bottom_width > 256 ? lb_dh : 192);\n"
            "            /* docs/342: the 3D layer the compositor used for this\n"
            "               same frame, magenta where transparent. */\n"
            "            if (lb_dump3d_on()) {\n"
            "                int lb_w3 = 0, lb_h3 = 0;\n"
            "                const uint32_t* lb_l3 = lb_used3d_rgb(&lb_w3, &lb_h3);\n"
            "                LbDump::go(lb_dir, \"layer3d\", lb_slot, lb_l3,\n"
            "                           lb_w3, lb_h3);\n"
            "            }\n"
            "            /* The two physical LCDs, native, always 256x192. */\n"
            "            LbDump::go(lb_dir, \"screen-top\", lb_slot,\n"
            "                       nds_gpu2d_framebuffer(0), 256, 192);\n"
            "            LbDump::go(lb_dir, \"screen-bot\", lb_slot,\n"
            "                       nds_gpu2d_framebuffer(1), 256, 192);\n"
            "            /* WHICH ENGINE FEEDS WHICH LCD, recorded not assumed. */\n"
            "            static bool lb_said = false;\n"
            "            if (!lb_said) {\n"
            "                lb_said = true;\n"
            "                const unsigned lb_pc = nds_powercontrol9();\n"
            "                std::fprintf(stderr,\n"
            "                             \"[lb_screens] POWCNT1=%04X engine A on \"\n"
            "                             \"%s  top=%dpx bottom=%dpx height=%d\\n\",\n"
            "                             lb_pc,\n"
            "                             (lb_pc & 0x8000u) ? \"TOP\" : \"BOTTOM\",\n"
            "                             (int)top_width, (int)bottom_width, lb_dh);\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "    uint64_t start = SDL_GetPerformanceCounter();")
    done.append("frontend.cpp: LB_DUMP_ADAPTIVE presented-surface dump")

    # 0n. THE BLACK BARS. The window is sized for a screen it does not draw.
    #
    #     The stacked layout opens the window at kScreenHeight * 2, because
    #     stacked means top screen above bottom screen. Enhanced draws ONLY the
    #     bottom screen -- the top one lives in the overlay panel -- so half the
    #     window is an empty slot for a screen that is never rendered into it.
    #
    #     Measured on the real window before this change: client 908x790 with
    #     the game occupying 908x390. 790 is 2x390. That is the whole of the
    #     black bars in gameplay, with the gameplay view not filling the
    #     window, and no amount of work in the compositor could have reached
    #     it.
    #
    #     One screen high makes the opening window 896x384 for a 448-wide
    #     surface -- exactly the surface's own aspect, so it fills completely on
    #     the first frame, and the cover fit above keeps it filled through any
    #     resize.
    replace(front,
            "    const int first_height = presentation.separate\n"
            "        ? kScreenHeight * kWindowScale\n"
            "        : kScreenHeight * 2 * kWindowScale;",
            "    const int first_height =\n"
            "        (presentation.separate ||\n"
            "         lb_setting_enhanced())   /* legobattles-decomp */\n"
            "            ? kScreenHeight * kWindowScale\n"
            "            : kScreenHeight * 2 * kWindowScale;")
    done.append("frontend.cpp: Enhanced opens one screen high, not two")

    # 0o. A BUILD BANNER, and verbose state on demand.
    #
    #     Logs from different builds are indistinguishable once they are on
    #     disk, and this project keeps several runs side by side in
    #     screenshots/exp-*/stderr.log. Without a stamp there is no way to tell
    #     whether a log predates a fix -- which matters, because at least one
    #     conclusion here was drawn from a run made before the change it was
    #     supposed to be testing.
    #
    #     __DATE__/__TIME__ are the build's own, so the stamp identifies the
    #     BINARY rather than the run, which is the thing that is ambiguous.
    replace(front,
            '    std::fprintf(stderr,\n'
            '        "[sdl] layout=%s adaptive=%s supersampling=%ux aa=%ux "',
            '    std::fprintf(stderr,\n'
            '        "[lb] LEGO Battles port | built %s %s | adaptive_width=%u"\n'
            '        " | enhanced=%d\\n",\n'
            '        __DATE__, __TIME__,\n'
            '        (unsigned)options.adaptive_max_width[1],\n'
            '        lb_setting_enhanced() ? 1 : 0);\n'
            '    std::fprintf(stderr,\n'
            '        "[sdl] layout=%s adaptive=%s supersampling=%ux aa=%ux "')
    done.append("frontend.cpp: build banner with date and settings")

    # 0p. LB_VRAMLOG: a histogram of which VRAM blocks the guest writes.
    #
    #     The widening work is stuck on one question that no amount of reading
    #     BGxCNT has settled: WHERE does the guest put its tilemap? Deriving it
    #     as `screen_base * 0x800` produced a relocation that renders a dither
    #     of wrong tiles, and the same registers claim BG2's char data starts at
    #     0x0000 -- exactly where its screen block was placed -- so at least one
    #     derivation is wrong (docs/199).
    #
    #     This does not derive anything. It counts writes per 2 KB block and
    #     prints the busiest, which is the guest telling us directly. Off unless
    #     the variable is set.
    vram = os.path.join(runner_dir, "src", "vram.cpp")
    # vram.cpp includes neither <cstdio> nor <cstdlib>.
    replace(vram, '#include "vram.h"',
            '#include "vram.h"\n#include <cstdio>\n#include <cstdlib>')
    replace(vram,
            "void nds_video_write(int cpu, uint32_t addr, uint32_t value, uint32_t width) {\n"
            "    addr &= ~(width - 1u);",
            "void nds_video_write(int cpu, uint32_t addr, uint32_t value, uint32_t width) {\n"
            "    addr &= ~(width - 1u);\n"
            "    {   /* legobattles-decomp: LB_VRAMLOG=1 block histogram */\n"
            "        static int lb_on = -1;\n"
            "        if (lb_on < 0) lb_on = std::getenv(\"LB_VRAMLOG\") ? 1 : 0;\n"
            "        if (lb_on && (addr & 0xFF000000u) == 0x06000000u) {\n"
            "            static uint32_t lb_hits[64];\n"
            "            static uint64_t lb_total;\n"
            "            const uint32_t lb_blk = ((addr - 0x06000000u) >> 11) & 63u;\n"
            "            ++lb_hits[lb_blk];\n"
            "            if ((++lb_total % 50000u) == 0u) {\n"
            "                std::fprintf(stderr, \"[lb_vramlog] writes per 2KB block:\");\n"
            "                for (int lb_i = 0; lb_i < 64; ++lb_i)\n"
            "                    if (lb_hits[lb_i])\n"
            "                        std::fprintf(stderr, \" %04X=%u\",\n"
            "                                     (unsigned)(lb_i * 0x800),\n"
            "                                     (unsigned)lb_hits[lb_i]);\n"
            "                std::fprintf(stderr, \"\\n\");\n"
            "                std::fflush(stderr);\n"
            "            }\n"
            "        }\n"
            "    }")
    done.append("vram.cpp: LB_VRAMLOG block-write histogram")

    #     LB_VERBOSE=1 reports the presented geometry once per second; it is
    #     inserted below, inside the hook that CREATES the lb_only rect, because
    #     that text does not exist yet at this point in the run.

    gpu3d = os.path.join(runner_dir, "src", "gpu3d.cpp")
    replace(gpu3d, "    if (width < 256u || width > 448u || (width & 1u)) return false;",
                   "    if (width < 256u || width > 1728u || (width & 1u)) return false;"
                   "   /* legobattles-decomp */")
    done.append("gpu3d.cpp: 3D output width ceiling 448 -> 1728")
    replace(os.path.join(runner_dir, "src", "frontend_config.cpp"),
            "            if (*value < 256 || *value > 448 || (*value & 1) != 0) {",
            "            if (*value < 256 || *value > 1728 || (*value & 1) != 0) {"
            "   /* legobattles-decomp */")
    done.append("frontend_config.cpp: adaptive_width ceiling 448 -> 1728")



    done.append("gpu2d.cpp: widen the sub engine instead of letterboxing")

    # 4b. The same widener for an engine-A scene that is not 3D.
    replace(gpu,
            "    if (!supported_scene) {\n"
            "        for (int y = 0; y < 192; ++y)\n"
            "            std::copy_n(native + y * 256, 256,\n"
            "                        adaptive.data() + y * output_width + extra);\n"
            "        return adaptive.data();\n"
            "    }",
            "    if (!supported_scene) {\n"
            "        // legobattles-decomp: a scene this path calls unsupported may\n"
            "        // still be a plain 2D one it simply was not written for. LEGO\n"
            "        // Battles draws its FIELD with engine A on the BOTTOM screen,\n"
            "        // so screen 1 arrives here as engine 0 and fails bg0_3d.\n"
            "        if (lb_wide2d_supported(0, u, palette, oam, vram)) {\n"
            "            for (int y = 0; y < 192; ++y)\n"
            "                lb_wide2d_line(0, y, u, palette, oam, *vram,\n"
            "                               adaptive.data() +\n"
            "                                   (y + extra_y) * output_width,\n"
            "                               output_width, extra);\n"
            "            lb_fill_rows_outward(adaptive.data(), output_width,\n"
            "                                 output_height, extra_y);\n"
            "            return adaptive.data();\n"
            "        }\n"
            "        // legobattles-decomp: menus cannot be widened truthfully;\n"
            "        // fill rather than letterbox. See lb_menu_fill.\n"
            "        lb_menu_fill(native, adaptive.data(), output_width,\n"
            "                     output_height, extra, extra_y);\n"
            "        return adaptive.data();\n"
            "    }"
            )
    done.append("gpu2d.cpp: widen a non-3D engine-A scene")



    # 4d. Report why the engine-A widener stood down, on change.
    edit(gpu, "        !g_present_capture_active;",
         ['{   /* legobattles-decomp: say why the engine-A widener stood down */',
          '    const char* lb_why = nullptr;',
          '    if (!palette || !oam || !vram) lb_why = "no palette/oam/vram";',
          '    else if (u.dispcnt & 0x80u) lb_why = "forced blank";',
          '    else if (mode != 1u) lb_why = "display mode";',
          '    else if (!bg0_3d) lb_why = "BG0 is not the 3D layer";',
          '    else if (!supported_hud_bgs) lb_why = "a non-text HUD BG";',
          '    else if (g_present_capture_active) lb_why = "present capture";',
          '    static const char* lb_last = reinterpret_cast<const char*>(1);',
          '    const char* lb_now = lb_why ? lb_why : "WIDENING";',
          '    if (lb_last != lb_now) {',
          '        lb_last = lb_now;',
          '        std::fprintf(stderr,',
          '                     "[lb_wideA] screen %d: %s (dispcnt %08X, w=%d)\\n",',
          '                     screen, lb_now, (unsigned)u.dispcnt, output_width);',
          '    }',
          '}'])
    done.append("gpu2d.cpp: engine-A widener diagnostics")

    # 4c. Windowed scenes may widen; the field is a 3D scene.
    replace(gpu,
            "    const bool windows_enabled = (u.dispcnt & 0xE000u) != 0u;\n"
            "    const bool supported_scene =\n"
            "        palette && oam && vram && !(u.dispcnt & 0x80u) &&\n"
            "        mode == 1u && bg0_3d && supported_hud_bgs && !windows_enabled &&\n"
            "        !g_present_capture_active;",
            "    const bool windows_enabled = (u.dispcnt & 0xE000u) != 0u;\n"
            "    // legobattles-decomp: windows do not gate widening. This renderer\n"
            "    // implements no window logic at all, so the native path ignores WIN0\n"
            "    // too. The field's BG scroll registers never move (measured: BG1-3\n"
            "    // pinned at 508 through every input), so the terrain is 3D geometry\n"
            "    // and widening it is the 3D frustum's job, not the tilemap's.\n"
            "    (void)windows_enabled;\n"
            "    const bool supported_scene =\n"
            "        palette && oam && vram && !(u.dispcnt & 0x80u) &&\n"
            "        mode == 1u && bg0_3d && supported_hud_bgs &&\n"
            "        !g_present_capture_active;"
            )
    done.append("gpu2d.cpp: windowed scenes may widen")

    # 5a. An env-configurable watchpoint range, to find VRAM writers.
    edit(bus, "bool watch_addr(uint32_t addr, uint32_t width) {",
         ['// legobattles-decomp: an env-configurable watch range.',
          '//',
          '// The ranges below are firmware-debugging addresses. Answering "which code',
          '// writes these bytes?" for anything else meant editing and rebuilding. The',
          '// tilemap the field is drawn from is reached through a computed VRAM base,',
          '// so it cannot be found by scanning for literals -- but a watchpoint on the',
          '// bytes themselves names the writer directly.',
          '//',
          '//     LB_WATCH=6000000:6001000   (hex lo:hi, ARM9 addresses)',
          'bool watch_range(uint32_t addr, uint32_t width, uint32_t lo, uint32_t hi);',
          'bool lb_watch_pc_ok();',
          'bool lb_watch_rw_ok(unsigned write);',
          'bool lb_watch_val_ok(uint32_t value);',
          'bool lb_watch_notpc_ok();',
          '// SETTABLE AT RUNTIME, not only from the environment.',
          '//',
          '// LB_WATCH is read once at startup, which is fine for an address a',
          '// literal names. It is useless for one that does not exist yet: the',
          '// entity to be watched is reached through a pool pointer and an',
          '// array, so its address is only known once a battle is running, and',
          '// by then the variable has long been parsed. The alternative was to',
          '// run the whole route once to learn the address and again to watch',
          '// it, which is twenty minutes to ask one question.',
          'uint32_t g_lb_watch_lo = 0, g_lb_watch_hi = 0;',
          '// Set once a runtime range arrives, so watch_addr knows to consult it',
          '// instead of falling through to its always-on defaults.',
          'bool g_lb_watch_runtime = false;',
          'extern "C" void lb_watch_set_range(uint32_t lo, uint32_t hi) {',
          '    g_lb_watch_lo = lo;',
          '    g_lb_watch_hi = hi;',
          '    g_lb_watch_runtime = (lo < hi);',
          '    std::fprintf(stderr, "[lb_watch] range set to %08X:%08X\\n",',
          '                 lo, hi);',
          '    std::fflush(stderr);',
          '}',
          'bool lb_watch_extra(uint32_t addr, uint32_t width) {',
          '    static bool init = false;',
          '    if (!init) {',
          '        init = true;',
          '        if (const char* s = std::getenv("LB_WATCH")) {',
          '            unsigned a = 0, b = 0;',
          '            if (std::sscanf(s, "%x:%x", &a, &b) == 2) {',
          '                g_lb_watch_lo = a;',
          '                g_lb_watch_hi = b;',
          '            }',
          '        }',
          '    }',
          '    return g_lb_watch_lo < g_lb_watch_hi &&',
          '           watch_range(addr, width, g_lb_watch_lo, g_lb_watch_hi) &&',
          '           lb_watch_pc_ok();',
          '}',
          '',
          '// Optional companion filter: keep only accesses made BY a given',
          '// instruction.',
          '//',
          '//     LB_WATCH_PC=2039700,2039780   (hex ARM9 PCs, up to eight)',
          '//',
          '// A range alone answers "who touches this address?". This answers the',
          '// mirror question, "what address does THIS instruction touch?", which is',
          '// how a structure reached only through a register gets located: the field',
          '// camera is loaded by ldrsh from a `this` pointer that arrives via a',
          '// vtable, so no literal names it and no static scan can find it, but the',
          '// load itself reports its own effective address.',
          '//',
          '// Without this the ring keeps the most recent 512 accesses over whatever',
          '// range is watched, and one interesting load per frame is drowned by the',
          '// hundreds of thousands around it.',
          'bool lb_watch_pc_ok() {',
          '    static bool init = false;',
          '    static uint32_t pcs[8];',
          '    static int n = 0;',
          '    if (!init) {',
          '        init = true;',
          '        if (const char* s = std::getenv("LB_WATCH_PC")) {',
          '            while (*s && n < 8) {',
          '                char* end = nullptr;',
          '                const unsigned long v = std::strtoul(s, &end, 16);',
          '                if (end == s) break;',
          '                pcs[n++] = static_cast<uint32_t>(v);',
          '                s = (*end == ' + chr(39) + ',' + chr(39) + ') ? end + 1 : end;',
          '                while (*s == ' + chr(39) + ' ' + chr(39) + ') ++s;',
          '            }',
          '        }',
          '    }',
          '    if (n == 0) return true;   // unset: keep the range filter alone',
          '    for (int i = 0; i < n; ++i)',
          '        if (g_cpu.R[15] == pcs[i]) return true;',
          '    return false;',
          '}',
          '',
          '// Keep only reads, or only writes.',
          '//',
          '//     LB_WATCH_RW=w    writes only',
          '//     LB_WATCH_RW=r    reads only',
          '//',
          '// The ring holds 512 events. A range that is READ every frame and',
          '// written once an hour will report the read traffic and nothing else,',
          '// and the writes -- the interesting half -- are evicted before they can',
          '// be looked at. That failure is silent: the result looks like "no writes',
          '// happen here", which is a conclusion, not an absence of data.',
          '//',
          '// docs/117 retracted two documents built on exactly that mistake. The',
          '// tell is that the event count equals the cap; the fix is to not record',
          '// the uninteresting half in the first place.',
          'bool lb_watch_rw_ok(unsigned write) {',
          '    static bool init = false;',
          '    static int want = -1;   // -1 both, 0 reads, 1 writes',
          '    if (!init) {',
          '        init = true;',
          '        if (const char* s = std::getenv("LB_WATCH_RW")) {',
          '            if (*s == 0x77 || *s == 0x57) want = 1;   // w, W',
          '            else if (*s == 0x72 || *s == 0x52) want = 0;   // r, R',
          '        }',
          '    }',
          '    return want < 0 || want == (int)(write ? 1 : 0);',
          '}',
          '',
          '// Keep only accesses carrying a particular VALUE.',
          '//',
          '//     LB_WATCH_VAL=1   only accesses whose value is 1',
          '//',
          '// The third filter, and it exists for a case the other two cannot',
          '// reach. The sound module keeps a byte per stream slot saying whether',
          '// that slot has been started, and CLEARS the whole array every frame',
          '// from a single instruction. A range watch on it fills the 512-entry',
          '// ring with zeroes inside one frame, so the one write that matters --',
          '// the 1 that says a track was started -- is evicted before it can be',
          '// read. The PC filter is the wrong shape here: it names the PCs to',
          '// KEEP, and the interesting PC is the unknown being looked for.',
          '// EMPTY THE RING, which nothing could do before.',
          '//',
          '// bus_debug_watch_copy COPIES the last N events and leaves the ring',
          '// exactly as it found it -- g_watch_count is never reset. So every',
          '// "drain the backlog before the click" call in every probe this',
          '// project has written did nothing, and each read returned the same',
          '// accumulating window. It shows up as impossible results: a watch',
          '// filtered to one entity POINTER value came back full of writes to',
          '// the keypad registers, because those were stale entries recorded',
          '// before the filter was set.',
          '//',
          '// Nothing else needs to change: the reader stays non-destructive,',
          '// and a caller that wants a clean window asks for one.',
          'extern "C" void lb_watch_clear(void) {',
          '    g_watch_w = 0;',
          '    g_watch_count = 0;',
          '}',
          '// SETTABLE AT RUNTIME as well, for the same reason the range is.',
          '//',
          '// The value worth filtering on is often one that only exists once a',
          '// battle is running. The case this was added for: when a unit is',
          '// selected, its ENTITY POINTER must be written into the selection',
          '// list, so watching all of RAM for a write of that exact pointer',
          '// finds the selection object -- which is what docs/226 named as the',
          '// next thing to look at. An env var parsed at startup cannot carry a',
          '// heap address the harness only learns mid-match.',
          'long long g_lb_watch_val = -1;',
          'extern "C" void lb_watch_set_val(long long v) {',
          '    g_lb_watch_val = v;',
          '    std::fprintf(stderr, "[lb_watch] value filter set to %lld\\n", v);',
          '    std::fflush(stderr);',
          '}',
          'bool lb_watch_val_ok(uint32_t value) {',
          '    static bool init = false;',
          '    if (!init) {',
          '        init = true;',
          '        if (const char* s = std::getenv("LB_WATCH_VAL"))',
          '            g_lb_watch_val = (long long)std::strtoul(s, nullptr, 0);',
          '    }',
          '    return g_lb_watch_val < 0 ||',
          '           (uint32_t)g_lb_watch_val == value;',
          '}',
          '',
          '// Drop accesses made BY given instructions -- the complement of',
          '// LB_WATCH_PC.',
          '//',
          '//     LB_WATCH_NOTPC=208985c,2087378,2087384',
          '//',
          '// LB_WATCH_PC answers "what does THIS instruction touch?", which needs',
          '// the instruction to be known already. The question that comes up more',
          '// often is the opposite one -- "who ELSE touches this?" -- and it has a',
          '// specific failure mode: a region with two or three per-frame writers',
          '// refills the 512-entry ring inside one frame, so a one-off write from',
          '// the unknown PC being looked for is evicted long before the ring can',
          '// be drained. Measured on the sound globals: three PCs accounted for',
          '// all 512 events and the interesting transition was minutes of',
          '// emulated time in the past. Naming the known noise leaves the ring',
          '// for everything else.',
          'bool lb_watch_notpc_ok() {',
          '    static bool init = false;',
          '    static uint32_t pcs[8];',
          '    static int n = 0;',
          '    if (!init) {',
          '        init = true;',
          '        if (const char* s = std::getenv("LB_WATCH_NOTPC")) {',
          '            while (*s && n < 8) {',
          '                char* end = nullptr;',
          '                const unsigned long v = std::strtoul(s, &end, 16);',
          '                if (end == s) break;',
          '                pcs[n++] = static_cast<uint32_t>(v);',
          '                s = (*end == ' + chr(39) + ',' + chr(39) + ') ? end + 1 : end;',
          '            }',
          '        }',
          '    }',
          '    for (int i = 0; i < n; ++i)',
          '        if ((g_cpu.R[15] & ~1u) == (pcs[i] & ~1u)) return false;',
          '    return true;',
          '}',
          ''],
         after=False)
    edit(bus, "bool watch_addr(uint32_t addr, uint32_t width) {",
         ['    {  /* legobattles-decomp: LB_WATCH replaces the defaults, '
          'which are noisy enough to flush the ring */',
          '        // ALSO when the range was set at RUNTIME, not only from the',
          '        // environment. This read the variable once and cached the',
          '        // answer in a `static const`, so a range set later by the',
          '        // watch_range command was stored and then never consulted:',
          '        // the predicate fell through to the default always-on ranges',
          '        // and the ring filled with firmware IRQ traffic instead. The',
          '        // command reported success, the range was printed, and the',
          '        // events came from somewhere else entirely.',
          '        static const bool lb_env = std::getenv("LB_WATCH") != nullptr;',
          '        if (lb_env || g_lb_watch_runtime)',
          '            return lb_watch_extra(addr, width);',
          '    }'])
    edit(bus, "    if (!watch_addr(addr, width)) return;",
         ['    /* legobattles-decomp: LB_WATCH_RW / LB_WATCH_VAL filters */',
          '    if (!lb_watch_rw_ok(write)) return;',
          '    if (!lb_watch_val_ok(value)) return;',
          '    if (!lb_watch_notpc_ok()) return;'])
    # WHO CALLED THE CODE THAT WROTE IT.
    #
    # A watch event names the PC that performed the access, and that is
    # repeatedly the wrong granularity for this project's questions. The
    # selection count at 0215802C is written from 020EC8F0, inside a generic
    # vector push with THIRTY-ONE static callers -- so "who writes the
    # selection" has a definite answer that the watch cannot give and that
    # static cross-references cannot narrow.
    #
    # The link register is sitting right there in the same register file the
    # PC is read from, and on ARM a BL leaves the caller's return address in
    # it. Recording it costs one word per event and turns "written at
    # 020EC8F0" into "written at 020EC8F0, called from <the function that
    # decided>". runtime_trace already captures LR for its own entries, on the
    # same reasoning; the watch ring simply never did.
    state_h = os.path.join(runner_dir, "src", "state.h")
    # A BIGGER WATCH RING, because 512 is small enough to invent findings.
    #
    # A watch filtered to ONE pc in a loop that runs 640 times per drain returns
    # the last 512 events, and two drains then look like two disjoint sets of
    # addresses rather than two windows of the same walk. That is precisely how
    # the handler-list comparison in docs/233 nearly concluded "the two cases
    # walk completely different handler lists" from an artefact of where each
    # window happened to start.
    #
    # 16384 events is 16384 * 40 bytes, about 640 KB -- irrelevant next to the
    # generated banks -- and covers every loop this project has needed to watch.
    # The debug server still caps a single `watch` reply at 512, so a caller
    # that wants more asks repeatedly; what changes is that the ring no longer
    # DISCARDS them before anyone asks.
    replace(bus,
            "constexpr uint32_t kWatchSize = 512;",
            "constexpr uint32_t kWatchSize = 16384;   /* legobattles-decomp:"
            " was 512, small enough to invent findings -- see docs/233 */")
    replace(state_h,
            "struct BusWatchEvent {\n"
            "    uint64_t seq;",
            "struct BusWatchEvent {\n"
            "    /* legobattles-decomp: the caller, not just the accessor */\n"
            "    uint32_t lr;\n"
            "    uint64_t seq;")
    replace(bus,
            "    e.pc = g_cpu.R[15];\n"
            "    e.addr = addr;",
            "    e.pc = g_cpu.R[15];\n"
            "    e.lr = g_cpu.R[14];   /* legobattles-decomp */\n"
            "    e.addr = addr;")
    # The reply cap has to move with the ring or the extra events are simply
    # never asked for. 8192 keeps a single reply under a few megabytes of JSON,
    # which the tools already stream, while covering the loops this project
    # watches.
    replace(os.path.join(runner_dir, "src", "debug_server.cpp"),
            "        if (max > 512) max = 512;\n"
            "        std::vector<BusWatchEvent> ev(max);",
            "        if (max > 8192) max = 8192;   /* legobattles-decomp */\n"
            "        std::vector<BusWatchEvent> ev(max);")
    replace(os.path.join(runner_dir, "src", "debug_server.cpp"),
            "                \"\\\"pc\\\":%u,\\\"addr\\\":%u,\\\"value\\\":%u}\",",
            "                \"\\\"pc\\\":%u,\\\"addr\\\":%u,\\\"value\\\":%u,\"\n"
            "                \"\\\"lr\\\":%u}\",")
    replace(os.path.join(runner_dir, "src", "debug_server.cpp"),
            "                ev[i].pc, ev[i].addr, ev[i].value);",
            "                ev[i].pc, ev[i].addr, ev[i].value, ev[i].lr);")
    # 11. RASTER BIG ENOUGH TO STOP THE GAME ZOOMING IN.
    #
    #     User: "consistent scaling across window size (the game not 'zooming
    #     in' / preserving aspect when the window size is larger) must be
    #     solved". A bigger window currently shows the SAME field scaled up,
    #     because the raster is capped at 448x192 and then stretched to fit.
    #     More field of view needs more raster; there is no zoom lever, since
    #     the tile engine draws 1:1 (apply_view_width sets how many TILES the
    #     game draws, not a scale).
    #
    #     Two ceilings, both in the vendored melonDS and both already
    #     constexpr with the buffers sized off them:
    #
    #         MaxRenderWidth = 448      ScanlineWidth = MaxRenderWidth + 2
    #         NumScanlines   = 194      BufferSize    = ScanlineWidth * NumScanlines
    #
    #     Raising them costs memory and nothing else: the renderer still draws
    #     192 lines until something asks for more, so this step alone changes
    #     no pixels. 962 x 546 x 4 bytes x 3 buffers x 2 is about 12 MB, which
    #     THE VERTEX FORMAT IS THE REAL CEILING, and it is 512 x 256:
#
#         vtx->FinalPosition[0] = posX & 0x1FF;   // X: 0..511
#         vtx->FinalPosition[1] = posY & 0xFF;    // Y: 0..255
#
#     Those masks emulate the DS's own 9-bit X and 8-bit Y screen
#     coordinates. An X past 511 WRAPS and puts geometry somewhere
#     else entirely, so 1024 -- which this hook briefly used -- was
#     unsound; nothing had requested it yet only because the width is
#     chosen as 192 * aspect. 512 x 256 needs no mask change at all,
#     and gives 455 x 256 on a 16:9 window: 1.78x the native width,
#     1.33x the height, 1.7x the visible area.
    soft_h = os.path.join(runner_dir, "vendor", "melonds", "GPU3D_Soft.h")
    replace(soft_h,
            "    static constexpr int MaxRenderWidth = 448;",
            "    static constexpr int MaxRenderWidth = 1728;"
            "   /* legobattles-decomp: was 448, then 512 */")
    replace(soft_h,
            "    static constexpr int NumScanlines = 194;",
            "    static constexpr int MaxRenderHeight = 768;"
            "  /* legobattles-decomp: was 256 */\n"
            "    static constexpr int NumScanlines = MaxRenderHeight + 2;")
    done.append("melonds: raise the raster ceiling to 1728x768")


    #     ...and every clamp that would still refuse the wider raster. These
    #     are separate from the buffer ceiling above: the buffers can hold it,
    #     these decide whether anything is allowed to ask.
    gpu3d_cpp = os.path.join(runner_dir, "vendor", "melonds", "GPU3D.cpp")
    replace(gpu3d_cpp,
            "    if (width > 448) width = 448;",
            "    if (width > 1728) width = 1728;"
            "   /* legobattles-decomp: was 448, then 512 */")
    # COUNT THE GEOMETRY THE HARDWARE THROWS AWAY.
    #
    # The DS keeps 2048 polygons and 6144 vertices per frame and silently drops
    # the rest, recording it only in DISP3DCNT bit 13 -- and a search of the
    # whole ARM9 finds ZERO references to RAM_COUNT (0x04000604), so the game
    # never looks. Widening the view makes the port submit far more geometry
    # than the cartridge ever did, and entities are appended in gather order,
    # row-major from the rect's top-left, so whatever overflows is biased to the
    # bottom-right. Units becoming cropped or invisible as the camera pans
    # is exactly that shape.
    #
    # Nobody has ever measured whether this fires in play. This counts it.
    # AND HOW CLOSE IT GETS, so a zero is interpretable. "The cap never fired"
    # means nothing on its own -- it reads the same whether the scene uses 200
    # polygons or 2040. The peak per frame says which.
    replace(gpu3d_cpp,
            "void GPU3D::VBlank() noexcept\n"
            "{",
            "/* legobattles-decomp: the submitted polygon count, so it can be\n"
            "   read beside the rendered one. File scope -- a linkage\n"
            "   specification is illegal inside a function. */\n"
            "static unsigned lb_gx_submitted_v = 0;\n"
            "extern \"C\" unsigned lb_gx_submitted(void) {\n"
            "    return lb_gx_submitted_v;\n"
            "}\n"
            "static void lb_gx_submitted_set(unsigned n) {\n"
            "    lb_gx_submitted_v = n;\n"
            "}\n"
            "\n"
            "void GPU3D::VBlank() noexcept\n"
            "{\n"
            "    /* legobattles-decomp: peak geometry per frame, so the polygon\n"
            "       cap's headroom is a number rather than an assumption. */\n"
            "    {\n"
            "        /* WHAT THE GEOMETRY ENGINE SUBMITTED, published beside\n"
            "           what the renderer kept. docs/337 measured that 64-75%\n"
            "           of frames in which the camera MOVES draw zero polygons,\n"
            "           against 13-17% when it is still -- but\n"
            "           nds_gpu3d_render_polygon_count() is the post-clip list,\n"
            "           so on its own it cannot say whether the geometry was\n"
            "           CLIPPED (this port's shift pushing it out of the\n"
            "           frustum) or NEVER SUBMITTED (the guest's own gather\n"
            "           rect excluding it). Those are different bugs in\n"
            "           different code owned by different sides, and guessing\n"
            "           between them is how docs/268 was nearly mis-fixed.\n"
            "           NumPolygons here is the submitted count. */\n"
            "        lb_gx_submitted_set(NumPolygons);\n"
            "        static u32 lb_peak_p = 0, lb_peak_v = 0;\n"
            "        static unsigned lb_n = 0;\n"
            "        if (NumPolygons > lb_peak_p) lb_peak_p = NumPolygons;\n"
            "        if (NumVertices > lb_peak_v) lb_peak_v = NumVertices;\n"
            "        if ((++lb_n % 600u) == 0u)\n"
            "            std::fprintf(stderr,\n"
            "                \"[lb_polypeak] peak %u/2048 polygons, \"\n"
            "                \"%u/6144 vertices\\n\", lb_peak_p, lb_peak_v);\n"
            "    }")
    replace(gpu3d_cpp,
            "        LastStripPolygon = NULL;\n"
            "        DispCnt |= (1<<13);\n"
            "        return;",
            "        LastStripPolygon = NULL;\n"
            "        DispCnt |= (1<<13);\n"
            "        /* legobattles-decomp: see the note in apply_hooks.py */\n"
            "        {\n"
            "            static unsigned lb_drop = 0;\n"
            "            if ((++lb_drop & (lb_drop - 1)) == 0u)\n"
            "                std::fprintf(stderr,\n"
            "                    \"[lb_polydrop] hardware dropped a polygon: \"\n"
            "                    \"%u so far (polys %u, verts %u)\\n\",\n"
            "                    lb_drop, NumPolygons, NumVertices);\n"
            "        }\n"
            "        return;")
    gpu3d_h = os.path.join(runner_dir, "vendor", "melonds", "GPU3D.h")
    replace(gpu3d_h,
            "    u32 ScrolledLine[448];",
            "    u32 ScrolledLine[1728];")
    replace(gpu3d_h,
            "    u32 ScrolledAttrLine[448];",
            "    u32 ScrolledAttrLine[1728];")
    # NOTE the runner-side clamps are ALREADY 1024 - this port raised them in
    # an earlier hook. melonDS was the one silently clamping to 448, which is
    # why game.toml called 448 "the runner's own maximum". It was not.
    #     AND THE VERTEX FORMAT, which is what actually capped this at 512x256.
    #
    #     The earlier note in this file called the format "the real ceiling" and
    #     stopped there, because raising the raster alone would have been
    #     unsound: FinalPosition is masked to the DS's own 9-bit X and 8-bit Y,
    #     so an X past 511 WRAPS and puts geometry somewhere else entirely.
    #
    #     That reasoning was right about the mask and wrong about the
    #     conclusion. The mask only bites once the VIEWPORT is wider than the
    #     hardware's, because at the DS's own 256x192 the projected X never
    #     reaches 512 -- so widening it changes nothing on the vanilla path and
    #     is exactly what a wider surface needs. melonDS itself already keeps a
    #     13-bit HiresPosition for its GL renderer on the same reasoning.
    #
    #     WHY THIS MATTERS: the game view must scale properly and extend with
    #     window size, not just stretch to fit, so a larger screen shows more
    #     of the world instead of the game simply being blown up. Measured
    #     before this change, a 1920x1080
    #     window hit 472x256 and stopped; every pixel past that was
    #     magnification. 1024x512 is four times the area, which covers a 4K
    #     window at the same 4x scale.
    #     ELEVEN BITS IN X, and the reason is the clamp rather than the width.
    #
    #     AND THE Y MASK WAS A THIRD HORIZONTAL-ONLY FIX.
    #
    #     X went 0x1FF -> 0x3FF -> 0x7FF as the surface grew; Y was raised once,
    #     to 0x1FF, and left there. 0x1FF is 511, and kMaxAdaptiveHeight is
    #     768 -- so on any surface taller than 512 rows a vertex below row 511
    #     WRAPS to the top of the picture. Measured: a 620x1100 window gives a
    #     296x526 surface, already past it.
    #
    #     This is the same asymmetry twice already found and fixed elsewhere in
    #     this port -- the entity gather rect had only its horizontal half
    #     widened, and so did the view dimensions. A third instance is enough
    #     to call it the pattern: when a widening is added, the vertical twin
    #     is the thing to go looking for.
    #
    #     0x3FF is 1023, which covers kMaxAdaptiveHeight with headroom, exactly
    #     as 0x7FF covers the 1728 ceiling on the other axis.
    #
    #     0x3FF was sized for a CENTRED band, where the largest shift a 1728
    #     surface can ask for is half the overhang: (1728-256)/2 = 736, and a
    #     native 0..255 lands at 736..992, just inside 1023. That is not a
    #     coincidence -- the 1728 ceiling was derived from this mask.
    #
    #     lb_band_left may now put the WHOLE overhang on one side at a map edge,
    #     so the shift reaches 1472 and a vertex lands near 1728. Left at ten
    #     bits it would wrap to 704: geometry teleporting across the picture,
    #     and only ever at a map edge, which is the hardest place to notice it.
    #
    #     Safe at eleven: FinalPosition is s32, and every consumer of X is slope
    #     setup or a comparison (GPU3D_Soft.cpp:632/659/697-708), none of which
    #     index an array by it -- the scanline buffer is bounded separately by
    #     MaxRenderWidth + 2. 0x7FF covers 1728 with room and stays inside the
    #     same word.
    #
    #     HiresPosition is deliberately NOT widened. It is 13 bits against a
    #     value shifted left by four, so it already wraps above x=511 and has
    #     done since the surface passed that -- which is the standing evidence
    #     that nothing on this port's path reads it: melonDS keeps it for the GL
    #     and compute renderers, and this port runs the software one.
    replace(gpu3d_cpp,
            "        vtx->FinalPosition[0] = posX & 0x1FF;\n"
            "        vtx->FinalPosition[1] = posY & 0xFF;",
            "        vtx->FinalPosition[0] = posX & 0x7FF;"
            "   /* legobattles-decomp: was 0x1FF, then 0x3FF */\n"
            "        vtx->FinalPosition[1] = posY & 0x3FF;"
            "  /* legobattles-decomp: was 0xFF, then 0x1FF */")
    done.append("melonds: widen the vertex screen-position mask to 2048x1024")
    done.append("raster: clamps raised from 448 to 1024")

    #     RENDER HEIGHT, mirroring RenderWidth exactly.
    #
    #     Default 192, so this step is INERT: nothing asks for more until the
    #     compositor and the layout do. Patch 0009 added the width half and is
    #     the model for all of it -- including the detail that matters most,
    #     that the viewport is SHIFTED rather than stretched, which is why units
    #     are not distorted in the widened columns today.
    #
    #     Clamped to 256 because FinalPosition[1] is masked with 0xFF. Going
    #     past that needs the mask widened and is not needed by any ordinary
    #     monitor: 16:9 wants 455 x 256.
    replace(gpu3d_h,
            "    void SetRenderWidth(u32 width) noexcept;",
            "    void SetRenderWidth(u32 width) noexcept;\n"
            "    void SetRenderHeight(u32 height) noexcept;\n"
            "    [[nodiscard]] u32 GetRenderHeight() const noexcept"
            " { return RenderHeight; }")
    replace(gpu3d_h,
            "    u32 RenderWidth = 256;",
            "    u32 RenderWidth = 256;\n"
            "    // Host-only enhanced output HEIGHT; 192 is the native path.\n"
            "    u32 RenderHeight = 192;")
    replace(gpu3d_h,
            "    virtual void SetRenderWidth(u32 width) {}",
            "    virtual void SetRenderWidth(u32 width) {}\n"
            "    virtual void SetRenderHeight(u32 height) {}")
    replace(gpu3d_cpp,
            "    CurrentRenderer->SetRenderWidth(RenderWidth);",
            "    CurrentRenderer->SetRenderWidth(RenderWidth);\n"
            "    CurrentRenderer->SetRenderHeight(RenderHeight);")
    replace(gpu3d_cpp,
            "void GPU3D::SetRenderWidth(u32 width) noexcept",
            "void GPU3D::SetRenderHeight(u32 height) noexcept\n"
            "{\n"
            "    if (height < 192) height = 192;\n"
            "    if (height > 768) height = 768;  /* legobattles-decomp: was\n"
            "        256, the old FinalPosition[1] & 0xFF ceiling; the mask is\n"
            "        0x1FF now, and 768 keeps (H+192)/2 = 480 inside it. */\n"
            "    if (height & 1) height--;\n"
            "    /* docs/348: the clip matrix carries a 192/RenderHeight Y\n"
            "       scale (UpdateClipMatrix) and is cached until the guest\n"
            "       touches a matrix, so a new height must mark it stale. */\n"
            "    if (height != RenderHeight) ClipMatrixDirty = true;\n"
            "    RenderHeight = height;\n"
            "    if (CurrentRenderer) CurrentRenderer->SetRenderHeight(height);\n"
            "}\n"
            "\n"
            "void GPU3D::SetRenderWidth(u32 width) noexcept")
    replace(soft_h,
            "    void SetRenderWidth(u32 width) override;",
            "    void SetRenderWidth(u32 width) override;\n"
            "    void SetRenderHeight(u32 height) override;")
    replace(soft_h,
            "    int RenderWidth = 256;",
            "    int RenderWidth = 256;\n"
            "    int RenderHeight = 192;")
    soft_cpp = os.path.join(runner_dir, "vendor", "melonds", "GPU3D_Soft.cpp")
    replace(soft_cpp,
            "void SoftRenderer::SetRenderWidth(u32 width)",
            "void SoftRenderer::SetRenderHeight(u32 height)\n"
            "{\n"
            "    RenderHeight = std::clamp<int>(static_cast<int>(height),\n"
            "                                   192, MaxRenderHeight);\n"
            "    FrameIdentical = false;\n"
            "}\n"
            "\n"
            "void SoftRenderer::SetRenderWidth(u32 width)")
    done.append("melonds: RenderHeight plumbing (inert, default 192)")

    #     The vertical half of the viewport transform.
    #
    #     SHIFT, never stretch. The X side has an `enhanced_full_viewport`
    #     special case that substitutes RenderWidth for the viewport width, but
    #     this game does not take it -- units measure 24..31 px in the polygon
    #     dump, native size -- so the branch that actually runs just moves the
    #     scene by half the overhang to centre it, and the extra columns then
    #     carry whatever survives clipping. Y is exactly symmetric, and doing
    #     only the shift means nothing can be distorted.
    #
    #     FinalPosition[1] stays within its 0xFF mask: at RenderHeight 256 the
    #     offset is 32, so a native 0..191 becomes 32..223.
    # LB_NOFULLVP=1 forces the horizontal substitution OFF, so sprite widths
    # can be compared with and without it. That is the one measurement that
    # says whether the horizontal path is a working model for the vertical one
    # or a second latent stretch nobody has checked: it is in force right now
    # at w=RenderWidth against a native 256 (measured, [lb_viewport] full=1),
    # and if it stretches, copying it vertically was never the fix.
    #     THE VERTICAL FRUSTUM, widened at the projection rather than the
    #     viewport. See docs/251 for how this was arrived at.
    #
    #     Measured: the game's projection is horizontally OVER-WIDE and
    #     vertically EXACT. Geometry reaches guest x -114, outside the native
    #     256 columns, which can only happen if the projection puts world there
    #     and the DS viewport was clipping it -- so widening viewport_width
    #     reveals frustum that already exists, at unchanged scale, and sprite
    #     width measures invariant (23/25/23 across surfaces 484/946/1266).
    #     Vertically there is no spare frustum, so widening viewport_height can
    #     only magnify: heights 42/65/87 for the same polygons, exactly
    #     RenderHeight/192. That is the visible vertical stretch.
    #
    #     Sprite HEIGHT is already 24 at every surface size, so vertical scale
    #     was never wrong. COVERAGE is. And coverage cannot be bought at the
    #     viewport, because the geometry engine clips to the frustum before the
    #     viewport transform ever runs.
    #
    #     So scale the CLIP MATRIX's Y output by 192/RenderHeight. Position[1]
    #     is built from ClipMatrix[1,5,9,13], so scaling those scales it. With
    #     viewport_height then RenderHeight, the pixel size is
    #
    #         (k*P1) * RenderHeight / 2w  where k = 192/RenderHeight
    #                = P1 * 192 / 2w      -- identical to native
    #
    #     while the NDC range now spans RenderHeight/192 times more world. Same
    #     pixels-per-world, more of it: what the horizontal axis gets for free.
    #
    #     Applied where the clip matrix is BUILT, so it is in force for clipping
    #     as well as for the transform.
    replace(gpu3d_cpp,
            "    memcpy(ClipMatrix, ProjMatrix, 16*4);\n"
            "    MatrixMult4x4(ClipMatrix, PosMatrix);",
            "    memcpy(ClipMatrix, ProjMatrix, 16*4);\n"
            "    MatrixMult4x4(ClipMatrix, PosMatrix);\n"
            "    /* legobattles-decomp: widen the VERTICAL frustum. */\n"
            "    {\n"
            "        static const bool lb_on = [] {\n"
            "            const char* v = getenv(\"LB_FULLVP_Y\");\n"
            "            return !(v && v[0] == '0');\n"
            "        }();\n"
            "        if (lb_on && RenderHeight > 192) {\n"
            "            const s64 k = ((s64)192 << 12) / (s64)RenderHeight;\n"
            "            const int lb_yi[4] = {1, 5, 9, 13};\n"
            "            for (int i = 0; i < 4; ++i)\n"
            "                ClipMatrix[lb_yi[i]] =\n"
            "                    (s32)(((s64)ClipMatrix[lb_yi[i]] * k) >> 12);\n"
            "        }\n"
            "    }")
    replace(gpu3d_cpp,
            "        const bool enhanced_full_viewport =\n"
            "            RenderWidth > 256 && Viewport[0] == 0 && Viewport[4] == 256;",
            "        static const bool lb_no_full_vp =\n"
            "            getenv(\"LB_NOFULLVP\") != nullptr;"
            "   /* legobattles-decomp */\n"
            "        const bool enhanced_full_viewport =\n"
            "            !lb_no_full_vp &&\n"
            "            RenderWidth > 256 && Viewport[0] == 0 && Viewport[4] == 256;")
    replace(gpu3d_cpp,
            "        const u32 viewport_x = enhanced_full_viewport\n"
            "            ? 0 : Viewport[0] + (RenderWidth - 256) / 2;",
            "        /* legobattles-decomp: the band, not half the overhang.\n"
            "           lb_band_for_geometry RECORDS what it hands back, so\n"
            "           GPU3D::VBlank can promote the value this geometry was\n"
            "           actually transformed with when the banks swap -- rather\n"
            "           than this code having to know whether the guest's vblank\n"
            "           handler has already run. See lb_widemap.cpp.\n"
            "\n"
            "           CALLED OUTSIDE THE TERNARY, and that is not style. Put\n"
            "           on the false arm it is skipped whenever\n"
            "           enhanced_full_viewport is true, and the recording it\n"
            "           exists for is skipped with it -- a hand-off that stops\n"
            "           happening silently, which is the exact failure this\n"
            "           chain was written to avoid. The value is still only\n"
            "           APPLIED on the false arm, as before. */\n"
            "        const int lb_band = lb_band_for_geometry(RenderWidth);\n"
            "        const u32 viewport_x = enhanced_full_viewport\n"
            "            ? 0 : Viewport[0] + lb_band;\n"
            "        /* WHERE THE GUEST'S OWN 256 COLUMNS MUST LAND.\n"
            "\n"
            "           THE HORIZONTAL TWIN OF lb_ybias BELOW, and the reason\n"
            "           the project owner sees units standing beside the ground\n"
            "           instead of on it. The vertical case was found, measured\n"
            "           and fixed -- 'units floating below the ground they stand\n"
            "           on' -- and the identical horizontal case was left.\n"
            "\n"
            "           With viewport_width set to RenderWidth, a viewport_x of\n"
            "           0 centres the guest's 256 columns on the SURFACE, at\n"
            "           (RenderWidth-256)/2. The 2D layers are composited at\n"
            "           lb_band, which is NOT that number whenever band_compute\n"
            "           clamps at a world edge to keep the extension on-map.\n"
            "           The two then disagree by exactly centre - band.\n"
            "\n"
            "           MEASURED, one line per frame from inside the compositor\n"
            "           (LB_DRIFTLOG), surface 1728 with the camera at map x\n"
            "           328: extra = band_left = 468, centre = 736, and the 3D\n"
            "           pixels landed at 609..652 against 619/643 predicted from\n"
            "           the CENTRE and 351/375 predicted from the BAND. 268 px\n"
            "           of disagreement, which at magnification 2 is the ~536\n"
            "           window px seen in a screenshot between the base and\n"
            "           the spawn circle that should contain it.\n"
            "\n"
            "           WHY IT SCALES WITH THE MONITOR. The clamp only engages\n"
            "           near a world edge. The map is 1536 px wide, so a 946\n"
            "           surface asks for 690 px of extension and is usually\n"
            "           unclamped (centre == band, no error), while a\n"
            "           3440x1440 window gives a ~1720 surface asking for 1464\n"
            "           px -- nearly the whole world -- so it is clamped almost\n"
            "           always. Hence far away at 3440 wide, only a bit at\n"
            "           1920. */\n"
            "        const int lb_xcentre = ((int)RenderWidth - 256) / 2;\n"
            "        /* ...AND IT IS NOW ZERO BY DEFAULT, because SubmitVertex\n"
            "           carries this shift in clip space, where the CLIPPER can\n"
            "           see it. Applied here it ran after clipping: it placed\n"
            "           whatever survived the frustum, and at a world edge the\n"
            "           frustum is the thing throwing the units away -- the\n"
            "           render polygon list measured EMPTY. Doing both would\n"
            "           move the picture twice. LB_FULLVP_X=0 puts the shift\n"
            "           back here, which is the A/B. */\n"
            "        static const bool lb_xshift_in_clip = [] {\n"
            "            const char* v = getenv(\"LB_FULLVP_X\");\n"
            "            return !(v && v[0] == '0');\n"
            "        }();\n"
            "        const s32 lb_xbias =\n"
            "            (enhanced_full_viewport && !lb_xshift_in_clip)\n"
            "            ? (s32)(lb_band - lb_xcentre) : 0;\n"
            "        /* legobattles-decomp: the vertical band, recorded the\n"
            "           same way and for the same reason as the horizontal\n"
            "           one above.\n"
            "\n"
            "           SUBSTITUTING RenderHeight FOR Viewport[5] HERE WAS\n"
            "           TRIED AND IT STRETCHES THE SPRITES. It looked like the\n"
            "           obvious mirror of enhanced_full_viewport above, which\n"
            "           swaps RenderWidth in for the viewport WIDTH. Measured\n"
            "           by polygon size, same route, one variable:\n"
            "\n"
            "             before   dominant poly height 24 (109 polys), 56\n"
            "             after    dominant poly height 42  (43 polys), 98\n"
            "\n"
            "           24 -> 42 and 56 -> 98 are both exactly RenderHeight/192\n"
            "           = 1.75. Scaling normalised Y over more rows magnifies\n"
            "           the picture; it does not show more of the world: it\n"
            "           only stretched the sprites vertically.\n"
            "\n"
            "           Widths were unchanged across the same pair, which is\n"
            "           the second finding: this game does NOT take the\n"
            "           enhanced_full_viewport branch, exactly as the note\n"
            "           above it says. So that branch is not what buys the\n"
            "           horizontal reach either, and the claim that geometry\n"
            "           extends further sideways than upward needs a better\n"
            "           measurement than comparing guest x against guest y --\n"
            "           guest coordinates are computed by subtracting the band\n"
            "           read at PRINT time from a position transformed with the\n"
            "           band read at TRANSFORM time, so a mismatch between the\n"
            "           two shows up as apparent negative guest x with no\n"
            "           geometry out there at all. See docs/251. */\n"
            "        /* legobattles-decomp: LB_FULLVP_Y=1 applies the vertical\n"
            "           twin of enhanced_full_viewport, for the A/B described\n"
            "           in docs/251. The 2D terrain is drawn 1:1 across the\n"
            "           WHOLE widened surface, so the 3D has to span the same\n"
            "           extent or units sit at a different scale from the\n"
            "           ground under them -- which is what the horizontal\n"
            "           substitution is for, and why turning it off leaves\n"
            "           units visibly compressed against unchanged terrain.\n"
            "           ON, paired with the clip-matrix Y scaling above: that\n"
            "           scaling is what makes this size-preserving instead of a\n"
            "           magnification. Measured at surface 484, polygons matched\n"
            "           by (sub, tex, pal): 55x56, 23x24, 24x23 against an\n"
            "           X-only baseline of 55x56, 23x24, 24x24 -- identical --\n"
            "           where scaling the viewport ALONE gave 55x98, 23x42,\n"
            "           24x42. LB_FULLVP_Y=0 turns the pair off. */\n"
            "        static const bool lb_full_vp_y = [] {\n"
            "            const char* v = getenv(\"LB_FULLVP_Y\");\n"
            "            return !(v && v[0] == '0');\n"
            "        }();\n"
            "        const bool enhanced_full_viewport_y =\n"
            "            lb_full_vp_y && RenderHeight > 192 &&\n"
            "            Viewport[3] == 0 && Viewport[5] == 192;\n"
            "        /* CALLED UNCONDITIONALLY, like its horizontal twin: it\n"
            "           RECORDS what it returns so GPU3D::VBlank can promote\n"
            "           the band this geometry was transformed with. Putting it\n"
            "           on one arm of the ternary stops the recording whenever\n"
            "           the other arm runs -- the exact silent hand-off failure\n"
            "           the note above the X side warns about. */\n"
            "        const int lb_band_y = lb_band_for_geometry_y(RenderHeight);\n"
            "        /* WHERE THE GUEST'S OWN 192 ROWS MUST LAND.\n"
            "\n"
            "           With the clip matrix scaled and viewport_height set to\n"
            "           RenderHeight, a viewport_y of 0 centres the guest's rows\n"
            "           on the SURFACE -- at (RenderHeight-192)/2. The 2D layers\n"
            "           are composited at lb_band_y, which is NOT that number\n"
            "           whenever the band is clamped at a map edge, and the two\n"
            "           then disagree: measured at the map's north edge with\n"
            "           band_y 0, every polygon sat +12/+84/+162/+252 px low at\n"
            "           surfaces 216/360/516/696 -- exactly (RenderHeight-192)/2\n"
            "           -- units floating below the ground they stand on.\n"
            "\n"
            "           So bias the origin by the difference. Clamped at 0\n"
            "           because viewport_y is unsigned and a negative origin\n"
            "           would wrap; geometry above the surface is off-screen\n"
            "           either way, and the clip matrix has already widened what\n"
            "           is reachable. */\n"
            "        const int lb_ycentre = ((int)RenderHeight - 192) / 2;\n"
            "        /* docs/348: ZERO WHEN SubmitVertex CARRIES THE SHIFT IN CLIP\n"
            "           SPACE (lb_clip_y_on, the default). Applied here it ran\n"
            "           AFTER the clipper, which had already cut the frustum\n"
            "           symmetrically about the camera: at the north edge the\n"
            "           bottom lb_ycentre rows of the surface got no 3D at all\n"
            "           and the top ones were squashed onto row 0; at the south\n"
            "           edge the top rows went empty and polygons pushed past\n"
            "           RenderHeight were dropped as Degenerate. The horizontal\n"
            "           axis was moved into clip space for exactly this reason\n"
            "           (docs/268); this is its twin. */\n"
            "        const s32 lb_ybias = (enhanced_full_viewport_y &&\n"
            "                              !lb_clip_y_on())\n"
            "            ? (s32)(lb_band_y - lb_ycentre) : 0;\n"
            "        const u32 viewport_height =\n"
            "            enhanced_full_viewport_y ? RenderHeight : Viewport[5];\n"
            "        const u32 viewport_y =\n"
            "            enhanced_full_viewport_y ? 0u\n"
            "                                     : Viewport[3] + lb_band_y;\n"
            "        /* legobattles-decomp: WHICH BRANCH IS ACTUALLY TAKEN.\n"
            "           Earlier analysis went in circles about whether\n"
            "           enhanced_full_viewport applies to this game, deducing\n"
            "           it from sprite widths and from negative guest x and\n"
            "           reaching opposite answers. The values are right here;\n"
            "           print them once and stop guessing. */\n"
            "        /* READ ONCE. This is the per-VERTEX viewport transform, and\n"
            "           an uncached getenv here locked and scanned the process\n"
            "           environment for every clipped vertex of every frame.\n"
            "           */\n"
            "        static const bool lb_vplog = getenv(\"LB_VIEWPORTLOG\") != nullptr;\n"
            "        if (lb_vplog) {\n"
            "            static int lb_said = 0;\n"
            "            if (!lb_said) {\n"
            "                lb_said = 1;\n"
            "                printf(\"[lb_viewport] full=%d  Viewport[0]=%u \"\n"
            "                       \"[3]=%u [4]=%u [5]=%u  ->  x=%u w=%u \"\n"
            "                       \"y=%u h=%u  RenderWidth=%u RenderHeight=%u\\n\",\n"
            "                       enhanced_full_viewport ? 1 : 0,\n"
            "                       Viewport[0], Viewport[3], Viewport[4],\n"
            "                       Viewport[5], viewport_x, viewport_width,\n"
            "                       viewport_y, viewport_height,\n"
            "                       RenderWidth, RenderHeight);\n"
            "                fflush(stdout);\n"
            "            }\n"
            "        }")
    replace(gpu3d_cpp,
            "            posX = ((posX * viewport_width) / den) + viewport_x;",
            "            {   /* legobattles-decomp: signed bias, clamped result,\n"
            "                   exactly as the Y axis below. The bias is\n"
            "                   negative whenever band_compute clamped the band\n"
            "                   below centre at a world edge, so it cannot be\n"
            "                   folded into the unsigned viewport_x. Without it\n"
            "                   the 3D units sit centre-minus-band pixels away\n"
            "                   from the 2D ground they stand on. */\n"
            "                s32 lb_px = (s32)((posX * viewport_width) / den)\n"
            "                          + (s32)viewport_x + lb_xbias;\n"
            "                if (lb_px < 0) lb_px = 0;\n"
            "                posX = (u32)lb_px;\n"
            "            }")
    replace(gpu3d_cpp,
            "            posY = ((posY * Viewport[5]) / den) + Viewport[3];",
            "            {   /* legobattles-decomp: signed bias, clamped result.\n"
            "                   The bias is negative whenever the band is\n"
            "                   clamped below centre, so it cannot be folded\n"
            "                   into the unsigned viewport_y. */\n"
            "                s32 lb_py = (s32)((posY * viewport_height) / den)\n"
            "                          + (s32)viewport_y + lb_ybias;\n"
            "                if (lb_py < 0) lb_py = 0;\n"
            "                posY = (u32)lb_py;\n"
            "            }")
    # THE HI-RES PATH NEEDS THE SAME BIAS, on both axes.
    #
    # These write HiresPosition, which only the accelerated renderer reads, and
    # at these widths it does not run -- "compute render width 512 is
    # unsupported", so the port falls back to the threaded software renderer
    # and the FinalPosition path above is what draws. That is also why the
    # vertical fix measured clean while this path kept the old arithmetic.
    #
    # Patched anyway so the two paths cannot disagree the day the compute
    # renderer does initialise: a band fix that silently applies to one
    # renderer and not the other is the same class of trap as the band that
    # applied to the 2D and not the 3D. Shifted <<4 because these are 4-bit
    # fixed point, and clamped because the results are masked unsigned.
    replace(gpu3d_cpp,
            "            posX = ((((s64)(vtx->Position[0] + w) * viewport_width) << 4) "
            "/ (((s64)w) << 1)) + (viewport_x << 4);",
            "            {   /* legobattles-decomp: band bias, hi-res path. */\n"
            "                s64 lb_hx = ((((s64)(vtx->Position[0] + w) * "
            "viewport_width) << 4) / (((s64)w) << 1))\n"
            "                          + ((s64)viewport_x << 4) "
            "+ ((s64)lb_xbias << 4);\n"
            "                if (lb_hx < 0) lb_hx = 0;\n"
            "                posX = (u32)lb_hx;\n"
            "            }")
    replace(gpu3d_cpp,
            "            posY = ((((s64)(-vtx->Position[1] + w) * Viewport[5]) << 4) "
            "/ (((s64)w) << 1)) + (Viewport[3] << 4);",
            "            {   /* legobattles-decomp: band bias, hi-res path. */\n"
            "                s64 lb_hy = ((((s64)(-vtx->Position[1] + w) * "
            "viewport_height) << 4) / (((s64)w) << 1))\n"
            "                          + ((s64)viewport_y << 4) "
            "+ ((s64)lb_ybias << 4);\n"
            "                if (lb_hy < 0) lb_hy = 0;\n"
            "                posY = (u32)lb_hy;\n"
            "            }")
    done.append("melonds: vertical viewport shift")

    #     ...and the polygon Y BOUNDS, which are not cosmetic and were the
    #     menu misalignment.
    #
    #     SubmitPolygon marks a polygon Degenerate -- dropped entirely, not
    #     clipped -- when its bottom vertex falls below row 192. The viewport
    #     shift directly above moves EVERY polygon down by
    #     (RenderHeight - 192) / 2, so at height 512 that is 160 rows, and any
    #     polygon whose native bottom is below row 32 is thrown away. That is
    #     why the Single Player menu's selection highlight went from ~3690
    #     detectable orange pixels to as few as 6: it is 3D geometry, and it
    #     was being discarded. The "position" the measurement was reading was a
    #     centroid over the handful of survivors, which is why it looked like a
    #     drift rather than a deletion.
    #
    #     The same function seeds its top-vertex min-scan with `ytop = 192`.
    #     Once every vertex sits at y >= 192 the `<` test never fires, so VTop,
    #     YTop and XTop keep their placeholder values and the rasteriser walks
    #     an edge that does not exist. RenderHeight is a safe seed: a shifted
    #     vertex tops out at (RenderHeight + 192) / 2, which is < RenderHeight
    #     for every height above 192.
    replace(gpu3d_cpp,
            "    s32 ytop = 192, ybot = 0;",
            "    s32 ytop = static_cast<s32>(RenderHeight), ybot = 0;")
    replace(gpu3d_cpp,
            "    if (ybot > 192) poly->Degenerate = true;",
            "    if (ybot > static_cast<s32>(RenderHeight))"
            " poly->Degenerate = true;")
    #     The savestate reload path re-derives the same flag and must agree.
    replace(gpu3d_cpp,
            "            if (poly->YBottom > 192) poly->Degenerate = true;",
            "            if (poly->YBottom > static_cast<s32>(RenderHeight))\n"
            "                poly->Degenerate = true;")

    #     THE POLYGON SORT KEY, which overflows at every height we use.
    #
    #     SortKey packs the sort rules into one u32 -- opaque before
    #     translucent, then lower bottom Y, then lower top Y -- as
    #
    #         bit 16     Translucent
    #         bits 15..8 ybot
    #         bits  7..0 ytop
    #
    #     Eight bits each, because on the DS those are screen rows and cannot
    #     exceed 191. Under the viewport shift they reach (RenderHeight+192)/2,
    #     so they pass 255 as soon as the surface is taller than 318 rows -- and
    #     336, 360, 426 and 512 are all of the heights this port actually uses.
    #     Past that point ytop carries into ybot's field, and ybot = 256 sets
    #     `ybot << 8` = 0x10000, which IS the Translucent bit: an opaque polygon
    #     sorts as a translucent one. This is not a hypothesis about the code,
    #     it is the arithmetic of the layout.
    #
    #     Ten bits each keeps the ordering identical and holds 0..1023, which
    #     covers every height the vertex masks allow.
    replace(gpu3d_cpp,
            "    poly->SortKey = (ybot << 8) | ytop;\n"
            "    if (poly->Translucent) poly->SortKey |= 0x10000;",
            "    /* legobattles-decomp: 10 bits each, not 8. See apply_hooks.py. */\n"
            "    poly->SortKey = (ybot << 10) | ytop;\n"
            "    if (poly->Translucent) poly->SortKey |= 0x100000;")

    #     LB_POLYDIAG makes the claim above a measurement rather than a story:
    #     it counts, per batch of submitted polygons, how many the OLD bound
    #     would have discarded and how many sit wholly below row 192. Off by
    #     default and behind a one-shot getenv.
    #
    #     The VALUE is the batch size, because the first version hard-coded
    #     20000 and a whole menu run submitted fewer than that -- so it printed
    #     nothing at all, which reads exactly like "the diagnostic was never
    #     compiled in". LB_POLYDIAG=1 means the 2000 default; any other number
    #     is used as-is.
    replace(gpu3d_cpp, "#include <stdio.h>\n#include <string.h>",
            "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
            "/* legobattles-decomp: the widened band. See src/lb_widemap.cpp. */\n"
            "extern \"C\" int lb_band_for_geometry(int render_width);\n"
            "extern \"C\" int lb_band_for_geometry_y(int render_height);\n"
            "/* SubmitVertex reads the band WITHOUT recording it. The latch\n"
            "   chain's g_band_used must be stamped by the viewport transform,\n"
            "   which is the stage the compositor pairs its snapshot with; a\n"
            "   second recorder would promote a value no frame was placed\n"
            "   with. */\n"
            "extern \"C\" int lb_band_left(int output_width);\n"
            "/* The geometry path's own shift: latched once per frame, no width\n"
            "   test, so it cannot change between two vertices of one polygon\n"
            "   nor fall back to a centred split. See lb_band_latch. */\n"
            "extern \"C\" int lb_band_geom_shift(int render_width);\n"
            "extern \"C\" void lb_band_promote(void);\n"
            "/* docs/342: scanline-stamped event trace (LB_LIVE3D_PROBE). */\n"
            "extern \"C\" void lb_evt(int kind);\n"
            "/* docs/348: the camera each list is built with, and the vertical\n"
            "   band applied in clip space like the horizontal one. */\n"
            "extern \"C\" void lb_tag_on_swap(void);\n"
            "extern \"C\" int lb_band_geom_shift_y(int render_height);\n"
            "extern \"C\" int lb_clip_y_on(void);")

    #     SLIDE THE WIDENED FRUSTUM ONTO THE BAND, BEFORE THE CLIPPER.
    #
    #     The line above this one already widens the horizontal frustum, by
    #     shrinking clip X by 256/RenderWidth -- that is what reveals the extra
    #     world the band shows, at unchanged pixels-per-world. But it widens
    #     SYMMETRICALLY about the camera, and band_compute clamps the band
    #     ASYMMETRICALLY at a world edge.
    #
    #     Measured, a pan into the map's east edge at a 1712-wide surface. The
    #     camera stops at 1240 on a 1536-px map, the clamp puts the whole
    #     extension west (band 1370), so the visible world runs -130..1496 --
    #     while the frustum, symmetric with half-width RenderWidth/2 = 856,
    #     covers 384..2096. Everything west of 384 is clipped, and the render
    #     polygon list goes EMPTY:
    #
    #         step burst  cam   gx_polygons count
    #            0     0   755        3      (min_x 312..335)
    #            0     1  1027        0
    #          ...                    0   <- all 52 remaining samples
    #
    #     On screen that shows as sprites deforming into slivers and
    #     then vanishing while panning, and as the picture shifting: once no 3D
    #     frame is snapshotted, the compositor's `extra` freezes at the last
    #     band while the live one runs on (1095 against 1370, a 275 px shift).
    #
    #     WHY HERE AND NOT AT THE VIEWPORT. lb_xbias applied the same shift at
    #     the viewport transform, which runs AFTER clipping: it places whatever
    #     survived the frustum and cannot bring back what the clipper dropped.
    #
    #     WHY HERE AND NOT AT THE CLIP MATRIX. That was tried and reverted. The
    #     guest does not rebuild its clip matrix every frame, so a band baked in
    #     at matrix-build time goes stale as the band moves -- measured as the
    #     units sliding ~27 px per 32 px of camera travel, a tracking fault
    #     traded for a clipping one. SubmitVertex runs per vertex, so the band
    #     it reads is always this frame's.
    #
    #     A TRANSLATION, NOT A SCALE. Adding s*W to Position[0] shifts NDC and
    #     leaves pixels-per-world alone, so sprite widths are unchanged -- the
    #     invariant docs/251 measured (23/25/23 across surfaces 484/946/1266)
    #     is the check that this did not become the vertical axis's stretch.
    #
    #     The shift is the one lb_xbias applied, in clip space: band - centre
    #     pixels out of a viewport_width of RenderWidth is an NDC shift of
    #     2*(band - centre)/RenderWidth, and Position[0] carries W's scale, so
    #     the delta is W * 2 * (band - centre) / RenderWidth. lb_xbias is then
    #     ZERO; applying both would move the picture twice.
    #
    #     LB_FULLVP_X=0 restores the viewport-stage bias, which is the A/B
    #     between a shift the clipper knows about and one it does not.
    replace(gpu3d_cpp,
            "    if (RenderWidth > 256 && Viewport[0] == 0 && Viewport[4] == 256)\n"
            "        vertextrans->Position[0] = static_cast<s32>(\n"
            "            (static_cast<s64>(vertextrans->Position[0]) * 256) /\n"
            "            RenderWidth);",
            "    if (RenderWidth > 256 && Viewport[0] == 0 && Viewport[4] == 256)\n"
            "    {\n"
            "        vertextrans->Position[0] = static_cast<s32>(\n"
            "            (static_cast<s64>(vertextrans->Position[0]) * 256) /\n"
            "            RenderWidth);\n"
            "        /* legobattles-decomp: see apply_hooks.py. */\n"
            "        static const bool lb_shift_on = [] {\n"
            "            const char* v = getenv(\"LB_FULLVP_X\");\n"
            "            return !(v && v[0] == '0');\n"
            "        }();\n"
            "        if (lb_shift_on)\n"
            "        {\n"
            "            /* ONE VALUE FOR THE WHOLE FRAME, not one per vertex.\n"
            "               This read lb_band_left(RenderWidth) here, per\n"
            "               vertex. Two ways that stretches geometry: that\n"
            "               accessor drops to a CENTRED split whenever the width\n"
            "               it is asked about is not the latched one (hundreds\n"
            "               of pixels away at a wide surface), and if the answer\n"
            "               moves between two vertices of one polygon the\n"
            "               polygon is submitted half-shifted. The shift is\n"
            "               applied BEFORE the clipper, so one vertex can land\n"
            "               inside the frustum and its neighbour outside; the\n"
            "               clipper then makes a boundary vertex and the quad is\n"
            "               stretched to the screen edge: sprites stretched\n"
            "               horizontally very wide while panning, and only\n"
            "               while panning because\n"
            "               the split only moves when the clamp engages near a\n"
            "               world edge. lb_band_geom_shift is latched once per\n"
            "               frame and takes no width test, so it can do\n"
            "               neither. */\n"
            "            const int lb_shift = lb_band_geom_shift((int)RenderWidth);\n"
            "            if (lb_shift)\n"
            "                vertextrans->Position[0] += static_cast<s32>(\n"
            "                    ((s64)vertextrans->Position[3] * 2 *\n"
            "                     (s64)lb_shift) / (s64)RenderWidth);\n"
            "        }\n"
            "    }\n"
            "    /* legobattles-decomp docs/348: THE VERTICAL BAND, IN CLIP SPACE.\n"
            "       UpdateClipMatrix widens the Y frustum symmetrically about the\n"
            "       camera (ClipMatrix Y scaled by 192/RenderHeight); the band\n"
            "       slides the guest's 192 rows within the surface at the map's\n"
            "       north and south edges. Shifting AFTER the clipper (the old\n"
            "       lb_ybias) placed only what the symmetric frustum kept, so at\n"
            "       an edge a strip RenderHeight-192 over 2 rows tall got no 3D.\n"
            "       Here, before the clipper, it moves the frustum with the band.\n"
            "       Screen y = (w - Position[1]) * RenderHeight / 2w, so moving the\n"
            "       picture down by s rows subtracts 2*w*s/RenderHeight. Same\n"
            "       viewport test as the transform's enhanced_full_viewport_y. */\n"
            "    {\n"
            "        static const bool lb_vp_y_on = [] {\n"
            "            const char* v = getenv(\"LB_FULLVP_Y\");\n"
            "            return !(v && v[0] == '0');\n"
            "        }();\n"
            "        if (lb_vp_y_on && lb_clip_y_on() && RenderHeight > 192 &&\n"
            "            Viewport[3] == 0 && Viewport[5] == 192)\n"
            "        {\n"
            "            const int lb_shift_y =\n"
            "                lb_band_geom_shift_y((int)RenderHeight);\n"
            "            if (lb_shift_y)\n"
            "                vertextrans->Position[1] -= static_cast<s32>(\n"
            "                    ((s64)vertextrans->Position[3] * 2 *\n"
            "                     (s64)lb_shift_y) / (s64)RenderHeight);\n"
            "        }\n"
            "    }")

    #     THE BAND FOLLOWS THE GEOMETRY THROUGH THE BANK SWAP.
    #
    #     Vertices are transformed as the guest submits them, during the frame;
    #     they are not rasterised until the NEXT frame's scanlines. So the band
    #     the compositor must use is not the current one -- it is the one those
    #     already-transformed vertices went through. This is the moment the two
    #     part company: FlushRequest hands the submitted geometry over to the
    #     renderer, so it is also the moment to hand its band over.
    #
    #     Only in the FlushRequest branch. The else branch reuses the previous
    #     frame's geometry unchanged (RenderFrameIdentical), and its band must
    #     stay unchanged with it.
    replace(gpu3d_cpp,
            "                RenderNumPolygons = NumPolygons;\n"
            "                RenderFrameIdentical = false;",
            "                RenderNumPolygons = NumPolygons;\n"
            "                RenderFrameIdentical = false;\n"
            "                /* legobattles-decomp: see apply_hooks.py */\n"
            "                lb_band_promote();")
    done.append("melonds: the band is promoted with the geometry banks")
    # docs/342: stamp the scanline at which the guest's SwapBuffers EXECUTES.
    # A flush executed after VBlank starts latches one VBlank late.
    replace(gpu3d_cpp,
            "            FlushRequest = 1;\n"
            "            FlushAttributes = entry.Param & 0x3;",
            "            FlushRequest = 1;\n"
            "            FlushAttributes = entry.Param & 0x3;\n"
            "            lb_evt(0);   /* legobattles-decomp docs/342 */\n"
            "            lb_tag_on_swap();   /* docs/348 */")
    done.append("melonds: trace the scanline of each swap")
    replace(gpu3d_cpp,
            "    poly->VTop = vtop; poly->VBottom = vbot;\n"
            "    poly->YTop = ytop; poly->YBottom = ybot;\n"
            "    poly->XTop = xtop; poly->XBottom = xbot;",
            "    poly->VTop = vtop; poly->VBottom = vbot;\n"
            "    poly->YTop = ytop; poly->YBottom = ybot;\n"
            "    poly->XTop = xtop; poly->XBottom = xbot;\n"
            "\n"
            "    /* legobattles-decomp: see the note in apply_hooks.py. */\n"
            "    {\n"
            "        static int lb_diag = -1;\n"
            "        if (lb_diag < 0)\n"
            "        {\n"
            "            const char* lb_s = getenv(\"LB_POLYDIAG\");\n"
            "            lb_diag = lb_s ? atoi(lb_s) : 0;\n"
            "            if (lb_diag == 1) lb_diag = 2000;\n"
            "        }\n"
            "        if (lb_diag > 0)\n"
            "        {\n"
            "            static unsigned lb_tot = 0, lb_bot = 0, lb_low = 0,\n"
            "                            lb_key = 0;\n"
            "            ++lb_tot;\n"
            "            if (ybot > 192) ++lb_bot;\n"
            "            if (ytop >= 192) ++lb_low;\n"
            "            if (ybot > 255 || ytop > 255) ++lb_key;\n"
            "            if (lb_tot >= (unsigned)lb_diag)\n"
            "            {\n"
            "                fprintf(stderr, \"[lb_polydiag] RenderHeight=%u  \"\n"
            "                        \"of %u polys: %u had ybot>192 (the old \"\n"
            "                        \"bound dropped these), %u lie wholly \"\n"
            "                        \"below row 192, %u overflowed the old \"\n"
            "                        \"8-bit sort key\\n\",\n"
            "                        RenderHeight, lb_tot, lb_bot, lb_low,\n"
            "                        lb_key);\n"
            "                lb_tot = 0; lb_bot = 0; lb_low = 0; lb_key = 0;\n"
            "            }\n"
            "        }\n"
            "    }")
    done.append("melonds: polygon Y bounds follow RenderHeight (+LB_POLYDIAG)")

    #     ...and the scanline counts that decide how many rows are produced.
    #
    #     Every one of these is a literal 192 standing for "the height of the
    #     DS screen". With RenderHeight defaulting to 192 they are unchanged, so
    #     this is still inert; it is what lets the renderer produce more rows
    #     once something asks.
    # The clear loop appears TWICE -- the textured-clear path and the flat one.
    # replace() insists on a unique match, which is the right rule, so each is
    # anchored on its own preceding line rather than the check being loosened.
    replace(soft_cpp,
            "        u8 yoff = (gpu.GPU3D.RenderClearAttr2 >> 24) & 0xFF;\n"
            "\n"
            "        for (int y = 0; y < ScanlineWidth*192; y+=ScanlineWidth)",
            "        u8 yoff = (gpu.GPU3D.RenderClearAttr2 >> 24) & 0xFF;\n"
            "\n"
            "        for (int y = 0; y < ScanlineWidth*RenderHeight;"
            " y+=ScanlineWidth)")
    replace(soft_cpp,
            "        polyid |= (gpu.GPU3D.RenderClearAttr1 & 0x8000);\n"
            "\n"
            "        for (int y = 0; y < ScanlineWidth*192; y+=ScanlineWidth)",
            "        polyid |= (gpu.GPU3D.RenderClearAttr1 & 0x8000);\n"
            "\n"
            "        for (int y = 0; y < ScanlineWidth*RenderHeight;"
            " y+=ScanlineWidth)")
    replace(soft_cpp,
            "    for (s32 y = 1; y < 192; y++)",
            "    for (s32 y = 1; y < RenderHeight; y++)")
    #     THE FRAME-END VALUE IS MaxRenderHeight, NOT RenderHeight, and that
    #     is a deadlock fix rather than a tidy-up.
    #
    #     GetLine() waits until RenderedScanlines passes the row it was asked
    #     for. EnableRenderThread() resets the count to 0 and the semaphore to
    #     empty at the start of each frame, and the ONLY thing that posts to it
    #     is the render thread -- which parks on Sema_RenderStart until the
    #     MAIN thread wakes it. So if the frame just finished at height 192 and
    #     a window resize then raises RenderHeight, a main-thread request for
    #     row 200 waits for a count this frame will never reach, and the thread
    #     that would have started the next frame is the one waiting. Hard hang,
    #     reproduced: the size sweep froze at the first window taller than the
    #     DS's own 192 rows.
    #
    #     Storing MaxRenderHeight at frame end means "this frame is done, no
    #     further rows are coming", which is true whatever height it ran at,
    #     and releases every possible waiter. The per-scanline stores inside
    #     the loop still gate correctly, because those are the real counts.
    replace(soft_cpp,
            "        RenderedScanlines.store(192, std::memory_order_release);\n"
            "        Platform::Semaphore_Post(Sema_ScanlineCount);",
            "        RenderedScanlines.store(MaxRenderHeight,"
            " std::memory_order_release);\n"
            "        Platform::Semaphore_Post(Sema_ScanlineCount);")
    done.append("melonds: scanline counts follow RenderHeight")

    #     Three more 192s in the soft renderer that the pass above walked past.
    #
    #     ScanlineFinalPass(..., 191) finalises the LAST row. Hard-coded, it
    #     runs the edge-marking/fog/anti-alias pass over row 191 a second time
    #     and never runs it over row RenderHeight-1 at all.
    #
    #     GetLine() only WAITS for the render thread when the row is below 192.
    #     Above that it returns the buffer immediately, so with the height
    #     raised the compositor reads rows the render thread has not written
    #     yet -- a read/write race over two thirds of the picture, which is
    #     exactly the kind of thing that reads as flicker rather than as a bug.
    #
    #     And the FrameIdentical shortcut posts a count of 192 rather than
    #     RenderHeight, so a skipped frame releases waiters for only the top
    #     192 rows.
    replace(soft_cpp,
            "    ScanlineFinalPass(gpu.GPU3D, 191);",
            "    ScanlineFinalPass(gpu.GPU3D, RenderHeight - 1);")
    replace(soft_cpp,
            "        if (line < 192)",
            "        if (line < RenderHeight)")
    #     The skip-frame shortcut posts the same end-of-frame sentinel, for the
    #     same reason as above.
    replace(soft_cpp,
            "            RenderedScanlines.store(192, std::memory_order_release);\n"
            "            Platform::Semaphore_Post(Sema_ScanlineCount, 192);",
            "            RenderedScanlines.store(MaxRenderHeight,"
            " std::memory_order_release);\n"
            "            Platform::Semaphore_Post(Sema_ScanlineCount,"
            " MaxRenderHeight);")
    #     And GetLine must never index past the buffer it returns into, however
    #     the caller arrived at the row number.
    replace(soft_cpp,
            "    return &ColorBuffer[(line * ScanlineWidth) + FirstPixelOffset];",
            "    if (line < 0) line = 0;\n"
            "    if (line >= NumScanlines) line = NumScanlines - 1;\n"
            "    return &ColorBuffer[(line * ScanlineWidth) + FirstPixelOffset];")
    done.append("melonds: final pass, GetLine wait and skip-frame count "
                "follow RenderHeight")

    #     LB_3DWAIT=1: DOES THE EMULATION THREAD WAIT FOR THE RASTERISER?
    #
    #     docs/353. At 3440x1440 (surface 1536x642) a profile of the main
    #     thread spends ~10% of its samples inside kernel waits it cannot
    #     name, emulation grows from 9.6 to 13.3 ms/frame with the surface,
    #     and the software rasteriser -- one thread, whatever the width --
    #     has 2.5x the pixels of the 842x474 surface where docs/347 measured
    #     a wait of 0.009 ms/frame. The main thread meets the render thread
    #     at exactly two places: VCount144, which waits for the frame begun
    #     at the previous VCount 215 to finish, and GetLine, which waits for
    #     a row the compositor asks for. Both are timed here, with the render
    #     thread's own time per frame, and reported every 300 frames. Off, it
    #     costs one static read per call.
    replace(soft_cpp,
            "#include <string.h>\n",
            "#include <string.h>\n"
            "#include <chrono>     /* legobattles-decomp: LB_3DWAIT */\n"
            "#include <cstdlib>\n")
    replace(soft_cpp,
            "void SoftRenderer::VCount144(GPU& gpu)\n"
            "{\n"
            "    if (RenderThreadRunning.load(std::memory_order_relaxed)"
            " && !gpu.GPU3D.AbortFrame)\n"
            "        Platform::Semaphore_Wait(Sema_RenderDone);\n"
            "}",
            "/* legobattles-decomp docs/353: LB_3DWAIT=1. */\n"
            "namespace {\n"
            "using LbClock = std::chrono::steady_clock;\n"
            "bool lb_3dwait_on() {\n"
            "    static const bool on = std::getenv(\"LB_3DWAIT\") != nullptr;\n"
            "    return on;\n"
            "}\n"
            "uint64_t lb_us_since(LbClock::time_point t0) {\n"
            "    return static_cast<uint64_t>(\n"
            "        std::chrono::duration_cast<std::chrono::microseconds>(\n"
            "            LbClock::now() - t0).count());\n"
            "}\n"
            "std::atomic<uint64_t> lb_render_us{0}, lb_render_n{0},"
            " lb_render_max{0};\n"
            "std::atomic<uint64_t> lb_line_us{0}, lb_line_n{0};\n"
            "uint64_t lb_144_us = 0, lb_144_n = 0, lb_frames = 0;\n"
            "void lb_3dwait_render(LbClock::time_point t0) {\n"
            "    const uint64_t us = lb_us_since(t0);\n"
            "    lb_render_us += us;\n"
            "    ++lb_render_n;\n"
            "    uint64_t m = lb_render_max.load();\n"
            "    while (us > m && !lb_render_max.compare_exchange_weak(m, us)) {}\n"
            "}\n"
            "void lb_3dwait_line(LbClock::time_point t0) {\n"
            "    lb_line_us += lb_us_since(t0);\n"
            "    ++lb_line_n;\n"
            "}\n"
            "void lb_3dwait_frame(LbClock::time_point t0, int w, int h) {\n"
            "    const uint64_t us = lb_us_since(t0);\n"
            "    lb_144_us += us;\n"
            "    if (us >= 50u) ++lb_144_n;\n"
            "    if (++lb_frames % 300u != 0u) return;\n"
            "    const uint64_t rn = lb_render_n.exchange(0);\n"
            "    const uint64_t ru = lb_render_us.exchange(0);\n"
            "    const uint64_t rm = lb_render_max.exchange(0);\n"
            "    const uint64_t ln = lb_line_n.exchange(0);\n"
            "    const uint64_t lu = lb_line_us.exchange(0);\n"
            "    std::fprintf(stderr, \"[lb_3dwait] 300 frames at %dx%d: \"\n"
            "        \"render thread %.2f ms per render (%llu renders, max %.2f);\"\n"
            "        \" emu thread waited at VCount144 %.3f ms/frame (%llu \"\n"
            "        \"frames >= 0.05 ms), in GetLine %.3f ms/frame (%llu \"\n"
            "        \"waits)\\n\", w, h,\n"
            "        rn ? ru / 1000.0 / rn : 0.0, (unsigned long long)rn,\n"
            "        rm / 1000.0, lb_144_us / 1000.0 / 300.0,\n"
            "        (unsigned long long)lb_144_n, lu / 1000.0 / 300.0,\n"
            "        (unsigned long long)ln);\n"
            "    std::fflush(stderr);\n"
            "    lb_144_us = 0;\n"
            "    lb_144_n = 0;\n"
            "}\n"
            "}  // namespace\n"
            "\n"
            "void SoftRenderer::VCount144(GPU& gpu)\n"
            "{\n"
            "    const LbClock::time_point lb_t0 =\n"
            "        lb_3dwait_on() ? LbClock::now() : LbClock::time_point{};\n"
            "    if (RenderThreadRunning.load(std::memory_order_relaxed)"
            " && !gpu.GPU3D.AbortFrame)\n"
            "        Platform::Semaphore_Wait(Sema_RenderDone);\n"
            "    if (lb_3dwait_on()) lb_3dwait_frame(lb_t0, RenderWidth,"
            " RenderHeight);\n"
            "}")
    replace(soft_cpp,
            "        else\n"
            "        {\n"
            "            ClearBuffers(gpu);\n"
            "            RenderPolygons(gpu, true, &gpu.GPU3D.RenderPolygonRAM[0],"
            " gpu.GPU3D.RenderNumPolygons);\n"
            "        }",
            "        else\n"
            "        {\n"
            "            const LbClock::time_point lb_r0 =\n"
            "                lb_3dwait_on() ? LbClock::now()"
            " : LbClock::time_point{};\n"
            "            ClearBuffers(gpu);\n"
            "            RenderPolygons(gpu, true, &gpu.GPU3D.RenderPolygonRAM[0],"
            " gpu.GPU3D.RenderNumPolygons);\n"
            "            if (lb_3dwait_on()) lb_3dwait_render(lb_r0);\n"
            "        }")
    replace(soft_cpp,
            "            while (RenderedScanlines.load(std::memory_order_acquire)"
            " <= line)\n"
            "                Platform::Semaphore_Wait(Sema_ScanlineCount);",
            "            if (lb_3dwait_on() &&\n"
            "                RenderedScanlines.load(std::memory_order_acquire)"
            " <= line) {\n"
            "                const LbClock::time_point lb_w0 = LbClock::now();\n"
            "                while (RenderedScanlines.load("
            "std::memory_order_acquire) <= line)\n"
            "                    Platform::Semaphore_Wait(Sema_ScanlineCount);\n"
            "                lb_3dwait_line(lb_w0);\n"
            "            }\n"
            "            while (RenderedScanlines.load(std::memory_order_acquire)"
            " <= line)\n"
            "                Platform::Semaphore_Wait(Sema_ScanlineCount);")
    done.append("melonds: LB_3DWAIT rasteriser wait probe (docs/353)")

    #     docs/358: THE RENDER THREAD'S PRIORITY, MEASURABLE IN-RUN.
    #
    #     The emulation thread runs at SDL_THREAD_PRIORITY_HIGH and waits for
    #     this thread in GetLine and at VCount144. The compositor's row
    #     workers now run at that priority too, and during a composite the
    #     two together hold nearly every logical CPU, so a render thread
    #     at normal priority shares what is left with the rest of the
    #     desktop. Raising it looked obvious and MEASURED WORSE: interleaved
    #     in-run under load, 8% more frames over 20 ms (paired t 2.5). So it
    #     stays at normal. The thread registers itself first thing so that
    #     src/lb_prof.cpp holds its handle: LB_RENDERPRIO=1 raises it, and
    #     LB_POOLAB=render switches it in-run to measure it again.
    replace(soft_cpp,
            "void SoftRenderer::SetupRenderThread(GPU& gpu)\n"
            "{\n",
            "/* legobattles-decomp docs/358: see apply_hooks.py. */\n"
            "extern \"C\" void lb_note_render_thread(void);\n"
            "\n"
            "void SoftRenderer::SetupRenderThread(GPU& gpu)\n"
            "{\n")
    replace(soft_cpp,
            "            RenderThread = Platform::Thread_Create([this, &gpu]() {\n"
            "                RenderThreadFunc(gpu);\n"
            "            });",
            "            RenderThread = Platform::Thread_Create([this, &gpu]() {\n"
            "                lb_note_render_thread();   /* docs/358 */\n"
            "                RenderThreadFunc(gpu);\n"
            "            });")
    done.append("melonds: the render thread's handle, for its priority "
                "(docs/358)")

    #     docs/359: TWO COLOUR BUFFERS INSTEAD OF A 5 MB COPY A FRAME.
    #
    #     The widened compositor runs at the frame boundary, after the render
    #     thread has begun the NEXT 3D frame at VCount 215 in the one colour
    #     buffer it has. So render_engine_line snapshotted every wide 3D row
    #     into g_wide_3d_frame as its scanline passed, and the 528 rows below
    #     the guest's 192 at line 191: at 1720x642 about 5 MB a frame on the
    #     emulation thread, 2.3% of it (docs/358 s2).
    #
    #     With two buffers used in turn, a frame's rows stay whole until the
    #     frame after next begins to render, which is after this frame's
    #     composite. So the snapshot can be the renderer's own rows.
    #
    #     NOT A TOGGLE. Each render draws into the buffer that is NOT the
    #     one on display, recorded as DisplayedBuffer when RenderFrame starts
    #     a frame at VCount 215. A plain toggle would be wrong on the one
    #     path that renders a frame twice: RestartFrame, which GPU::StartFrame
    #     calls for an aborted 3D frame at line 0 -- in the same tick as the
    #     flip and BEFORE the composite reads the snapshot. A toggle there
    #     would clear the snapshot's buffer; this re-renders into the same
    #     target as the aborted attempt. A frame skipped as identical draws
    #     nothing and leaves the shown buffer as it is.
    #
    #     The switch happens where a frame starts to be drawn, before
    #     ClearBuffers, on both the threaded and the unthreaded path. GetLine
    #     hands out ShownColor, which the switch publishes with release
    #     order; a GetLine that waited for a row of the new frame acquires
    #     RenderedScanlines, stored after the switch. DisplayedBuffer is
    #     written on the emulation thread before Sema_RenderStart is posted
    #     and read by the render thread after it is taken, and VCount144 has
    #     already waited out the previous render.
    soft_h = os.path.join(runner_dir, "vendor", "melonds", "GPU3D_Soft.h")
    replace(soft_h,
            "    const u32* GetAttrLine(int line) override;\n",
            "    const u32* GetAttrLine(int line) override;\n"
            "    bool OwnsColorLine(const u32* line) const noexcept;"
            "   /* docs/359 */\n")
    replace(soft_h,
            "    u32 ColorBuffer[BufferSize * 2];\n",
            "    /* legobattles-decomp docs/359: TWO COLOUR BUFFERS, one per\n"
            "       rendered frame in turn. ColorBuffer is the one being\n"
            "       drawn and ShownColor the one GetLine hands out, so a\n"
            "       frame's rows stay whole until the frame after next\n"
            "       starts to render. See apply_hooks.py. */\n"
            "    u32 ColorStore[2][BufferSize * 2];\n"
            "    u32* ColorBuffer = ColorStore[0];\n"
            "    u32* DisplayedBuffer = ColorStore[0];\n"
            "    std::atomic<u32*> ShownColor {ColorStore[0]};\n"
            "    void lb_NextColorBuffer() noexcept;\n")
    replace(soft_cpp,
            "void SoftRenderer::Reset(GPU& gpu)\n"
            "{\n"
            "    memset(ColorBuffer, 0, BufferSize * 2 * 4);\n",
            "/* legobattles-decomp docs/359: see the header and apply_hooks.py.\n"
            "   The buffer that is not on display, never a toggle. */\n"
            "void SoftRenderer::lb_NextColorBuffer() noexcept\n"
            "{\n"
            "    ColorBuffer = (DisplayedBuffer == ColorStore[0]) ? ColorStore[1]\n"
            "                                                     : ColorStore[0];\n"
            "    ShownColor.store(ColorBuffer, std::memory_order_release);\n"
            "}\n"
            "\n"
            "bool SoftRenderer::OwnsColorLine(const u32* line) const noexcept\n"
            "{\n"
            "    const u32* lo = &ColorStore[0][0];\n"
            "    return line >= lo && line < lo + 2 * (BufferSize * 2);\n"
            "}\n"
            "\n"
            "void SoftRenderer::Reset(GPU& gpu)\n"
            "{\n"
            "    memset(ColorStore, 0, sizeof(ColorStore));   /* docs/359 */\n"
            "    ColorBuffer = ColorStore[0];\n"
            "    DisplayedBuffer = ColorStore[0];\n"
            "    ShownColor.store(ColorBuffer, std::memory_order_release);\n")
    replace(soft_cpp,
            "void SoftRenderer::RenderFrame(GPU& gpu)\n"
            "{\n",
            "void SoftRenderer::RenderFrame(GPU& gpu)\n"
            "{\n"
            "    /* docs/359: the frame on display now, which this render must\n"
            "       not draw over. RestartFrame does not come through here. */\n"
            "    DisplayedBuffer = ShownColor.load(std::memory_order_relaxed);\n"
            "\n")
    replace(soft_cpp,
            "            ClearBuffers(gpu);\n"
            "            RenderPolygons(gpu, true,",
            "            lb_NextColorBuffer();   /* docs/359 */\n"
            "            ClearBuffers(gpu);\n"
            "            RenderPolygons(gpu, true,")
    replace(soft_cpp,
            "        ClearBuffers(gpu);\n"
            "        RenderPolygons(gpu, false,",
            "        lb_NextColorBuffer();   /* docs/359 */\n"
            "        ClearBuffers(gpu);\n"
            "        RenderPolygons(gpu, false,")
    replace(soft_cpp,
            "    return &ColorBuffer[(line * ScanlineWidth) + FirstPixelOffset];",
            "    return &ShownColor.load(std::memory_order_acquire)"
            "[(line * ScanlineWidth) + FirstPixelOffset];   /* docs/359 */")
    #     The runner asks whether a line it was handed is one of those rows,
    #     rather than the shared ScrolledLine (X scroll, aborted frame) or the
    #     compute renderer's single frame, which must still be copied.
    gpu3d_cpp = os.path.join(runner_dir, "src", "gpu3d.cpp")
    replace(gpu3d_cpp,
            "const uint32_t* nds_gpu3d_wide_attr_line(int line) {",
            "/* legobattles-decomp docs/359: does this 3D line stay whole until\n"
            "   the frame after next starts to render? Only the software\n"
            "   renderer's own double-buffered rows do. */\n"
            "bool nds_gpu3d_line_persists(const uint32_t* line) {\n"
            "    if (!line || g_nds.GPU.GPU3D.IsRendererAccelerated())\n"
            "        return false;\n"
            "    auto* renderer = dynamic_cast<melonDS::SoftRenderer*>(\n"
            "        &g_nds.GPU.GPU3D.GetCurrentRenderer());\n"
            "    return renderer && renderer->OwnsColorLine(line);\n"
            "}\n"
            "\n"
            "const uint32_t* nds_gpu3d_wide_attr_line(int line) {")
    gpu3d_h = os.path.join(runner_dir, "src", "gpu3d.h")
    replace(gpu3d_h,
            "const uint32_t* nds_gpu3d_wide_attr_line(int line);",
            "const uint32_t* nds_gpu3d_wide_attr_line(int line);\n"
            "bool nds_gpu3d_line_persists(const uint32_t* line);  /* docs/359 */")
    #     The runner side: a row pointer per snapshot row, pointing at the
    #     renderer's row when it persists and at the old copy when it does
    #     not. LB_SNAPCOPY=1 forces the copy, for A/B. A row the frame did not
    #     record (a line the copy block never reached) reads the copy
    #     buffer, as before: the table is cleared when the frame's slot
    #     becomes the back buffer.
    replace(gpu,
            "std::array<AdaptiveFrame, 2> g_wide_3d_attr_frame{};\n",
            "std::array<AdaptiveFrame, 2> g_wide_3d_attr_frame{};\n"
            "/* legobattles-decomp docs/359: see apply_hooks.py. */\n"
            "std::array<std::array<const uint32_t*, kMaxAdaptiveHeight>, 2>\n"
            "    g_wide_3d_rows{};\n"
            "static bool lb_snap_copy() {\n"
            "    static const bool on = [] {\n"
            "        const char* v = std::getenv(\"LB_SNAPCOPY\");\n"
            "        return v && v[0] == '1';\n"
            "    }();\n"
            "    return on;\n"
            "}\n"
            "/* LB_SNAPAB=1: copy and pointers in turn, 300 frames each, timing\n"
            "   the snapshot's work per frame (the GetLine waits included, which\n"
            "   are the same either way). Reported every 600 frames. */\n"
            "static bool lb_snap_ab() {\n"
            "    static const bool on = [] {\n"
            "        const char* v = std::getenv(\"LB_SNAPAB\");\n"
            "        return v && v[0] == '1';\n"
            "    }();\n"
            "    return on;\n"
            "}\n"
            "static unsigned g_lb_snap_frames = 0;\n"
            "static double g_lb_snap_ms[2] = {0.0, 0.0};   /* [pointers] */\n"
            "static unsigned g_lb_snap_n[2] = {0u, 0u};\n"
            "static bool lb_snap_ab_copy() {\n"
            "    return lb_snap_ab() && ((g_lb_snap_frames / 300u) & 1u) == 0u;\n"
            "}\n"
            "static void lb_keep_3d_row(int slot, int row, uint16_t width) {\n"
            "    if (row < 0 || row >= kMaxAdaptiveHeight) return;\n"
            "    const bool timed = lb_snap_ab();\n"
            "    const auto t0 = timed ? std::chrono::steady_clock::now()\n"
            "                          : std::chrono::steady_clock::time_point{};\n"
            "    const bool must_copy = lb_snap_copy() || lb_snap_ab_copy();\n"
            "    const uint32_t* src = nds_gpu3d_wide_line(row);\n"
            "    uint32_t* copy = g_wide_3d_frame[slot].data() +\n"
            "                     static_cast<size_t>(row) * width;\n"
            "    if (!must_copy && nds_gpu3d_line_persists(src)) {\n"
            "        g_wide_3d_rows[slot][row] = src;\n"
            "    } else {\n"
            "        std::copy_n(src, width, copy);\n"
            "        g_wide_3d_rows[slot][row] = copy;\n"
            "    }\n"
            "    if (timed)\n"
            "        g_lb_snap_ms[must_copy ? 0 : 1] +=\n"
            "            std::chrono::duration<double, std::milli>(\n"
            "                std::chrono::steady_clock::now() - t0).count();\n"
            "}\n"
            "static void lb_snap_ab_frame() {\n"
            "    if (!lb_snap_ab()) return;\n"
            "    ++g_lb_snap_n[lb_snap_ab_copy() ? 0 : 1];\n"
            "    if (++g_lb_snap_frames % 600u != 0u) return;\n"
            "    std::fprintf(stderr,\n"
            "                 \"[lb_snapab] 3D snapshot per frame, 300 frames each,\"\n"
            "                 \" interleaved: copied %.3f ms, pointers %.3f ms\\n\",\n"
            "                 g_lb_snap_n[0] ? g_lb_snap_ms[0] / g_lb_snap_n[0] : 0.0,\n"
            "                 g_lb_snap_n[1] ? g_lb_snap_ms[1] / g_lb_snap_n[1] : 0.0);\n"
            "    std::fflush(stderr);\n"
            "    g_lb_snap_ms[0] = g_lb_snap_ms[1] = 0.0;\n"
            "    g_lb_snap_n[0] = g_lb_snap_n[1] = 0u;\n"
            "}\n"
            "static const uint32_t* lb_snap_row(int slot, int row, int width) {\n"
            "    const uint32_t* p = (row >= 0 && row < kMaxAdaptiveHeight)\n"
            "        ? g_wide_3d_rows[slot][row] : nullptr;\n"
            "    return p ? p : g_wide_3d_frame[slot].data() +\n"
            "                   static_cast<size_t>(row) * width;\n"
            "}\n")
    replace(gpu,
            "                std::copy_n(nds_gpu3d_wide_line(y), wide_width,\n"
            "                            g_wide_3d_frame[g_front ^ 1].data() + offset);\n",
            "                /* docs/359: a pointer, or a copy if it must be. */\n"
            "                lb_keep_3d_row(g_front ^ 1, y, wide_width);\n")
    replace(gpu,
            "                        std::copy_n(nds_gpu3d_wide_line(lb_y),\n"
            "                                    wide_width,\n"
            "                                    g_wide_3d_frame[g_front ^ 1]\n"
            "                                        .data() + lb_off);\n",
            "                        lb_keep_3d_row(g_front ^ 1, lb_y,\n"
            "                                       wide_width);   /* docs/359 */\n")
    replace(gpu,
            "            ? g_wide_3d_frame[g_front].data() +\n"
            "                  static_cast<size_t>(y) * output_width\n"
            "            : nds_gpu3d_wide_line(y);",
            "            ? lb_snap_row(g_front, y, output_width)   /* docs/359 */\n"
            "            : nds_gpu3d_wide_line(y);")
    replace(gpu,
            "                    const uint32_t* lb_s =\n"
            "                        g_wide_3d_frame[g_front].data() +\n"
            "                        static_cast<size_t>(lb_r) * output_width;",
            "                    const uint32_t* lb_s =\n"
            "                        lb_snap_row(g_front, lb_r, output_width);")
    replace(gpu,
            "void nds_gpu2d_finish_frame(){g_front ^= 1;}",
            "void nds_gpu2d_finish_frame(){\n"
            "    lb_snap_ab_frame();   /* docs/359: before the mode can change */\n"
            "    g_front ^= 1;\n"
            "    /* docs/359: the slot the next frame records into. */\n"
            "    g_wide_3d_rows[g_front ^ 1].fill(nullptr);\n"
            "}")
    replace(gpu,
            "    for (auto& frame : g_wide_3d_frame) frame.fill(0u);\n",
            "    for (auto& frame : g_wide_3d_frame) frame.fill(0u);\n"
            "    for (auto& rows : g_wide_3d_rows) rows.fill(nullptr);"
            "   /* docs/359 */\n")
    done.append("melonds + gpu2d: double-buffered 3D colour, the snapshot "
                "keeps pointers (docs/359)")

    #     docs/360: THE RASTER'S BORDER, FOR EVERY ROW THE RASTER HAS.
    #
    #     Edge marking (DISP3DCNT bit 5) compares each pixel with its four
    #     neighbours, and at the raster's edges those are the one-pixel
    #     border ClearBuffers fills with the clear depth and polygon ID. Its
    #     loops still said 192 rows: side borders for buffer rows 1..192,
    #     the bottom border at row 193. With the surface taller than 192,
    #     the side borders below it and the true bottom border (row
    #     RenderHeight + 1) were never cleared -- depth 0 from Reset, or a
    #     taller frame's pixels after the window shrinks -- so edges there
    #     were missing or came from stale geometry.
    #
    #     LEGO Battles draws with edge marking on: Building_Draw3D
    #     (0203B3AC) sets bit 5 of 0x04000060 (its literal at 0203B764) on
    #     every building it draws. At RenderHeight 192 these are the same
    #     loops as before, so the DS's own picture cannot move.
    replace(soft_cpp,
            "    for (int x = ScanlineWidth; x < ScanlineWidth*193; x+=ScanlineWidth)\n",
            "    /* legobattles-decomp docs/360: every row, not 192. */\n"
            "    for (int x = ScanlineWidth; x < ScanlineWidth*(RenderHeight+1);"
            " x+=ScanlineWidth)\n")
    replace(soft_cpp,
            "    for (int x = ScanlineWidth*193; x < ScanlineWidth*194; x++)\n",
            "    for (int x = ScanlineWidth*(RenderHeight+1);"
            " x < ScanlineWidth*(RenderHeight+2); x++)   /* docs/360 */\n")
    #     LB_3DCNT=1: the DISP3DCNT each frame is rendered WITH (the latched
    #     copy, not the register the game keeps rewriting), printed when it
    #     changes. Answers whether a frame is edge-marked at all.
    replace(soft_cpp,
            "    DisplayedBuffer = ShownColor.load(std::memory_order_relaxed);\n",
            "    DisplayedBuffer = ShownColor.load(std::memory_order_relaxed);\n"
            "    {   /* docs/360: LB_3DCNT=1 */\n"
            "        static const bool lb_on = std::getenv(\"LB_3DCNT\") != nullptr;\n"
            "        static unsigned lb_last = 0xFFFFFFFFu;\n"
            "        const unsigned lb_cnt = (unsigned)gpu.GPU3D.RenderDispCnt;\n"
            "        if (lb_on && lb_cnt != lb_last) {\n"
            "            lb_last = lb_cnt;\n"
            "            std::fprintf(stderr, \"[lb_3dcnt] rendering with DISP3DCNT\"\n"
            "                         \" %04X: edge marking %s, antialias %s,\"\n"
            "                         \" fog %s, %ux%u\\n\", lb_cnt,\n"
            "                         (lb_cnt & 0x20u) ? \"ON\" : \"off\",\n"
            "                         (lb_cnt & 0x10u) ? \"ON\" : \"off\",\n"
            "                         (lb_cnt & 0x80u) ? \"ON\" : \"off\",\n"
            "                         (unsigned)RenderWidth, (unsigned)RenderHeight);\n"
            "        }\n"
            "    }\n")
    done.append("melonds: the raster's edge-marking border cleared for every "
                "row (docs/360)")

    #     docs/361: ENGINE A'S NATIVE PICTURE, SKIPPED WHEN NOTHING SHOWS IT.
    #
    #     In a widened battle frame the emulation thread composes engine A's
    #     256x192 picture every scanline (~0.64 ms a frame at 1720x642,
    #     docs/358 s6) and the widened composite replaces it on screen. It is
    #     shown only when the composite, which runs at the frame boundary with
    #     the NEXT frame's registers, refuses the scene. So a skip decided at
    #     scanline time has to survive the frame the game leaves the battle
    #     configuration. The decomp says how it leaves: a timed master-
    #     brightness fade (FUN_0200d9bc/0200da2c/0200dad8) to -16, i.e. black,
    #     and a line drawn at full fade is exactly 0xFF000000 (darken by 16
    #     zeroes every channel) whatever its layers were.
    #
    #     Per scanline of engine A this decides whether the composite would
    #     take the frame (every condition it checks, evaluated then, plus the
    #     title's adaptive screen and a widened surface) and records that with
    #     the line's master brightness. A skipped line composes nothing and
    #     writes nothing; everything else the scanline does (the 3D snapshot,
    #     the widemap scan) still happens.
    #
    #     COMPLETION. nds_gpu2d_framebuffer() fills a skipped line in before
    #     anyone reads it: with the exact colour when the line was at full
    #     fade, else by composing it late (inexact, logged -- never seen). The
    #     composite and the frontend's per-frame fetch use _raw and complete
    #     only when they really show the native picture. A reader that needs
    #     the true picture (the debug server's native framebuffer, the frame
    #     dumps, the battlefield panel) calls nds_gpu2d_want_native(), which
    #     stops skipping for 120 frames.
    #
    #     LB_NATSKIP=0 turns it off. LB_NATSKIP=probe skips nothing and, when
    #     a frame with skippable lines is shown natively (a HAZARD), checks
    #     pixel for pixel that its rows are the colour the fill would write.
    #     Measured with the probe (docs/361 s2): 8,583 widened battle frames in
    #     a recorded replay, no hazard; the pause menu's one hazard frame was
    #     192 lines at full fade, all 192 rows exactly the fill colour.
    replace(gpu,
            "void render_engine_line(int engine, int y) {\n"
            "    Unit& u=g_unit[engine];\n",
            "/* legobattles-decomp docs/361: LB_NATSKIP. See apply_hooks.py. */\n"
            "struct LbNatSkipSlot {\n"
            "    /* skipped (probing: skippable) when drawn */\n"
            "    std::array<uint8_t, 192> line{};\n"
            "    std::array<uint16_t, 192> mb{};    /* MASTER_BRIGHT then */\n"
            "    int screen = -1;                   /* where engine A drew */\n"
            "};\n"
            "static std::array<LbNatSkipSlot, 2> g_natskip{};\n"
            "static unsigned g_natskip_frame = 0, g_natskip_want_until = 0;\n"
            "static int lb_natskip_mode() {   /* 0 off, 1 skip, 2 probe */\n"
            "    static const int m = [] {\n"
            "        const char* v = std::getenv(\"LB_NATSKIP\");\n"
            "        if (!v) return 1;\n"
            "        if (v[0] == 'p') return 2;\n"
            "        return v[0] == '0' ? 0 : 1;\n"
            "    }();\n"
            "    return m;\n"
            "}\n"
            "/* LB_NATSKIP=force skips even while a reader wants the true\n"
            "   picture (a test, with dumps on). LB_NATSKIP=ab skips in every\n"
            "   other block of 300 frames and times engine A's scanlines. */\n"
            "static char lb_natskip_flag() {\n"
            "    static const char f = [] {\n"
            "        const char* v = std::getenv(\"LB_NATSKIP\");\n"
            "        return v ? v[0] : '\\0';\n"
            "    }();\n"
            "    return f;\n"
            "}\n"
            "static bool lb_natskip_ab_draws() {   /* an A/B block that draws */\n"
            "    return lb_natskip_flag() == 'a' &&\n"
            "           ((g_natskip_frame / 300u) & 1u) != 0u;\n"
            "}\n"
            "static double g_natskip_ab_ms[2] = {0.0, 0.0};   /* [skipping] */\n"
            "static unsigned g_natskip_ab_n[2] = {0u, 0u};\n"
            "static void lb_natskip_ab_frame() {\n"
            "    if (lb_natskip_flag() != 'a') return;\n"
            "    ++g_natskip_ab_n[lb_natskip_ab_draws() ? 0 : 1];\n"
            "    if ((g_natskip_frame + 1u) % 600u != 0u) return;\n"
            "    std::fprintf(stderr,\n"
            "                 \"[lb_natskip] engine A's scanlines per frame, 300 \"\n"
            "                 \"frames each, interleaved: drawn %.3f ms, skipping \"\n"
            "                 \"%.3f ms\\n\",\n"
            "                 g_natskip_ab_n[0] ? g_natskip_ab_ms[0] / g_natskip_ab_n[0] : 0.0,\n"
            "                 g_natskip_ab_n[1] ? g_natskip_ab_ms[1] / g_natskip_ab_n[1] : 0.0);\n"
            "    std::fflush(stderr);\n"
            "    g_natskip_ab_ms[0] = g_natskip_ab_ms[1] = 0.0;\n"
            "    g_natskip_ab_n[0] = g_natskip_ab_n[1] = 0u;\n"
            "}\n"
            "static bool lb_natskip_screen_on(int screen);   /* after the mask */\n"
            "void render_engine_line(int engine, int y) {\n"
            "    Unit& u=g_unit[engine];\n"
            "    if (engine == 0 && y >= 0 && y < 192 && lb_natskip_mode())\n"
            "        g_natskip[g_front ^ 1].line[y] = 0;   /* docs/361 */\n")
    replace(gpu,
            "    const bool bg0_3d = engine == 0 && (u.dispcnt & 0x8u) != 0 &&\n"
            "                        (u.dispcnt & 0x100u) != 0;\n"
            "\n"
            "    if (bg0_3d || cap || mode != 1u) {\n",
            "    const bool bg0_3d = engine == 0 && (u.dispcnt & 0x8u) != 0 &&\n"
            "                        (u.dispcnt & 0x100u) != 0;\n"
            "    /* docs/361: would the composite take this line's frame? The\n"
            "       conditions it checks, as they stand at this scanline. */\n"
            "    bool lb_natskip_skip = false;\n"
            "    if (engine == 0 && y >= 0 && y < 192 && lb_natskip_mode()) {\n"
            "        bool lb_text = true;\n"
            "        for (int lb_bg = 1; lb_bg < 4; ++lb_bg)\n"
            "            if ((u.dispcnt & (0x100u << lb_bg)) &&\n"
            "                bg_kind(u, lb_bg) != BgKind::Text)\n"
            "                lb_text = false;\n"
            "        const int lb_screen = lb_engine_a_on_top() ? 0 : 1;\n"
            "        const bool lb_ok =\n"
            "            mode == 1u && bg0_3d && lb_text && !cap &&\n"
            "            !g_frame_capture_active && !g_direct_frame_active &&\n"
            "            lb_setting_enhanced() && lb_natskip_screen_on(lb_screen) &&\n"
            "            (nds_gpu3d_output_width() > 256 ||\n"
            "             nds_gpu3d_output_height() > 192);\n"
            "        /* Skipping also waits while a reader wants the true picture. */\n"
            "        const bool lb_now = lb_ok &&\n"
            "            (lb_natskip_mode() == 2 || lb_natskip_flag() == 'f' ||\n"
            "             g_natskip_frame >= g_natskip_want_until) &&\n"
            "            !lb_natskip_ab_draws();\n"
            "        LbNatSkipSlot& lb_s = g_natskip[g_front ^ 1];\n"
            "        lb_s.line[y] = lb_now ? 1 : 0;\n"
            "        lb_s.mb[y] = static_cast<uint16_t>(u.master_bright);\n"
            "        lb_s.screen = lb_screen;\n"
            "        lb_natskip_skip = lb_now && lb_natskip_mode() == 1;\n"
            "    }\n"
            "\n"
            "    if (bg0_3d || cap || mode != 1u) {\n")
    #     ...and the line itself: past the 3D snapshot, before any compose.
    replace(gpu,
            "        static std::array<uint32_t, 256> comp6;\n",
            "        /* docs/361: the widened composite replaces this line. */\n"
            "        if (lb_natskip_skip) return;\n"
            "        static std::array<uint32_t, 256> comp6;\n")
    replace(gpu,
            "    g_lb_adaptive_screens = mask;\n"
            "}\n",
            "    g_lb_adaptive_screens = mask;\n"
            "}\n"
            "/* docs/361: see render_engine_line and apply_hooks.py. Declared\n"
            "   in the anonymous namespace up there, so defined in it here. */\n"
            "namespace {\n"
            "static bool lb_natskip_screen_on(int screen) {\n"
            "    return (g_lb_adaptive_screens & (1u << (screen & 1))) != 0u;\n"
            "}\n"
            "}  // namespace\n"
            "/* docs/361: a skipped line composed NOW, with the registers as they\n"
            "   stand now -- the next frame's. Only for a skipped line that was\n"
            "   not at full fade and is needed after all: inexact by nature,\n"
            "   never measured, logged. The affine reference points the decode\n"
            "   advances are put back, so the next frame's lines are untouched. */\n"
            "static void lb_natskip_late_line(int y, uint32_t* dst) {\n"
            "    Unit& u = g_unit[0];\n"
            "    const uint8_t* const palette = nds_vram_renderer_palette(0);\n"
            "    const uint8_t* const oam = nds_vram_renderer_oam(0);\n"
            "    const NdsVramRendererView* const vram = nds_vram_renderer_view(0);\n"
            "    if (!palette || !oam || !vram || (u.dispcnt & 0x80u)) {\n"
            "        std::fill_n(dst, 256, 0xFFFFFFFFu);\n"
            "        return;\n"
            "    }\n"
            "    const uint32_t mode = (u.dispcnt >> 16) & 3u;\n"
            "    if (mode != 1u) {\n"
            "        std::fill_n(dst, 256, mode == 0u ? 0xFFFFFFFFu : to_rgb32(0u));\n"
            "        return;\n"
            "    }\n"
            "    const bool bg0_3d = (u.dispcnt & 0x8u) != 0 &&\n"
            "                        (u.dispcnt & 0x100u) != 0;\n"
            "    int32_t rx[2], ry[2];\n"
            "    for (int i = 0; i < 2; ++i) {\n"
            "        rx[i] = u.refx_internal[i];\n"
            "        ry[i] = u.refy_internal[i];\n"
            "    }\n"
            "    std::array<uint32_t, 256> comp6{};\n"
            "    compose_line6(0, y, u, palette, oam, *vram,\n"
            "                  bg0_3d ? nds_gpu3d_line(y + lb_band_render_y())\n"
            "                         : nullptr,\n"
            "                  bg0_3d, comp6.data());\n"
            "    for (int i = 0; i < 2; ++i) {\n"
            "        u.refx_internal[i] = rx[i];\n"
            "        u.refy_internal[i] = ry[i];\n"
            "    }\n"
            "    const uint32_t mbmode = u.master_bright >> 14;\n"
            "    const uint32_t mb = std::min<uint32_t>(16, u.master_bright & 0x1Fu);\n"
            "    for (int x = 0; x < 256; ++x) {\n"
            "        uint32_t c = comp6[x];\n"
            "        if (mbmode == 1u) c = brighten(c, mb, 0u);\n"
            "        else if (mbmode == 2u) c = darken(c, mb, 15u);\n"
            "        dst[x] = to_rgb32(c);\n"
            "    }\n"
            "}\n"
            "/* docs/361: fill in the skipped lines of the native picture on\n"
            "   screen, for a reader about to read it. A line drawn at full fade\n"
            "   is the exact colour the native path produces (darken or brighten\n"
            "   by 16 leaves nothing of the layers). Once per frame. */\n"
            "static unsigned g_natskip_exact = 0, g_natskip_late = 0;\n"
            "void nds_gpu2d_native_complete(int screen) {\n"
            "    if (lb_natskip_mode() != 1) return;\n"
            "    LbNatSkipSlot& s = g_natskip[g_front];\n"
            "    if (s.screen != (screen & 1)) return;\n"
            "    unsigned& exact = g_natskip_exact;\n"
            "    unsigned& late = g_natskip_late;\n"
            "    uint32_t* const nat = g_fb[g_front][screen & 1].data();\n"
            "    int n = 0, nl = 0;\n"
            "    for (int y = 0; y < 192; ++y) {\n"
            "        if (!s.line[y]) continue;\n"
            "        s.line[y] = 0;\n"
            "        ++n;\n"
            "        const uint32_t m = s.mb[y] >> 14, f = s.mb[y] & 0x1Fu;\n"
            "        uint32_t* const row = nat + y * 256;\n"
            "        if (m == 2u && f >= 16u) std::fill_n(row, 256, to_rgb32(0u));\n"
            "        else if (m == 1u && f >= 16u)\n"
            "            std::fill_n(row, 256, to_rgb32(0x3F3F3Fu));\n"
            "        else { lb_natskip_late_line(y, row); ++nl; }\n"
            "    }\n"
            "    if (!n) return;\n"
            "    if (nl) ++late; else ++exact;\n"
            "    if (nl || exact <= 3u) {\n"
            "        std::fprintf(stderr,\n"
            "                     \"[lb_natskip] completed %d skipped lines of the \"\n"
            "                     \"native picture: %d exact (full fade), %d \"\n"
            "                     \"composed late (inexact); frames so far: %u \"\n"
            "                     \"exact, %u late\\n\", n, n - nl, nl, exact, late);\n"
            "        std::fflush(stderr);\n"
            "    }\n"
            "}\n"
            "const uint32_t* nds_gpu2d_framebuffer_raw(int screen) {\n"
            "    return g_fb[g_front][screen & 1].data();\n"
            "}\n"
            "void nds_gpu2d_want_native(void) {\n"
            "    g_natskip_want_until = g_natskip_frame + 120u;\n"
            "}\n"
            "static void lb_natskip_present(int screen, bool widened,\n"
            "                               uint32_t dispcnt_now) {\n"
            "    if (!lb_natskip_mode()) return;\n"
            "    static unsigned presents = 0, would = 0, full = 0, hazards = 0,\n"
            "                    synth = 0, verified = 0;\n"
            "    const LbNatSkipSlot& s = g_natskip[g_front];\n"
            "    int n = 0;\n"
            "    for (int y = 0; y < 192; ++y) n += s.line[y] ? 1 : 0;\n"
            "    ++presents;\n"
            "    if (n && widened) {\n"
            "        ++would;\n"
            "        if (n == 192) ++full;\n"
            "    } else if (n && lb_natskip_mode() == 1) {\n"
            "        /* Skipped lines, and the native picture shown after all. */\n"
            "        ++hazards;\n"
            "        std::fprintf(stderr,\n"
            "                     \"[lb_natskip] present %u shows the native picture \"\n"
            "                     \"of a frame with %d skipped lines; dispcnt now \"\n"
            "                     \"%08X\\n\", presents, n, (unsigned)dispcnt_now);\n"
            "        nds_gpu2d_native_complete(screen);\n"
            "    } else if (n) {\n"
            "        /* A HAZARD: skippable lines, and the native picture shown. */\n"
            "        ++hazards;\n"
            "        const uint32_t* nat = g_fb[g_front][screen & 1].data();\n"
            "        int fade = 0, match = 0;\n"
            "        for (int y = 0; y < 192; ++y) {\n"
            "            if (!s.line[y]) continue;\n"
            "            const uint32_t m = s.mb[y] >> 14, f = s.mb[y] & 0x1Fu;\n"
            "            uint32_t want = 0;\n"
            "            if (m == 2u && f >= 16u) want = to_rgb32(0u);\n"
            "            else if (m == 1u && f >= 16u) want = to_rgb32(0x3F3F3Fu);\n"
            "            else continue;\n"
            "            ++fade;\n"
            "            bool same = true;\n"
            "            for (int x = 0; x < 256; ++x)\n"
            "                if (nat[y * 256 + x] != want) { same = false; break; }\n"
            "            if (same) ++match;\n"
            "        }\n"
            "        if (fade == n) ++synth;\n"
            "        if (match == n) ++verified;\n"
            "        std::fprintf(stderr,\n"
            "                     \"[lb_natskip] HAZARD at present %u: %d skippable \"\n"
            "                     \"lines shown natively, %d at full fade, %d of \"\n"
            "                     \"those rendered as the synthesized colour; \"\n"
            "                     \"dispcnt now %08X\\n\", presents, n, fade, match,\n"
            "                     (unsigned)dispcnt_now);\n"
            "    }\n"
            "    if (presents % 600u == 0u) {\n"
            "        if (lb_natskip_mode() == 2)\n"
            "            std::fprintf(stderr,\n"
            "                     \"[lb_natskip] %u presents: %u widened with skippable \"\n"
            "                     \"lines (%u all 192), %u hazards (%u fully faded, \"\n"
            "                     \"%u verified pixel for pixel)\\n\",\n"
            "                     presents, would, full, hazards, synth, verified);\n"
            "        else\n"
            "            std::fprintf(stderr,\n"
            "                     \"[lb_natskip] %u presents: %u widened with skipped \"\n"
            "                     \"lines (%u all 192); %u native pictures shown \"\n"
            "                     \"after skipping, completed %u exactly and %u \"\n"
            "                     \"late\\n\", presents, would, full, hazards,\n"
            "                     g_natskip_exact, g_natskip_late);\n"
            "        std::fflush(stderr);\n"
            "    }\n"
            "}\n")
    #     Every reader of the native picture completes it first; the frame
    #     counter moves at the flip; the header says so.
    replace(gpu,
            "const uint32_t* nds_gpu2d_framebuffer(int screen){\n"
            "    return g_fb[g_front][screen & 1].data();\n"
            "}",
            "const uint32_t* nds_gpu2d_framebuffer(int screen){\n"
            "    nds_gpu2d_native_complete(screen);   /* docs/361 */\n"
            "    return g_fb[g_front][screen & 1].data();\n"
            "}")
    replace(gpu,
            "    g_wide_3d_rows[g_front ^ 1].fill(nullptr);\n"
            "}",
            "    g_wide_3d_rows[g_front ^ 1].fill(nullptr);\n"
            "    /* docs/361: the frame count, and a clean slot for the next\n"
            "       frame (one that never draws engine A must not inherit\n"
            "       another frame's skipped lines). */\n"
            "    lb_natskip_ab_frame();\n"
            "    ++g_natskip_frame;\n"
            "    g_natskip[g_front ^ 1].line.fill(0);\n"
            "}")
    replace(gpu,
            "    if (!profiling()) {\n"
            "        if (!g_direct_frame_active) render_engine_line(0, line);\n"
            "        render_engine_line(1, line);\n"
            "        return;\n"
            "    }",
            "    if (!profiling()) {\n"
            "        if (!g_direct_frame_active) {\n"
            "            if (lb_natskip_flag() == 'a') {   /* docs/361 A/B */\n"
            "                const auto lb_t0 = std::chrono::steady_clock::now();\n"
            "                render_engine_line(0, line);\n"
            "                g_natskip_ab_ms[lb_natskip_ab_draws() ? 0 : 1] +=\n"
            "                    std::chrono::duration<double, std::milli>(\n"
            "                        std::chrono::steady_clock::now() - lb_t0)\n"
            "                        .count();\n"
            "            } else {\n"
            "                render_engine_line(0, line);\n"
            "            }\n"
            "        }\n"
            "        render_engine_line(1, line);\n"
            "        return;\n"
            "    }")
    replace(os.path.join(runner_dir, "src", "gpu2d.h"),
            "const uint32_t* nds_gpu2d_framebuffer(int screen);",
            "const uint32_t* nds_gpu2d_framebuffer(int screen);\n"
            "/* legobattles-decomp docs/361: in a widened battle frame engine A's\n"
            "   native picture may be left unwritten. nds_gpu2d_framebuffer()\n"
            "   fills it in before returning; _raw does not, for a caller that\n"
            "   completes it itself only if it presents it. A reader that needs\n"
            "   the true picture calls nds_gpu2d_want_native(), which stops the\n"
            "   skipping for 120 frames. */\n"
            "const uint32_t* nds_gpu2d_framebuffer_raw(int screen);\n"
            "void nds_gpu2d_native_complete(int screen);\n"
            "void nds_gpu2d_want_native(void);")
    #     The calls in lb_adaptive_fb_inner, the frontend and the debug server
    #     are applied at the END of this function: the text they anchor on is
    #     written by later stanzas (the pre-clear move and the pairing block).
    done.append("gpu2d: engine A's native picture skipped when nothing shows "
                "it (docs/361)")

    #     The runner-side height accessor, mirroring nds_gpu3d_output_width.
    #     Still inert: the height is 192 until the layout asks for more.
    gpu3d_hdr = os.path.join(runner_dir, "src", "gpu3d.h")
    replace(gpu3d_hdr,
            "uint16_t nds_gpu3d_output_width();",
            "uint16_t nds_gpu3d_output_width();\n"
            "bool nds_gpu3d_set_output_height(uint16_t height);\n"
            "uint16_t nds_gpu3d_output_height();")
    replace(gpu3d,
            "uint16_t nds_gpu3d_output_width() {\n"
            "    return static_cast<uint16_t>(g_nds.GPU.GPU3D.GetRenderWidth());\n"
            "}",
            "uint16_t nds_gpu3d_output_width() {\n"
            "    return static_cast<uint16_t>(g_nds.GPU.GPU3D.GetRenderWidth());\n"
            "}\n"
            "\n"
            "bool nds_gpu3d_set_output_height(uint16_t height) {\n"
            "    // Same refusal as the width setter: an accelerated renderer\n"
            "    // fixes its geometry at session start.\n"
            "    // legobattles-decomp: 512, not 256. This setter is where the\n"
            "    // raised ceiling was MISSED: the vertex mask, the melonDS\n"
            "    // clamp and the frontend cap all went to 512 while this one\n"
            "    // kept refusing anything taller. A refusal is silent -- the\n"
            "    // frontend just re-asks next frame -- so the surface stayed\n"
            "    // at whatever height it happened to hold, and a 1893x1030\n"
            "    // window sat at 212 rows while its width tracked correctly.\n"
            "    if (height < 192u || height > 768u || (height & 1u)) return false;\n"
            "    if (g_nds.GPU.GPU3D.IsRendererAccelerated() &&\n"
            "        height != g_nds.GPU.GPU3D.GetRenderHeight())\n"
            "        return false;\n"
            "    g_nds.GPU.GPU3D.SetRenderHeight(height);\n"
            "    return true;\n"
            "}\n"
            "\n"
            "uint16_t nds_gpu3d_output_height() {\n"
            "    return static_cast<uint16_t>(g_nds.GPU.GPU3D.GetRenderHeight());\n"
            "}")
    done.append("gpu3d: runner-side output height accessor")

    # ---- Let the GPU actually run the 3D layer -------------------------------
    #
    # Measured on a machine with an NVIDIA RTX card, in its log:
    #
    #   [gpu3d] renderer policy: auto (preferred: OpenGL 4.3 compute)
    #   [gpu3d] OpenGL 4.3.0 NVIDIA GeForce RTX <model>/PCIe/SSE2
    #   [gpu3d] compute render width 512 is unsupported
    #   [gpu3d] compute renderer init failed ... using threaded software
    #
    # nds_gpu3d_use_compute_renderer refuses any width above kComputeMaxWidth,
    # which is still 448 -- the pre-adaptive ceiling. docs/166 found three
    # independent 448s and this hook set rewrites the other two; this one was
    # missed, so every adaptive width above 448 silently fell back to the
    # threaded software rasteriser on a card that offers GL 4.3 compute. That is
    # why an RTX GPU ran a DS game at only 35-46 fps.
    #
    # THE HEIGHT MUST GO WITH IT. g_compute_frame and g_compute_attr_frame are
    # sized kComputeMaxWidth * 192, because the 3D layer used to be 192 rows
    # always. The frontend now sizes the surface up to 512 rows and calls
    # nds_gpu3d_set_output_height with it, so raising the WIDTH alone would let
    # the renderer write a 512-row frame into storage for 192 -- the same class
    # of out-of-bounds write that kMaxAdaptiveHeight already caused, on a
    # different buffer. Both dimensions move together or neither does.
    #
    # Cost: 1024*512*4 = 2 MB per frame buffer, two of them.
    # THE WIDTH STAYS AT 448 FOR NOW, AND THAT IS A MEASURED DECISION.
    #
    # Raising it to 1024 does start the renderer -- 33 compute shaders compile
    # and the log reads "[gpu3d] renderer: OpenGL 4.3 compute" instead of
    # "using threaded software" -- and then the process SEGFAULTS a few lines
    # later, reproducibly, both under the harness and run directly:
    #
    #     [lb_cull] LEFT EDGE NOT PATCHED: ... (a pre-existing, harmless line)
    #     Segmentation fault      RUNNER_EXIT=139
    #
    # So there are more 448- and 192-derived assumptions inside the compute
    # bridge than the two buffers below. Shipping it would trade a low frame
    # rate for a crash, which is a worse bug. The ceiling
    # goes back until the rest of them are found; the next step is to bisect
    # by raising it to 512 alone (the smallest width that reproduces) with the
    # readback mapping and SetRenderWidth path under a debugger.
    #
    # kComputeMaxHeight is introduced ANYWAY, because it fixes a real latent
    # bug independent of the ceiling: the two frame buffers were sized
    # kComputeMaxWidth * 192 from when the 3D layer was always 192 rows, while
    # the frontend now calls nds_gpu3d_set_output_height with up to 512. At 448
    # wide that is 918 KB each instead of 344 KB -- cheap insurance against the
    # same class of out-of-bounds write kMaxAdaptiveHeight already caused.
    replace(gpu3d, "constexpr uint32_t kComputeMaxWidth = 448u;",
            "constexpr uint32_t kComputeMaxWidth = 448u;\n"
            "constexpr uint32_t kComputeMaxHeight = 512u;"
            "  /* legobattles-decomp: was an implicit 192 */")
    replace(gpu3d,
            "uint32_t g_compute_frame[kComputeMaxWidth * 192u] = {};\n"
            "uint32_t g_compute_attr_frame[kComputeMaxWidth * 192u] = {};",
            "uint32_t g_compute_frame[kComputeMaxWidth * kComputeMaxHeight]"
            " = {};\n"
            "uint32_t g_compute_attr_frame[kComputeMaxWidth *"
            " kComputeMaxHeight] = {};")
    done.append("gpu3d.cpp: compute frames sized for 512 rows (ceiling stays "
                "448: raising it segfaults, see the note above)")

    #     The adaptive framebuffer has to be able to HOLD the extra rows.
    #     Its WIDTH cap is already handled further up (docs/166 found three
    #     independent 448s and that hook raised this one to 1024), so only the
    #     height is added here.
    #     512, NOT 256. frontend.cpp clamps the surface height to 512
    #     (`if (lb_wanth > 512) lb_wanth = 512;`) and gpu3d accepts it, but this
    #     array was sized for 256 -- so nds_gpu2d_adaptive_framebuffer's
    #     `fill_n(adaptive.data(), output_width * output_height, ...)` wrote
    #     946*512 = 484352 words into storage for 262144, i.e. about 867 KB
    #     past the end of g_adaptive_frame, EVERY FRAME, into whatever objects
    #     follow it. The frontend's own note lists "FOUR places [that] have to
    #     agree"; this was a fifth it does not name.
    replace(gpu,
            "using AdaptiveFrame = std::array<uint32_t, kMaxAdaptiveWidth * 192>;",
            "constexpr int kMaxAdaptiveHeight = 768;\n"
            "using AdaptiveFrame ="
            " std::array<uint32_t, kMaxAdaptiveWidth * kMaxAdaptiveHeight>;")
    done.append("gpu2d: adaptive frame gains the extra rows")

    #     ...and the adaptive compositor actually PRODUCING them.
    #
    #     One subtlety runs through all of this: the loop variable is used as
    #     BOTH the output row and the guest scanline, and those are no longer
    #     the same thing. melonDS's viewport shift already moved the 3D scene
    #     down by (RenderHeight - 192) / 2, so the 3D line is indexed by the
    #     OUTPUT row; the 2D layers are still authored in the guest's own 192
    #     rows, so they are indexed by output row MINUS that offset. Getting
    #     this backwards would slide the terrain against the units.
    #
    #     While the height is 192 the offset is 0 and every line below is what
    #     it was, which is why this stays inert until the layout asks for more.
    replace(gpu,
            "    const int output_width = nds_gpu3d_output_width();\n"
            "    if (output_width <= 256) {\n"
            "        if (width) *width = 256;\n"
            "        return native;\n"
            "    }",
            "    const int output_width = nds_gpu3d_output_width();\n"
            "    const int output_height = nds_gpu3d_output_height();\n"
            "    // Guest row 0 lands here in the output. See above.\n"
            "    //\n"
            "    // NO LONGER HALF THE OVERHANG, for engine A: lb_band_top\n"
            "    // slides the guest's own 192 rows within the surface at the\n"
            "    // map's north or south edge, exactly as lb_band_left does\n"
            "    // horizontally, and it comes back paired with the 3D frame\n"
            "    // being composited so the two axes cannot shear. Engine B\n"
            "    // keeps the even split -- it has no map to be confined to.\n"
            "    /* docs/343 LB_PAIR: decided ONCE per composite, before any\n"
            "       band is chosen, so the held raster and its band travel\n"
            "       together. Only for a genuinely widened engine-A surface. */\n"
            "    const int lb_pair_hold =\n"
            "        (engine == 0 && (output_width > 256 || output_height > 192))\n"
            "            ? lb_pair_decide(output_width, output_height) : 0;\n"
            "    /* docs/348: WHICH 3D RASTER GOES WITH THIS GROUND. The live one\n"
            "       (latched at the VBlank just past) or the snapshot (on\n"
            "       screen during the frame that just ended), chosen by the\n"
            "       camera each was built with against the camera the ground\n"
            "       in VRAM was committed with. See lb_pair_pick. Decided once,\n"
            "       here, so the raster and its band travel together. */\n"
            "    const int lb_use_snap =\n"
            "        (engine == 0 && !lb_pair_hold &&\n"
            "         (output_width > 256 || output_height > 192))\n"
            "            ? lb_pair_pick(\n"
            "                  g_wide_3d_width[g_front] == output_width &&\n"
            "                      g_wide_3d_height[g_front] == output_height,\n"
            "                  g_wide_3d_camx[g_front], g_wide_3d_camy[g_front],\n"
            "                  g_wide_3d_camok[g_front])\n"
            "            : 0;\n"
            "    /* CLAMPED INTO THE SURFACE. A band left over from a wider\n"
            "       surface (the frame after a window shrink) made\n"
            "       output - 256 - extra negative, and decode_text_line then\n"
            "       decoded 256 + 2*extra columns into a 1728-entry line.\n"
            "       */\n"
            "    const int extra_y = std::clamp(\n"
            "        (engine != 0)\n"
            "            ? (output_height - 192) / 2\n"
            "            : lb_pair_hold ? lb_pair_band_y()\n"
            "            : lb_use_snap ? g_wide_3d_band_y[g_front]\n"
            "            : lb_band_render_y(),\n"
            "        0, output_height > 192 ? output_height - 192 : 0);\n"
            "    g_lb_used_extra_y = extra_y;   /* legobattles-decomp */\n"
            "    if (output_width <= 256 && output_height <= 192) {\n"
            "        if (width) *width = 256;\n"
            "        return native;\n"
            "    }")
    # THE FULL-SURFACE PRE-CLEAR MOVES TO THE PATHS THAT STILL NEED IT.
    #
    # Upstream clears the whole adaptive surface to opaque black before it
    # knows which path will fill it. On the main engine-A composite that clear
    # is DEAD: lb_row_body's inner loop ends in an unconditional
    # `dst[x] = to_rgb32(color);` for every x in [0, output_width) -- the three
    # `continue`s in there belong to the nested hud_bg_count loop, not to the x
    # loop -- and lb_parallel_rows covers every y in [0, output_height). Every
    # pixel the clear writes is overwritten before anything reads it.
    #
    # It is NOT dead on the early-return paths: the engine-B widener writes only
    # the guest's own rows and leaves lb_fill_rows_outward to carry them, and
    # the unsupported-scene path writes only the centred native image. So the
    # clear is MOVED to the head of those two blocks, not deleted.
    #
    # At 1712x710 it is 4.64 MB of stores on the MAIN thread, ahead of the
    # worker fan-out -- benchmarked in isolation at 0.602 ms mean / 0.382 best
    # (0.171 / 0.142 at 946x516). That is the largest single-threaded thing left
    # in the composite and it buys nothing on the path that matters.
    replace(gpu,
            "    std::fill_n(adaptive.data(),\n"
            "                static_cast<size_t>(output_width) * 192u,\n"
            "                0xFF000000u);",
            "    /* legobattles-decomp: the full-surface clear now lives at the\n"
            "       head of the two early-return blocks below, because the main\n"
            "       composite writes every pixel of every row and does not need\n"
            "       it. See the note beside this replace() in apply_hooks.py. */")
    replace(gpu,
            "    if (engine != 0) {\n"
            "        // legobattles-decomp: widen a pure-2D scene rather than\n"
            "        // letterboxing it. See src/lb_wide2d.inc and docs/107.\n",
            "    if (engine != 0) {\n"
            "        /* legobattles-decomp: this path writes only part of the\n"
            "           surface, so it clears first. */\n"
            "        std::fill_n(adaptive.data(),\n"
            "                    static_cast<size_t>(output_width) *\n"
            "                        static_cast<size_t>(output_height),\n"
            "                    0xFF000000u);\n"
            "        // legobattles-decomp: widen a pure-2D scene rather than\n"
            "        // letterboxing it. See src/lb_wide2d.inc and docs/107.\n")
    replace(gpu,
            "    if (!supported_scene) {\n"
            "        // legobattles-decomp: a scene this path calls unsupported may\n",
            "    if (!supported_scene) {\n"
            "        /* legobattles-decomp: this path writes only part of the\n"
            "           surface, so it clears first. See above. */\n"
            "        std::fill_n(adaptive.data(),\n"
            "                    static_cast<size_t>(output_width) *\n"
            "                        static_cast<size_t>(output_height),\n"
            "                    0xFF000000u);\n"
            "        // legobattles-decomp: a scene this path calls unsupported may\n")
    # NOTE the two "copy the native picture" paths -- engine B, and any scene
    # the widener declines -- are NOT centred here. This port's own lb_wide2d
    # hooks have already rewritten both blocks, so the upstream text those
    # replaces looked for no longer exists; centring them means editing those
    # hooks instead. Both are menus and non-3D scenes, and while the height is
    # 192 they are unaffected either way, so they belong to the step that turns
    # the height on rather than being guessed at now.
    replace(gpu,
            "        const size_t needed =\n"
            "            static_cast<size_t>(output_width) * 192u * 2u;",
            "        const size_t needed =\n"
            "            static_cast<size_t>(output_width) *\n"
            "                static_cast<size_t>(output_height) * 2u;")
    done.append("gpu2d: adaptive clear and native copies follow the height")

    #     The main engine-A composite loop, split into OUTPUT row and GUEST row.
    #
    #     `y` stays the output row -- the 3D line, the attribute line, the
    #     destination pointer and the HUD shift all use it, because melonDS's
    #     viewport shift has already moved the 3D scene down by extra_y.
    #     `gy` is the guest's own scanline, and the 2D layers use that.
    #
    #     OBJ is clamped to the guest's rows rather than extended: sprites are
    #     authored for a 192-row screen and there is nothing outside it to draw.
    #     (In this game OBJ is disabled outright during gameplay anyway.)
    #
    #     At height 192, extra_y is 0 and gy == y, so this is a no-op until the
    #     layout asks for more rows.
    #     Anchored on the HD block above it: this exact loop header also opens
    #     the direct-present object path a few hundred lines earlier, and that
    #     one is a different function that must not be touched.
    replace(gpu,
            "            g_hd_below_pixels.resize(needed);\n"
            "        }\n"
            "    }\n"
            "    for (int y = 0; y < 192; ++y) {\n"
            "        render_obj_line(0, y, obj.data(), output_width,\n"
            "                        oam, palette, *vram);",
            "            g_hd_below_pixels.resize(needed);\n"
            "        }\n"
            "    }\n"
            "    /* legobattles-decomp: 15.9 MB a frame of memset, for a layer\n"
            "       this game does not use. render_obj_line CLEARS the whole\n"
            "       out_width buffer and only THEN returns if OBJ is off:\n"
            "\n"
            "           std::fill_n(out, out_width, Pixel{});\n"
            "           if (!(u.dispcnt&0x1000u)) return;\n"
            "\n"
            "       In gameplay dispcnt is 00010F08 and bit 12 is clear, so OBJ\n"
            "       is off and every one of those clears is wasted. Pixel is 12\n"
            "       bytes and the surface is 1728x768, so that is 1.33M writes\n"
            "       -- 15.9 MB -- per frame to produce a buffer that is already\n"
            "       in exactly that state.\n"
            "\n"
            "       Skipping it is safe because `obj` is value-initialised at\n"
            "       declaration, which runs Pixel's own default member\n"
            "       initialisers (priority 4, valid false) -- byte for byte what\n"
            "       fill_n(Pixel{}) writes. With OBJ off nothing else touches\n"
            "       it, so it stays correct for every row. */\n"
            "    const bool lb_obj_on = (u.dispcnt & 0x1000u) != 0;\n"
            "    for (int y = 0; y < output_height; ++y) {\n"
            "        const int gy = y - extra_y;\n"
            "        if (!lb_obj_on) {\n"
            "            /* obj is already all-invalid; leave it alone */\n"
            "        } else if (gy >= 0 && gy < 192) {\n"
            "            render_obj_line(0, gy, obj.data(), output_width,\n"
            "                            oam, palette, *vram);\n"
            "        } else {\n"
            "            std::fill_n(obj.data(), output_width, Pixel{});\n"
            "        }")
    done.append("gpu2d: composite loop splits output row from guest row")

    #     THREAD THE COMPOSITOR, which is the only thing a bigger window costs.
    #
    #     Measured twice: emu_ticks is 19.7 ms and does not move when the
    #     surface grows 2.5x, so neither the emulator core nor the 3D rasteriser
    #     is what widescreen costs. The whole cost is this loop, and it is
    #     exactly linear -- 1.343 ms over 524k pixels, 6.054 ms over 1.33M,
    #     ~4.5 ns a pixel. There is no hotspot inside to delete; it is ~30
    #     operations on each of 1.33M pixels, which is the shape threading is
    #     for. See lb_parallel_rows in lb_wide2d.inc for why it is safe.
    #
    #     The scratch moves INSIDE the parallel body so each worker gets its own
    #     on its own stack. Deliberately not thread_local: obj[x] is read in the
    #     per-pixel loop and a TLS access there would hand back the win.
    replace(gpu, "#include <vector>",
            "#include <atomic>  /* LbRowPool's row counter, docs/349 */\n"
            "#include <condition_variable>\n"
            "#include <functional>\n"
            "#include <mutex>\n"
            "#include <thread>\n"
            "#include <vector>")
    replace(gpu,
            "    std::array<Pixel, kMaxAdaptiveWidth> obj{};\n"
            "    std::array<int16_t, kMaxAdaptiveWidth> sky_left{};\n"
            "    std::array<int16_t, kMaxAdaptiveWidth> sky_indices{};\n"
            "    std::array<int16_t, kMaxAdaptiveWidth> sky_rank{};\n"
            "    std::array<int16_t, kMaxAdaptiveWidth> black_run{};\n"
            "    std::array<uint32_t, kMaxAdaptiveWidth> repaired_3d{};\n"
            "    std::array<BgLine, 3> hud_bg_lines{};\n",
            "    /* legobattles-decomp: the per-row scratch now lives inside\n"
            "       the parallel body below, one set per worker. */\n")
    #     lb_hud_scan HOISTED. It ran as `if (y == 0)` inside the loop, which
    #     with workers would run on whichever thread happened to own row 0,
    #     concurrently with rows that are already reading what it writes. It
    #     writes g_boxes/g_count once a frame and every row only reads them, so
    #     it belongs before the fan-out.
    replace(gpu,
            "        if (y == 0) lb_hud_scan(output_width, output_height);"
            "   /* legobattles-decomp */\n",
            "")
    replace(gpu,
            "    const bool lb_obj_on = (u.dispcnt & 0x1000u) != 0;\n"
            "    for (int y = 0; y < output_height; ++y) {",
            "    const bool lb_obj_on = (u.dispcnt & 0x1000u) != 0;\n"
            "    /* docs/348: the widened BG1/BG2 tables, built for the camera\n"
            "       the ground now in VRAM was committed with -- the same ground\n"
            "       the native columns are about to be decoded from. Once, on\n"
            "       this thread, before the rows fan out. */\n"
            "    lb_widemap_compose_prep(output_width, output_height);\n"
            "    /* legobattles-decomp: once a frame, before any row reads it.\n"
            "       Given the SAME band the rows are about to composite with:\n"
            "       the in-field HUD is 3D geometry, so it was shifted by the\n"
            "       band when it was transformed, and the anchor that undoes\n"
            "       that has to use the same number or it mis-locates every\n"
            "       quad by the difference.\n"
            "\n"
            "       docs/348: the polygons it reads are the LIVE list's, placed\n"
            "       with the live list's band -- which is not the composited\n"
            "       raster's band on a frame that pairs the snapshot. It is told\n"
            "       both. */\n"
            "    lb_hud_set_list_band(lb_band_render(), lb_band_render_y());\n"
            "    lb_hud_scan(output_width, output_height, extra, extra_y);\n"
            "    lb_layer_begin(output_width, output_height);\n"
            "    /* legobattles-decomp docs/325 phase 2: render the level grid\n"
            "       once, here, for the same reason lb_hud_scan is hoisted --\n"
            "       the row body below is fanned out across workers and this\n"
            "       draws the whole map (1.256 ms, Phase 1). The per-pixel\n"
            "       sampler only reads what this writes, so the workers may\n"
            "       call it concurrently. */\n"
            "    lb_hostterrain_prepare();\n"
            "    auto lb_row_body = [&](int lb_lo, int lb_hi) {\n"
            "    std::array<Pixel, kMaxAdaptiveWidth> obj{};\n"
            "    std::array<int16_t, kMaxAdaptiveWidth> sky_left{};\n"
            "    std::array<int16_t, kMaxAdaptiveWidth> sky_indices{};\n"
            "    std::array<int16_t, kMaxAdaptiveWidth> sky_rank{};\n"
            "    std::array<int16_t, kMaxAdaptiveWidth> black_run{};\n"
            "    std::array<uint32_t, kMaxAdaptiveWidth> repaired_3d{};\n"
            "    std::array<BgLine, 3> hud_bg_lines{};\n"
            "    for (int y = lb_lo; y < lb_hi; ++y) {")
    replace(gpu,
            "            dst[x] = to_rgb32(color);\n"
            "        }\n"
            "    }\n"
            "    if (emit_hd) {",
            "            if (g_lb_layer_on)\n"
            "                g_lb_layer[(size_t)y * output_width + x] =\n"
            "                    top.valid ? (uint8_t)(top.order + 1) : 0u;\n"
            "            dst[x] = to_rgb32(color);\n"
            "            /* legobattles-decomp docs/325 phase 2 acceptance:\n"
            "               did the host pixel survive the layer stack? This\n"
            "               lives HERE, in the last stanza that rewrites this\n"
            "               region, because an earlier stanza's version was\n"
            "               silently consumed by this one -- the note went in,\n"
            "               this replace matched over it, and the counters\n"
            "               read 0/0 while the sampler reported 5,898,240\n"
            "               hits. One row; a whole frame would need atomics.\n"
            "               THE MIDDLE ROW, not a constant: y == 96 was in the\n"
            "               field at the 1280x720 it was written against and is\n"
            "               near the top of the picture at a\n"
            "               1855x988 window, where it read band 0/672 and looked like\n"
            "               a failure that was not one (docs/330). */\n"
            "            if (lb_ht_ok && y == (output_height >> 1))\n"
            "                lb_hostterrain_note(\n"
            "                    y, (x < extra || x >= extra + 256) ? 1 : 0,\n"
            "                    dst[x], to_rgb32(lb_ht));\n"
            "        }\n"
            "    }\n"
            "    };\n"
            "    lb_parallel_rows(output_height, lb_row_body);\n"
            "    /* legobattles-decomp docs/325 phase 2: one composited frame,\n"
            "       once, after every row is done. */\n"
            "    lb_hostterrain_dump_once(adaptive.data(), output_width,\n"
            "                             output_height);\n"
            "    lb_threadcheck(adaptive.data(), output_width,\n"
            "                   output_height, lb_row_body);\n"
            "    /* legobattles-decomp: the composite is over; sprites rendered\n"
            "       anywhere else get their own band again. See item #5. */\n"
            "    g_lb_obj_band_valid = false;\n"
            "    /* legobattles-decomp: where the 3D layer -- the units and the\n"
            "       base -- actually landed, stamped with the shared frame\n"
            "       counter so [lb_ent] can be paired with it exactly. */\n"
            "    lb_drift_3d_extent(output_width, output_height);\n"
            "    lb_layer_write(adaptive.data(), output_width, output_height);\n"
            "    if (emit_hd) {")
    done.append("gpu2d: the compositor runs its rows across workers")

    #     docs/354: THE WIDENED FIELD'S PER-PIXEL LOOP, SPECIALISED.
    #
    #     lb_fast_row (src/lb_wide2d.inc) computes the same pixel as the
    #     general loop below it, from plain values, whenever nothing it leaves
    #     out is in play: OBJ off (it is, in the field), no HD surfaces (the
    #     field is screen 1), no host-terrain base. The general loop is kept,
    #     unchanged, as the else branch, and LB_FASTROW=check runs both and
    #     counts every pixel that differs. After the stanza above, the last
    #     one to rewrite this loop, so both anchors below are its text.
    replace(gpu,
            "        for (int x = 0; x < output_width; ++x) {\n"
            "            /* legobattles-decomp docs/325 phase 2 */\n"
            "            Pixel lb_base = backdrop;\n",
            "        /* legobattles-decomp docs/354: the specialised loop, when\n"
            "           nothing it leaves out is in play. */\n"
            "        const int lb_fast_mode = lb_fastrow_mode();\n"
            "        const bool lb_fast = lb_fast_mode != 0 && !lb_ht_have &&\n"
            "                             !lb_obj_on && !emit_hd;\n"
            "        uint32_t* const lb_fast_dst =\n"
            "            (lb_fast && lb_fast_mode == 2)\n"
            "                ? lb_fast_scratch(output_width) : dst;\n"
            "        const bool lb_fast_timed = lb_fast && lb_fast_mode == 2;\n"
            "        const auto lb_fast_t0 = lb_fast_timed\n"
            "            ? std::chrono::steady_clock::now()\n"
            "            : std::chrono::steady_clock::time_point{};\n"
            "        if (lb_fast)\n"
            "            lb_fast_row(u, output_width, extra,\n"
            "                        g_adaptive_hud_anchor, hud_center_left,\n"
            "                        hud_center_right, hud_bg_lines.data(),\n"
            "                        hud_bg_count, composited_3d, prio3d,\n"
            "                        lb_cover, backdrop, mbmode, mb,\n"
            "                        (g_lb_layer_on && lb_fast_mode == 1)\n"
            "                            ? g_lb_layer.data() +\n"
            "                                  (size_t)y * output_width\n"
            "                            : nullptr,\n"
            "                        lb_fast_dst);\n"
            "        const auto lb_fast_t1 = lb_fast_timed\n"
            "            ? std::chrono::steady_clock::now()\n"
            "            : std::chrono::steady_clock::time_point{};\n"
            "        if (!lb_fast || lb_fast_mode == 2)\n"
            "        for (int x = 0; x < output_width; ++x) {\n"
            "            /* legobattles-decomp docs/325 phase 2 */\n"
            "            Pixel lb_base = backdrop;\n")
    replace(gpu,
            "                    dst[x], to_rgb32(lb_ht));\n"
            "        }\n"
            "    }\n"
            "    };\n"
            "    lb_parallel_rows(output_height, lb_row_body);\n",
            "                    dst[x], to_rgb32(lb_ht));\n"
            "        }\n"
            "        if (lb_fast_timed)\n"
            "            lb_fast_check_row(y, dst, lb_fast_dst, output_width,\n"
            "                              lb_fast_t0, lb_fast_t1,\n"
            "                              std::chrono::steady_clock::now());\n"
            "    }\n"
            "    };\n"
            "    lb_parallel_rows(output_height, lb_row_body);\n")
    done.append("gpu2d.cpp: the widened field's per-pixel loop, specialised "
                "(docs/354)")

    #     THE MENU DOUBLE-OFFSET -- the same output-row/guest-row split, in the
    #     one function that was missed.
    #
    #     render_engine_line() builds the guest's OWN 256x192 picture, in the
    #     guest's own rows, and it sampled the 3D layer at row `y`. That was
    #     right until the viewport shift above started centring 3D geometry on
    #     a taller surface: guest row y now lives at 3D surface row y+extra_y,
    #     so sampling at y lays the 3D layer extra_y BELOW the 2D layers inside
    #     the native image.
    #
    #     It matters because that native image is not always thrown away. A
    #     scene neither widener will take -- and the Single Player menu is one,
    #     it logs "[lb_wideA] a non-text HUD BG" and then "[lb_wide2d] engine A
    #     3D scene" -- falls through to lb_menu_fill, which places the native
    #     image at output row y+extra_y. So its 2D layers came out at extra_y
    #     and anything 3D at 2*extra_y.
    #
    #     Measured on that menu. The render list holds three polygons and the
    #     Story book is one of them, at guest y 62..118 at EVERY surface height
    #     -- the 3D path was never wrong. The game also draws a flat icon
    #     behind it, and at surface 626x336 (extra_y 72) the two separated by
    #     exactly 72 guest rows, 69..110 and 134..189, giving the two books in
    #     the capture. At height 192 they coincide and the menu is correct.
    #
    #     extra_y is 0 whenever the surface is the DS's own height, so the
    #     native and capture paths are byte-identical to before.
    replace(gpu,
            "        const uint32_t* line3d =\n"
            "            (engine == 0) ? nds_gpu3d_line(y) : nullptr;",
            "        // legobattles-decomp: the guest's own row y lives at 3D\n"
            "        // surface row y + extra_y. See apply_hooks.py.\n"
            "        // NOT half the overhang: lb_band_top may slide the\n"
            "        // guest's rows within the surface at a map edge, and the\n"
            "        // geometry this line is being read from was transformed\n"
            "        // with the PROMOTED value, which is the one that came\n"
            "        // with the frame the renderer has just finished.\n"
            "        const int lb_3d_extra_y = lb_band_render_y();\n"
            "        /* NO LAG CORRECTION HERE, and the reason is recorded in\n"
            "           docs/335. The lag is real -- the 3D is a snapshot of\n"
            "           the frame the renderer just finished and the guest\n"
            "           steps its camera on alternate frames, so geometry\n"
            "           lands two compositor frames behind the ground -- but\n"
            "           sliding the 3D ROW's sample index is the wrong shape\n"
            "           of fix, twice over:\n"
            "\n"
            "             * the in-field HUD is 3D geometry in the SAME\n"
            "               polygon list as the units (lb_hud.cpp). Shifting\n"
            "               the row moves the HUD too, and the HUD is\n"
            "               screen-locked -- it would gain exactly the jitter\n"
            "               the units lose.\n"
            "             * on a camera discontinuity the two-frame delta is\n"
            "               enormous. Measured: 518 of 519 traced frames read\n"
            "               lagdx 0 and the one that did not read 32, which is\n"
            "               the clamp, not a pan.\n"
            "\n"
            "           A correct fix shifts only world geometry, which means\n"
            "           doing it where z is known, and resets across a camera\n"
            "           jump. lb_lag_dx() is kept for the trace only. */\n"
            "        const uint32_t* line3d =\n"
            "            (engine == 0) ? nds_gpu3d_line(y + lb_3d_extra_y)\n"
            "                          : nullptr;")
    done.append("gpu2d: the native composite samples the 3D layer at the "
                "guest's row")

    # THE SCROLL BIAS, EXPORTED SO A TRACE CAN READ THE RIGHT COPY.
    #
    # docs/195 read the sub-tile scroll phase out of the cartridge:
    # func_0202D1B4 writes BGxOFS as `-param_2 & mask` and func_020396E0 calls
    # it with `param_2 = 4 - (camx mod 8)`, giving an invariant the ORIGINAL
    # renderer satisfies by construction:
    #
    #     camx & 7 == (BG1HOFS + 4) & 7
    #
    # Testing that against a debug read of 0x04000014 measures nothing: BGxOFS
    # is WRITE-ONLY on the DS, the read returns 0 forever, and this file
    # already says so a few hundred lines up -- "those registers are
    # write-only, and reading them back returned 0 while the picture plainly
    # showed a non-zero offset". Repeating that mistake gave a tidy,
    # entirely false 58.7%.
    #
    # g_unit[engine].bgx[bg] is the value the renderer actually scrolls with,
    # which is the only copy the invariant can honestly be tested against.
    replace(gpu,
            "std::array<Unit,2> g_unit{};",
            "std::array<Unit,2> g_unit{};\n"
            "/* legobattles-decomp: the scroll bias the renderer USES. See\n"
            "   apply_hooks.py -- BGxOFS is write-only and reads back 0. */\n"
            "extern \"C\" unsigned lb_bg_scroll(int engine, int bg, int vert) {\n"
            "    if (engine < 0 || engine > 1 || bg < 0 || bg > 3) return 0u;\n"
            "    return vert ? g_unit[engine].bgy[bg] : g_unit[engine].bgx[bg];\n"
            "}")
    done.append("gpu2d: export the renderer's own scroll bias for the trace")
    # docs/342: stamp the scanline at which the guest writes BG1HOFS on
    # engine A -- func_020396E0's scroll commit, per the decomp.
    replace(gpu,
            "    const int e=(addr&0x1000u)?1:0;Unit&u=g_unit[e];"
            "const uint32_t off=addr&0xFFFu;\n",
            "    const int e=(addr&0x1000u)?1:0;Unit&u=g_unit[e];"
            "const uint32_t off=addr&0xFFFu;\n"
            "    if (e == 0 && off == 0x14u) {  /* docs/342, 343, 348 */\n"
            "        lb_evt(2);\n"
            "        lb_pair_on_bg();\n"
            "        lb_tag_on_bg1();\n"
            "    }\n"
            "    /* docs/348: BG2's scroll is written as its fog staging is\n"
            "       committed -- take the fog record that staging came from. */\n"
            "    if (e == 0 && off == 0x18u) lb_tag_on_bg2();\n")
    done.append("gpu2d: trace the scanline of each BG1HOFS write")
    # docs/347 item 5: command sprites drew beneath the fog, unlike vanilla.
    # The icons are OAM sprites; whether the shroud (BG2) SHOULD cover them is
    # decided by their OAM priority against BG2's. Print every engine-A
    # sprite once per frame (at its first row) in the widened composite, with
    # the BG priorities, so the icon's priority is read rather than assumed.
    replace(gpu,
            "            if (output_sx<=-bw || output_sx>=out_width) continue;\n",
            "            if (output_sx<=-bw || output_sx>=out_width) continue;\n"
            "            if (engine == 0 && out_width > 256 && row == 0 &&\n"
            "                lb_oam_trace_on()) {   /* legobattles-decomp */\n"
            "                std::fprintf(stderr,\n"
            "                    \"[lb_oam] frame %u n %d sx %d out %d sy %d \"\n"
            "                    \"prio %d tile %u pal %u %dx%d mode %d \"\n"
            "                    \"bgprio %u,%u,%u,%u\\n\",\n"
            "                    lb_dump_frame_get(), n, sx, output_sx, sy,\n"
            "                    priority, (unsigned)(a2 & 0x3FFu),\n"
            "                    (unsigned)((a2 >> 12) & 0xFu), w, h,\n"
            "                    (int)((a0 >> 10) & 3),\n"
            "                    (unsigned)(u.bgcnt[0] & 3u),\n"
            "                    (unsigned)(u.bgcnt[1] & 3u),\n"
            "                    (unsigned)(u.bgcnt[2] & 3u),\n"
            "                    (unsigned)(u.bgcnt[3] & 3u));\n"
            "            }\n")
    done.append("gpu2d: trace engine-A sprites in the widened composite")

    #     The presentation texture has to be able to HOLD the extra rows. It is
    #     created once, at kScreenHeight, so without this the compositor could
    #     produce more rows than would ever be uploaded. 512 is
    #     kMaxAdaptiveHeight in gpu2d.cpp and the height lb_fit_texture
    #     re-creates at; ALL THREE must agree. They did not: this said 256,
    #     lb_fit_texture said kScreenHeight (192), and the surface clamp said
    #     512, so whichever ran last decided what the player saw.
    replace(front,
            "            presentation.screen_widths[screen], kScreenHeight);",
            "            presentation.screen_widths[screen], 768);"
            "  /* legobattles-decomp: kMaxAdaptiveHeight */")
    done.append("frontend.cpp: presentation texture holds the extra rows")


    # 5a2. Let serve mode widen, so widescreen is verifiable headlessly.
    #
    #      nds_gpu3d_set_output_width is called only from frontend.cpp, so under
    #      --serve the adaptive framebuffer is always 256 wide and every widening
    #      path returns before it runs. That made widescreen verifiable ONLY in
    #      the interactive window -- where run_cycles is refused, because the
    #      frontend owns execution, so tools/lb_warp.py cannot reach gameplay.
    #      The two halves of the harness could not be used together.
    edit(mainc, "    if (serve) {",
         ['        {   /* legobattles-decomp: LB_FORCE_WIDTH=448 */',
          '            if (const char* w = std::getenv("LB_FORCE_WIDTH")) {',
          '                const int v = std::atoi(w);',
          '                if (v >= 256 && v <= 1024 && (v & 1) == 0) {',
          '                    /* an accelerated renderer refuses a resize; */',
          '                    /* serve mode has no GL surface anyway. */',
          '                    nds_gpu3d_use_soft_renderer(false);',
          '                    if (!nds_gpu3d_set_output_width(',
          '                            static_cast<uint16_t>(v)))',
          '                        std::fprintf(stderr,',
          '                            "[lb] LB_FORCE_WIDTH %d refused\\n", v);',
          '                    else',
          '                        std::fprintf(stderr,',
          '                            "[lb] serve output width %d\\n", v);',
          '                }',
          '            }',
          '        }'])
    edit(mainc, "    if (serve) {",
         ['        {   /* legobattles-decomp: LB_BG_EXTEND=1 */',
          '            /* Let the furthest-back text BG reach the widened ',
          '               columns. Upstream provides this and nothing calls it. ',
          '               docs/156: BG1 is the terrain here and IS furthest ',
          '               back at priority 3, so the new space is a ',
          '               continuation of it. */',
          '            if (std::getenv("LB_BG_EXTEND"))',
          '                nds_gpu2d_set_adaptive_bg_extend(true);',
          '        }'])
    done.append("main.cpp: LB_BG_EXTEND widens the backmost BG")
    done.append("main.cpp: LB_FORCE_WIDTH for headless widescreen")

    # 5c. COUNT THE FRAMES THE LOCAL-WIRELESS BRIDGE ACTUALLY CARRIES.
    #
    #     Local multiplayer. ndsrecomp's LocalMpTransport already gives each
    #     instance a loopback UDP socket and broadcasts the guest's Wi-Fi
    #     frames to the other instances, and it logs "[local_mp] enabled" when
    #     the socket opens. That says the SOCKET opened. It does not say the
    #     guest ever spoke, and the difference is the whole question: if the
    #     cartridge's own lobby cannot find a peer over this channel, no
    #     address from the master server is any use.
    #
    #     Counting sends and receives separately is what separates the three
    #     failures that look alike from the outside -- nobody transmitting,
    #     one side transmitting into silence, and both transmitting but the
    #     game not recognising what arrives.
    wifi = os.path.join(runner_dir, "src", "wifi_net.cpp")
    replace(wifi,
            "    int SendGeneric(uint32_t type, uint8_t* data, int len,\n"
            "                    uint64_t timestamp) {\n"
            "        if (!enabled_) return 0;",
            "    /* legobattles-decomp: did anything actually cross? */\n"
            "    void LbCount(int sent, int recvd) {\n"
            "        lb_sent_ += (sent > 0) ? 1u : 0u;\n"
            "        lb_recv_ += (recvd > 0) ? 1u : 0u;\n"
            "        const unsigned total = lb_sent_ + lb_recv_;\n"
            "        if (total && (total % 50u) == 0u && total != lb_said_) {\n"
            "            lb_said_ = total;\n"
            "            melonDS::Platform::Log(melonDS::Platform::Info,\n"
            "                \"[local_mp] frames sent=%u recv=%u\\n\",\n"
            "                lb_sent_, lb_recv_);\n"
            "        }\n"
            "    }\n"
            "    unsigned lb_sent_ = 0, lb_recv_ = 0, lb_said_ = 0;\n"
            "\n"
            "    int SendGeneric(uint32_t type, uint8_t* data, int len,\n"
            "                    uint64_t timestamp) {\n"
            "        if (!enabled_) return 0;")
    replace(wifi,
            "            if (timestamp) *timestamp = frame.timestamp;\n"
            "            if (frame.type == 1u) last_host_id_ = static_cast<int>(frame.sender);",
            "            if (timestamp) *timestamp = frame.timestamp;\n"
            "            LbCount(0, 1);   /* legobattles-decomp */\n"
            "            if (frame.type == 1u) last_host_id_ = static_cast<int>(frame.sender);")
    replace(wifi,
            "        return sent;\n"
            "#else\n"
            "        (void)type;",
            "        LbCount(sent, 0);   /* legobattles-decomp */\n"
            "        return sent;\n"
            "#else\n"
            "        (void)type;")
    done.append("wifi_net.cpp: count the local-wireless frames that cross")

    # 5d. A REMOTE PEER FOR THE WIRELESS BRIDGE.
    #
    #     docs/216 got two instances into one live battle. The transport that
    #     carried it sends to 127.0.0.1:(base + i) for every other instance
    #     index, so it reaches other processes on this machine and nothing
    #     else. Two MACHINES need it to also send somewhere it is told.
    #
    #     That is the whole gap between "multiplayer works" and "multiplayer
    #     works over the internet", because the master server already knows how
    #     to hand a client the host's address -- it is a directory, and this is
    #     the thing that consumes a directory entry.
    #
    #     THE CLIENT KNOWS THE HOST; THE HOST LEARNS THE CLIENT. A client that
    #     picked a lobby has an address to send to. A host has none until
    #     someone talks to it, so it remembers the source of the first datagram
    #     that carries the right magic and a foreign instance index, and
    #     answers there. No rendezvous protocol, no second port, and nothing
    #     for the master server to relay.
    #
    #     THE BIND ADDRESS IS THE ONE REAL DECISION. Binding INADDR_ANY makes
    #     the socket reachable from the network, which the loopback default
    #     deliberately is not. So it stays loopback unless remote play is
    #     actually asked for -- a peer set, or --local-wireless-remote on --
    #     and the socket is reopened when that changes rather than being
    #     silently wrong.
    replace(wifi,
            "    bool enabled_ = false;\n"
            "    uint32_t instance_ = 0;\n"
            "    uint16_t base_port_ = 26710;",
            "    /* legobattles-decomp: remote peer. See apply_hooks.py 5d. */\n"
            "public:\n"
            "    void LbSetPeer(uint32_t ip_be, uint16_t port) {\n"
            "        const bool was_remote = LbRemote();\n"
            "        lb_peer_ip_ = ip_be;\n"
            "        lb_peer_port_ = port;\n"
            "#ifdef _WIN32\n"
            "        if (LbRemote() != was_remote && socket_ != INVALID_SOCKET) {\n"
            "            CloseSocket();\n"
            "            Begin();\n"
            "        }\n"
            "#endif\n"
            "    }\n"
            "    void LbSetListen(bool on) {\n"
            "        const bool was_remote = LbRemote();\n"
            "        lb_listen_ = on;\n"
            "#ifdef _WIN32\n"
            "        if (LbRemote() != was_remote && socket_ != INVALID_SOCKET) {\n"
            "            CloseSocket();\n"
            "            Begin();\n"
            "        }\n"
            "#endif\n"
            "    }\n"
            "    bool LbRemote() const { return lb_listen_ || lb_peer_ip_ != 0u; }\n"
            "    uint32_t LbPeerIp() const { return lb_peer_ip_; }\n"
            "    uint16_t LbPeerPort() const { return lb_peer_port_; }\n"
            "    bool LbPeerLearned() const { return lb_peer_learned_; }\n"
            "    uint16_t LbLocalPort() const {\n"
            "        return static_cast<uint16_t>(base_port_ + instance_);\n"
            "    }\n"
            "    bool LbEnabled() const { return enabled_; }\n"
            "private:\n"
    # A HELLO, because the client is PASSIVE while it scans.
    #
    # Measured: with the two instances on separate broadcast bases and the
    # client given the host's address, the client sat on "Searching for
    # Games..." and the host's `recv` stayed at 0. The host beacons; the client
    # listens. Learn-from-the-first-datagram cannot work when the side that
    # knows the address never sends anything.
    #
    # So the side that HAS a peer says hello to it -- a header with no payload
    # and a type the receiver uses only to learn an address and then drops. It
    # is rate-limited and it carries nothing, so it cannot be mistaken for a
    # game frame, and it stops mattering the moment real traffic starts.
            "    static constexpr uint32_t kLbHello = 0x4C42484Fu;  /* LBHO */\n"
            "    uint32_t lb_peer_ip_ = 0u;      /* network byte order */\n"
            "    uint16_t lb_peer_port_ = 0u;    /* host byte order */\n"
            "    bool lb_listen_ = false;\n"
            "    bool lb_peer_learned_ = false;\n"
            "    std::chrono::steady_clock::time_point lb_last_hello_{};\n"
            "\n"
            "    void LbMaybeHello() {\n"
            "#ifdef _WIN32\n"
            "        if (!enabled_ || socket_ == INVALID_SOCKET) return;\n"
            "        if (lb_peer_ip_ == 0u || lb_peer_port_ == 0u) return;\n"
            "        const auto now = std::chrono::steady_clock::now();\n"
            "        if (now - lb_last_hello_ < std::chrono::milliseconds(250))\n"
            "            return;\n"
            "        lb_last_hello_ = now;\n"
            "        std::vector<uint8_t> datagram;\n"
            "        WriteLe32(&datagram, kMagic);\n"
            "        WriteLe32(&datagram, instance_);\n"
            "        WriteLe32(&datagram, kLbHello);\n"
            "        WriteLe32(&datagram, 0u);\n"
            "        WriteLe64(&datagram, 0u);\n"
            "        sockaddr_in remote{};\n"
            "        remote.sin_family = AF_INET;\n"
            "        remote.sin_addr.s_addr = lb_peer_ip_;\n"
            "        remote.sin_port = htons(lb_peer_port_);\n"
            "        ::sendto(socket_,\n"
            "                 reinterpret_cast<const char*>(datagram.data()),\n"
            "                 static_cast<int>(datagram.size()), 0,\n"
            "                 reinterpret_cast<sockaddr*>(&remote),\n"
            "                 sizeof(remote));\n"
            "#endif\n"
            "    }\n"
            "\n"
            "    bool enabled_ = false;\n"
            "    uint32_t instance_ = 0;\n"
            "    uint16_t base_port_ = 26710;")
    replace(wifi,
            "        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);\n"
            "        addr.sin_port = htons(static_cast<uint16_t>(base_port_ + instance_));",
            "        /* legobattles-decomp: reachable from the network only when\n"
            "           remote play was actually asked for. */\n"
            "        addr.sin_addr.s_addr =\n"
            "            htonl(LbRemote() ? INADDR_ANY : INADDR_LOOPBACK);\n"
            "        addr.sin_port = htons(static_cast<uint16_t>(base_port_ + instance_));")
    replace(wifi,
            "        LbCount(sent, 0);   /* legobattles-decomp */\n"
            "        return sent;",
            "        /* legobattles-decomp: and to the remote peer, if there is\n"
            "           one. Same datagram, same header -- the receiver cannot\n"
            "           tell and does not need to.\n"
            "\n"
            "           Not when the peer is a SIBLING: the sweep above already\n"
            "           reached it, and a second copy of every frame is what a\n"
            "           client on the host's own machine got once the master\n"
            "           server handed it the host's loopback address. */\n"
            "        if (lb_peer_ip_ != 0u && lb_peer_port_ != 0u &&\n"
            "            !(lb_peer_ip_ == htonl(INADDR_LOOPBACK) &&\n"
            "              lb_peer_port_ >= base_port_ &&\n"
            "              lb_peer_port_ < base_port_ + kMaxInstances)) {\n"
            "            sockaddr_in remote{};\n"
            "            remote.sin_family = AF_INET;\n"
            "            remote.sin_addr.s_addr = lb_peer_ip_;\n"
            "            remote.sin_port = htons(lb_peer_port_);\n"
            "            if (::sendto(socket_,\n"
            "                         reinterpret_cast<const char*>(datagram.data()),\n"
            "                         static_cast<int>(datagram.size()), 0,\n"
            "                         reinterpret_cast<sockaddr*>(&remote),\n"
            "                         sizeof(remote)) != SOCKET_ERROR)\n"
            "                sent = len;\n"
            "        }\n"
            "        LbCount(sent, 0);   /* legobattles-decomp */\n"
            "        return sent;")
    # Send the hello from the drain, which is the one place that runs whenever
    # the guest is doing wireless at all -- including while it is only
    # LISTENING, which is exactly the case that needed it.
    replace(wifi,
            "        if (!enabled_ || socket_ == INVALID_SOCKET) return;\n"
            "        for (;;) {\n"
            "            uint8_t datagram[kHeaderBytes + kMaxFrameBytes];",
            "        if (!enabled_ || socket_ == INVALID_SOCKET) return;\n"
            "        LbMaybeHello();   /* legobattles-decomp */\n"
            "        for (;;) {\n"
            "            uint8_t datagram[kHeaderBytes + kMaxFrameBytes];")
    # The magic check appears twice -- LocalMpTransport and WfcPeer -- so the
    # anchor carries the line after it, which is LocalMpFrame's and only its.
    replace(wifi,
            "            if (ReadLe32(datagram) != kMagic) continue;\n"
            "\n"
            "            LocalMpFrame frame;",
            "            if (ReadLe32(datagram) != kMagic) continue;\n"
            "            /* legobattles-decomp: a HOST has no address to answer\n"
            "               until someone talks to it. Take the source of the\n"
            "               first well-formed foreign datagram, checked AFTER\n"
            "               the magic so stray traffic on the port cannot\n"
            "               capture the slot.\n"
            "\n"
            "               NOT \"anything but loopback\". The exclusion has to be\n"
            "               the sibling instances this transport ALREADY reaches\n"
            "               by broadcasting to base..base+15 -- learning one of\n"
            "               those as a peer would duplicate every frame to it.\n"
            "               Excluding loopback wholesale would also make the\n"
            "               remote path untestable on one machine, which is the\n"
            "               only place it can be tested here. */\n"
            "            {\n"
            "                const uint16_t src_port = ntohs(from.sin_port);\n"
            "                const bool sibling =\n"
            "                    from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&\n"
            "                    src_port >= base_port_ &&\n"
            "                    src_port < base_port_ + kMaxInstances;\n"
            "            if (lb_listen_ && lb_peer_ip_ == 0u && !sibling) {\n"
            "                lb_peer_ip_ = from.sin_addr.s_addr;\n"
            "                lb_peer_port_ = ntohs(from.sin_port);\n"
            "                lb_peer_learned_ = true;\n"
            "                melonDS::Platform::Log(melonDS::Platform::Info,\n"
            "                    \"[local_mp] peer learned %u.%u.%u.%u:%u\\n\",\n"
            "                    (unsigned)(lb_peer_ip_ & 0xFFu),\n"
            "                    (unsigned)((lb_peer_ip_ >> 8) & 0xFFu),\n"
            "                    (unsigned)((lb_peer_ip_ >> 16) & 0xFFu),\n"
            "                    (unsigned)((lb_peer_ip_ >> 24) & 0xFFu),\n"
            "                    (unsigned)lb_peer_port_);\n"
            "            }\n"
            "            }\n"
            "            /* A hello carries nothing and exists only so the other\n"
            "               side has an address to answer. Never a game frame. */\n"
            "            if (ReadLe32(datagram + 8u) == kLbHello) continue;\n"
            "\n"
            "            LocalMpFrame frame;")
    # OUR OWN FRAME IS ONE FROM OUR OWN PORT. Upstream drops
    # every datagram whose sender index equals this instance's. Between
    # processes on one machine and one base that is the same thing -- two of
    # them cannot hold the same port -- but between machines it is not: each
    # numbers its instances from 0, so two default launches share index 0 and
    # each dropped every frame the other sent. What only our own echo has is
    # our own source address, so that is the test now. The learning test above
    # got the same correction (it refused a peer whose index matched ours, so a
    # default host never learned a default client from another machine); there
    # the sibling test already excludes our own port.
    replace(wifi,
            "            if (frame.sender == instance_) continue;\n"
            "            if (frame.sender >= kMaxInstances) continue;",
            "            /* legobattles-decomp (5d): our own frame is one from our\n"
            "               own port. Another machine numbers its instances from 0\n"
            "               as this one does, so an equal index proves nothing. */\n"
            "            if (frame.sender == instance_ &&\n"
            "                from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&\n"
            "                ntohs(from.sin_port) == base_port_ + instance_) continue;\n"
            "            if (frame.sender >= kMaxInstances) continue;")
    replace(wifi,
            "void MP_Begin(void* userdata) {\n"
            "    (void)userdata;\n"
            "    if (g_bridge) g_bridge->local_mp.Begin();\n"
            "}",
            "/* legobattles-decomp: remote peer control, for lb_net.cpp and the\n"
            "   Multiplayer tab. Safe to call before the bridge exists -- the\n"
            "   request is remembered and applied at attach. */\n"
            "namespace {\n"
            "uint32_t g_lb_pending_peer_ip = 0u;\n"
            "uint16_t g_lb_pending_peer_port = 0u;\n"
            "bool g_lb_pending_listen = false;\n"
            "bool g_lb_pending_valid = false;\n"
            "}\n"
            "\n"
            "extern \"C\" void nds_local_wireless_set_peer(uint32_t ip_be,\n"
            "                                            uint16_t port) {\n"
            "    g_lb_pending_peer_ip = ip_be;\n"
            "    g_lb_pending_peer_port = port;\n"
            "    g_lb_pending_valid = true;\n"
            "    if (g_bridge) g_bridge->local_mp.LbSetPeer(ip_be, port);\n"
            "}\n"
            "\n"
            "extern \"C\" void nds_local_wireless_set_listen(int on) {\n"
            "    g_lb_pending_listen = on != 0;\n"
            "    g_lb_pending_valid = true;\n"
            "    if (g_bridge) g_bridge->local_mp.LbSetListen(on != 0);\n"
            "}\n"
            "\n"
            "extern \"C\" int nds_local_wireless_peer(uint32_t* ip_be,\n"
            "                                       uint16_t* port,\n"
            "                                       int* learned) {\n"
            "    if (!g_bridge) return 0;\n"
            "    if (ip_be) *ip_be = g_bridge->local_mp.LbPeerIp();\n"
            "    if (port) *port = g_bridge->local_mp.LbPeerPort();\n"
            "    if (learned) *learned = g_bridge->local_mp.LbPeerLearned() ? 1 : 0;\n"
            "    return g_bridge->local_mp.LbPeerIp() != 0u ? 1 : 0;\n"
            "}\n"
            "\n"
            "/* The UDP port this instance listens on, so a host can advertise\n"
            "   something a client can actually send to. 0 when local wireless\n"
            "   is off, which is what the UI needs to say so. */\n"
            "extern \"C\" uint16_t nds_local_wireless_port(void) {\n"
            "    if (!g_bridge || !g_bridge->local_mp.LbEnabled()) return 0;\n"
            "    return g_bridge->local_mp.LbLocalPort();\n"
            "}\n"
            "\n"
            "/* legobattles-decomp (apply_hooks.py 5f): the first free instance\n"
            "   index, for a launch that named none. Two instances on one\n"
            "   machine must differ in both the UDP port (base + index) and the\n"
            "   guest MAC (perturbed by index), and the port is only bound once\n"
            "   the guest powers its radio on, so it cannot be probed at launch.\n"
            "   A named mutex per index is held instead, for the life of the\n"
            "   process: the OS releases it however the process ends. */\n"
            "extern \"C\" uint32_t nds_local_wireless_claim_instance(void) {\n"
            "#ifdef _WIN32\n"
            "    for (uint32_t i = 0; i < 16u; ++i) {\n"
            "        char name[64];\n"
            "        std::snprintf(name, sizeof(name),\n"
            "                      \"Local\\\\ndsrecomp-local-wireless-instance-%u\",\n"
            "                      static_cast<unsigned>(i));\n"
            "        HANDLE claim = CreateMutexA(nullptr, FALSE, name);\n"
            "        if (!claim) continue;\n"
            "        if (GetLastError() == ERROR_ALREADY_EXISTS) {\n"
            "            CloseHandle(claim);\n"
            "            continue;\n"
            "        }\n"
            "        return i;   /* held, never closed */\n"
            "    }\n"
            "#endif\n"
            "    return 0u;\n"
            "}\n"
            "\n"
            "void MP_Begin(void* userdata) {\n"
            "    (void)userdata;\n"
            "    if (g_bridge) {\n"
            "        /* legobattles-decomp: apply anything asked for before the\n"
            "           bridge existed. */\n"
            "        if (g_lb_pending_valid) {\n"
            "            g_bridge->local_mp.LbSetListen(g_lb_pending_listen);\n"
            "            if (g_lb_pending_peer_ip)\n"
            "                g_bridge->local_mp.LbSetPeer(g_lb_pending_peer_ip,\n"
            "                                             g_lb_pending_peer_port);\n"
            "        }\n"
            "        g_bridge->local_mp.Begin();\n"
            "    }\n"
            "}")
    done.append("wifi_net.cpp: a remote peer for the local-wireless bridge")

    # 5f. A FREE INSTANCE INDEX WHEN THE LAUNCH NAMED NONE.
    #
    #     Two copies started the same way -- e.g. play.bat run
    #     twice -- both took instance 0: the same UDP port
    #     (base + index, so the second bind fails) and the same guest MAC
    #     (nds_apply_instance_mac perturbs it by index; index 0 is the
    #     install's own identity for both). No wireless session can form
    #     between two consoles with one MAC. The port cannot be probed at
    #     launch -- it is bound only when the guest powers its radio on -- so
    #     each index is claimed with a named mutex instead
    #     (nds_local_wireless_claim_instance, 5d), held for the life of the
    #     process. An explicit --instance-index, which tools/lb_mp.py always
    #     passes, is left exactly as given.
    edit(mainc, '#include "wifi_net.h"',
         ['/* legobattles-decomp: apply_hooks.py 5f. */',
          'extern "C" uint32_t nds_local_wireless_claim_instance(void);'])
    edit(mainc, "// Resolve the network config to backend-ready numeric form.",
         ['/* legobattles-decomp (apply_hooks.py 5f): a launch that named no',
          '   instance takes the first free one, so two copies started the',
          '   same way get different ports and different guest MACs. */',
          'if (frontend_options.local_wireless.enabled &&',
          '    cli_instance_index.empty() &&',
          '    frontend_options.instance_index == 0u) {',
          '    frontend_options.instance_index =',
          '        nds_local_wireless_claim_instance();',
          '    std::fprintf(stderr,',
          '        "[local_mp] instance index %u (the first free one)\\n",',
          '        static_cast<unsigned>(frontend_options.instance_index));',
          '}'],
         after=False)
    done.append("main.cpp: a free local-wireless instance when none is named")

    # 5e. A DEBUG CLICK IN WINDOW PIXELS.
    #
    #     nds_frontend_debug_touch takes GUEST coordinates and refuses anything
    #     outside 0..255 / 0..191, then converts to window pixels through the
    #     VANILLA stacked layout:
    #
    #         if (!g_input_debug.active || x >= kScreenWidth ||
    #             y >= kScreenHeight) return false;
    #         event.button.x = (bottom_content_left + x) * kWindowScale;
    #
    #     Both halves are wrong for the one question the enhanced layout exists
    #     to answer. A click in a WIDENED column has no guest x inside 0..255 by
    #     definition, so it is rejected; and a click that is accepted is placed
    #     by a layout the enhanced path does not use.
    #
    #     THIS IS NOT A DETAIL. tools/lb_exp.py's --click-ext has been passing
    #     window pixels to it -- 412, 421 -- which are simply refused, so every
    #     click in that probe did nothing at all, INCLUDING ITS OWN CONTROL.
    #     "Clicking a unit outside the native band does not select it" was never
    #     measured; a probe that cannot click was.
    #
    #     A motion event goes first because ImGui tracks the cursor from motion,
    #     and lb_overlay_event swallows a click it believes is over a panel. A
    #     button event with no motion before it is judged against wherever the
    #     cursor last was.
    replace(front,
            "bool nds_frontend_debug_capture_mouse() {",
            "/* legobattles-decomp: a click in WINDOW pixels, pushed unchanged,\n"
            "   so the enhanced layout's own mapping is what gets exercised. */\n"
            "extern \"C\" bool nds_frontend_debug_click(int x, int y, bool down) {\n"
            "#if defined(NDS_HAVE_SDL2)\n"
            "    if (!g_input_debug.active) return false;\n"
            "    SDL_Event motion{};\n"
            "    motion.type = SDL_MOUSEMOTION;\n"
            "    motion.motion.windowID = g_input_debug.bottom_window_id;\n"
            "    motion.motion.x = x;\n"
            "    motion.motion.y = y;\n"
            "    SDL_PushEvent(&motion);\n"
            "    SDL_Event event{};\n"
            "    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;\n"
            "    event.button.windowID = g_input_debug.bottom_window_id;\n"
            "    event.button.button = SDL_BUTTON_LEFT;\n"
            "    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;\n"
            "    event.button.clicks = 1;\n"
            "    event.button.x = x;\n"
            "    event.button.y = y;\n"
            "    const bool pushed = SDL_PushEvent(&event) == 1;\n"
            "    if (pushed) ++g_input_debug.debug_touch_events;\n"
            "    else ++g_input_debug.debug_event_errors;\n"
            "    return pushed;\n"
            "#else\n"
            "    (void)x; (void)y; (void)down;\n"
            "    return false;\n"
            "#endif\n"
            "}\n"
            "\n"
            "/* legobattles-decomp: motion alone, in window pixels. A DRAG is a\n"
            "   button-down, a path, and a button-up, and the frontend only\n"
            "   updates the guest touch on motion while the button is held --\n"
            "   so without this a synthesised drag is two clicks at the ends\n"
            "   and nothing in between. */\n"
            "extern \"C\" bool nds_frontend_debug_move(int x, int y) {\n"
            "#if defined(NDS_HAVE_SDL2)\n"
            "    if (!g_input_debug.active) return false;\n"
            "    SDL_Event motion{};\n"
            "    motion.type = SDL_MOUSEMOTION;\n"
            "    motion.motion.windowID = g_input_debug.bottom_window_id;\n"
            "    motion.motion.state = SDL_BUTTON_LMASK;\n"
            "    motion.motion.x = x;\n"
            "    motion.motion.y = y;\n"
            "    return SDL_PushEvent(&motion) == 1;\n"
            "#else\n"
            "    (void)x; (void)y;\n"
            "    return false;\n"
            "#endif\n"
            "}\n"
            "\n"
            "bool nds_frontend_debug_capture_mouse() {")
    dbgsrv = os.path.join(runner_dir, "src", "debug_server.cpp")
    # At FILE scope, and extern "C". Declaring it inside the handler put it in
    # the enclosing anonymous namespace and the link failed on a mangled name
    # that nothing defines.
    replace(dbgsrv,
            "extern \"C\" uint32_t g_runtime_break_pc;",
            "extern \"C\" uint32_t g_runtime_break_pc;\n"
            "extern \"C\" bool nds_frontend_debug_click(int x, int y, bool down);\n"
            "extern \"C\" bool nds_frontend_debug_move(int x, int y);\n"
            "extern \"C\" void lb_watch_set_range(uint32_t lo, uint32_t hi);\n"
            "extern \"C\" void lb_watch_set_val(long long v);\n"
            "extern \"C\" void lb_watch_clear(void);")
    # `watch_range` -- point the memory watch somewhere while the game runs.
    #
    # LB_WATCH is parsed once at startup, which cannot express "the entity the
    # harness just found", because that address is reached through a pool
    # pointer and only exists once a battle is running.
    replace(dbgsrv,
            "    if (cmd == \"watch\") {",
            "    if (cmd == \"watch_clear\") {  /* legobattles-decomp */\n"
            "        lb_watch_clear();\n"
            "        return \"{\\\"ok\\\":true}\";\n"
            "    }\n"
            "\n"
            "    if (cmd == \"watch_val\") {  /* legobattles-decomp */\n"
            "        lb_watch_set_val((long long)json_u64(line, \"value\", 0));\n"
            "        return \"{\\\"ok\\\":true}\";\n"
            "    }\n"
            "\n"
            "    if (cmd == \"watch_range\") {  /* legobattles-decomp */\n"
            "        const uint32_t lo = (uint32_t)json_u64(line, \"lo\", 0);\n"
            "        const uint32_t hi = (uint32_t)json_u64(line, \"hi\", 0);\n"
            "        lb_watch_set_range(lo, hi);\n"
            "        return \"{\\\"ok\\\":true}\";\n"
            "    }\n"
            "\n"
            "    if (cmd == \"watch\") {")
    replace(dbgsrv,
            "        } else if (action == \"touch\") {",
            "        } else if (action == \"moveto\") {\n"
            "            /* legobattles-decomp: absolute motion, for a DRAG. */\n"
            "            ok = nds_frontend_debug_move(\n"
            "                static_cast<int>(json_i64(line, \"x\", 0)),\n"
            "                static_cast<int>(json_i64(line, \"y\", 0)));\n"
            "        } else if (action == \"click\") {\n"
            "            /* legobattles-decomp: WINDOW pixels, signed, pushed\n"
            "               unchanged -- see apply_hooks.py section 5e. */\n"
            "            ok = nds_frontend_debug_click(\n"
            "                static_cast<int>(json_i64(line, \"x\", 0)),\n"
            "                static_cast<int>(json_i64(line, \"y\", 0)),\n"
            "                json_bool(line, \"down\", true));\n"
            "        } else if (action == \"touch\") {")
    done.append("frontend.cpp: a debug click in window pixels")

    done.append("bus.cpp: LB_WATCH configurable watchpoint")

    # 5b. Let an override call the function it replaced.
    edit(rtarm, "extern \"C\" void nds_register_dispatch(int cpu, const DispatchEntry* t,",
         ['',
          '// legobattles-decomp: the ORIGINAL implementation of an address, skipping',
          '// overrides.',
          '//',
          '// Banks are searched first-match-first and nds_register_dispatch appends, so',
          '// an override registered ahead of the title banks wins -- which is how this',
          '// port changes behaviour at all. The cost is that an override cannot then',
          '// call what it replaced, which leaves only two options: reimplement the',
          '// function, which for anything non-trivial is guessing at the',
          "// cartridge's rules, or do not ship the feature. Several",
          '// gameplay options were stuck on that (docs/108).',
          '//',
          '// The original is simply the LAST bank carrying the address, because the',
          '// title banks register after every override.',
          '//',
          '// MEMOISED (docs/349). The scan below walks every row of every bank, and',
          '// the banks carry one row per guest INSTRUCTION -- 409,316 in the ARM9',
          '// title bank alone, ~10 MB of rows -- while the overrides ask on every',
          '// call they make. An LB_SAMPLE profile of a battle found 26% of the main',
          '// thread inside this loop, most of it for gfx_transfer_observer,',
          '// heal_tick_override and set_hp_override. The answer depends only on',
          '// which banks are registered (the tables are const), so it is kept per',
          '// (cpu, addr) and dropped whenever the bank list changes: the',
          '// generation is bumped by nds_register_dispatch and by runtime_init\'s',
          '// clear.',
          'uint32_t g_lb_dispatch_gen = 1u;',
          'extern "C" void (*lb_dispatch_original(int cpu, uint32_t addr))(void) {',
          '    struct Memo { uint32_t gen, addr; int cpu; void (*fn)(void); };',
          '    static Memo memo[64];',
          '    static unsigned memo_n = 0u;',
          '    cpu &= 1;',
          '    for (unsigned k = 0; k < memo_n; ++k)',
          '        if (memo[k].addr == addr && memo[k].cpu == cpu &&',
          '            memo[k].gen == g_lb_dispatch_gen)',
          '            return memo[k].fn;',
          '    const CpuCtx& c = g_ctx[cpu];',
          '    void (*found)(void) = nullptr;',
          '    for (const DispatchBank& bank : c.banks)',
          '        for (unsigned i = 0; i < bank.len; ++i)',
          '            if (bank.table[i].addr == addr && bank.table[i].fn)',
          '                found = bank.table[i].fn;',
          '    unsigned slot = memo_n;',
          '    for (unsigned k = 0; k < memo_n; ++k)',
          '        if (memo[k].addr == addr && memo[k].cpu == cpu) { slot = k; break; }',
          '    if (slot == memo_n) {',
          '        if (memo_n < sizeof memo / sizeof memo[0]) ++memo_n;',
          '        else slot = addr % memo_n;  /* full: evicting costs one rescan */',
          '    }',
          '    memo[slot] = Memo{g_lb_dispatch_gen, addr, cpu, found};',
          '    return found;',
          '}',
          ''],
         after=False)
    replace(rtarm,
            "    ctx.banks.push_back({t, len});\n",
            "    ctx.banks.push_back({t, len});\n"
            "    ++g_lb_dispatch_gen;  /* legobattles-decomp: see lb_dispatch_original */\n")
    replace(rtarm,
            "        ctx.banks.clear();\n",
            "        ctx.banks.clear();\n"
            "        ++g_lb_dispatch_gen;  /* legobattles-decomp: see lb_dispatch_original */\n")
    done.append("runtime_arm.cpp: lb_dispatch_original trampoline (memoised)")

    # 5b2. LB_COUNTS_AT=N[,N...]: the guest's retired-instruction and cycle
    #      totals at chosen PRESENTED frames (docs/349).
    #
    #      The exactness gate for any change that is meant to make the
    #      emulator faster without changing what it computes -- a cheaper path
    #      through the per-instruction timing helpers, say. The frontend runs
    #      exactly one emulated frame per present and a demo replays its input
    #      by frame number, so two builds whose guest execution is identical
    #      print identical lines. The instruction counts are the sensitive
    #      part: the cycle totals are pinned to the frame boundary by the
    #      scheduler and move only by one instruction's overshoot, while a
    #      changed timing model changes how much guest code has retired by
    #      then. event_counts.json cannot do this: the harness reads it at a
    #      wall-clock moment, a different frame every run. Only frames inside
    #      the demo count -- after its last event the harness starts probing
    #      the camera at wall-clock moments, and two runs part (docs/349).
    edit(front, "        ++shown_frames;",
         ["{   /* legobattles-decomp docs/349: LB_COUNTS_AT */",
          "    struct LbCountsAt { uint64_t at[16]; int n; };",
          "    static const LbCountsAt lb_counts_at = [] {",
          "        LbCountsAt v{};",
          "        const char* s = std::getenv(\"LB_COUNTS_AT\");",
          "        while (s && *s && v.n < 16) {",
          "            char* end = nullptr;",
          "            const unsigned long long n = std::strtoull(s, &end, 10);",
          "            if (end == s) break;",
          "            v.at[v.n++] = n;",
          "            if (*end != ',') break;",
          "            s = end + 1;",
          "        }",
          "        return v;",
          "    }();",
          "    for (int lb_i = 0; lb_i < lb_counts_at.n; ++lb_i)",
          "        if (shown_frames == lb_counts_at.at[lb_i])",
          "            std::fprintf(stderr, \"[lb_counts] frame %llu insn9 %llu \"",
          "                         \"insn7 %llu cyc9 %llu cyc7 %llu\\n\",",
          "                         (unsigned long long)shown_frames,",
          "                         (unsigned long long)g_insn_count[0],",
          "                         (unsigned long long)g_insn_count[1],",
          "                         (unsigned long long)scheduler_cpu_cycles(0),",
          "                         (unsigned long long)scheduler_cpu_cycles(1));",
          "}"],
         after=True)
    done.append("frontend.cpp: LB_COUNTS_AT exactness gate")

    # 5b3. Frame-time spikes in the live stats (docs/349).
    #
    #      fps and the per-phase means say whether the AVERAGE frame fits;
    #      "60 fps stability" is about the ones that do not. Each frame's WORK
    #      -- emulation plus presentation, excluding the audio-pacing wait,
    #      which is idle by definition -- is binned into cumulative counters of
    #      frames longer than 16.7 / 20 / 25 / 33.3 / 50 ms. Cumulative so a
    #      client diffs two queries over its own window like every other field
    #      here; no max, because a max would need a reset that the other
    #      nds_frontend_live_stats callers (framebuffer_sync) would trigger.
    fh = os.path.join(runner_dir, "src", "frontend.h")
    replace(fh,
            "    uint64_t underruns;       // audio underruns so far\n",
            "    uint64_t underruns;       // audio underruns so far\n"
            "    /* legobattles-decomp docs/349: frames whose work (emulate +\n"
            "       present, not the pacing wait) ran longer than these. */\n"
            "    uint64_t work_over_16ms;  // one 60 Hz period, 16.67 ms\n"
            "    uint64_t work_over_20ms;\n"
            "    uint64_t work_over_25ms;\n"
            "    uint64_t work_over_33ms;\n"
            "    uint64_t work_over_50ms;\n"
            "    /* docs/349: PRESENT CADENCE -- the interval between this\n"
            "       frame's present and the previous one's, in counter\n"
            "       ticks: how many, their sum and sum of squares (a client\n"
            "       derives the window's mean and spread from two queries),\n"
            "       and how many ran past 20 / 25 ms. Steady motion needs\n"
            "       steady presents, not only enough of them. */\n"
            "    uint64_t present_intervals;\n"
            "    uint64_t present_interval_sum;\n"
            "    uint64_t present_interval_sq;\n"
            "    uint64_t present_gap_20ms;\n"
            "    uint64_t present_gap_25ms;\n")
    replace(front,
            "        phase_present_ticks += SDL_GetPerformanceCounter() - phase1;\n",
            "        phase_present_ticks += SDL_GetPerformanceCounter() - phase1;\n"
            "        {   /* legobattles-decomp docs/349: per-frame work time */\n"
            "            /* Emulation plus presentation: this frame's share of\n"
            "               phase_emu_ticks (cumulative, never reset) and the\n"
            "               present phase. NOT now - phase0, which would also\n"
            "               count the LB_PACE wait between the two (5b4). */\n"
            "            static uint64_t lb_emu_prev = 0;\n"
            "            const uint64_t lb_emu_frame = phase_emu_ticks - lb_emu_prev;\n"
            "            lb_emu_prev = phase_emu_ticks;\n"
            "            const uint64_t lb_work =\n"
            "                (SDL_GetPerformanceCounter() - phase1) + lb_emu_frame;\n"
            "            if (lb_work * 60u > frequency) ++g_live_stats.work_over_16ms;\n"
            "            if (lb_work * 50u > frequency) ++g_live_stats.work_over_20ms;\n"
            "            if (lb_work * 40u > frequency) ++g_live_stats.work_over_25ms;\n"
            "            if (lb_work * 30u > frequency) ++g_live_stats.work_over_33ms;\n"
            "            if (lb_work * 20u > frequency) ++g_live_stats.work_over_50ms;\n"
            "            static uint64_t lb_last_present = 0;\n"
            "            const uint64_t lb_now = SDL_GetPerformanceCounter();\n"
            "            if (lb_last_present) {\n"
            "                const uint64_t lb_iv = lb_now - lb_last_present;\n"
            "                ++g_live_stats.present_intervals;\n"
            "                g_live_stats.present_interval_sum += lb_iv;\n"
            "                g_live_stats.present_interval_sq += lb_iv * lb_iv;\n"
            "                if (lb_iv * 50u > frequency) ++g_live_stats.present_gap_20ms;\n"
            "                if (lb_iv * 40u > frequency) ++g_live_stats.present_gap_25ms;\n"
            "            }\n"
            "            lb_last_present = lb_now;\n"
            "        }\n")
    dbg_cpp = os.path.join(runner_dir, "src", "debug_server.cpp")
    replace(dbg_cpp,
            "               \",\\\"underruns\\\":\" + std::to_string(s.underruns) + \"}\";\n",
            "               \",\\\"underruns\\\":\" + std::to_string(s.underruns) +\n"
            "               /* legobattles-decomp docs/349 */\n"
            "               \",\\\"work_over_16ms\\\":\" + std::to_string(s.work_over_16ms) +\n"
            "               \",\\\"work_over_20ms\\\":\" + std::to_string(s.work_over_20ms) +\n"
            "               \",\\\"work_over_25ms\\\":\" + std::to_string(s.work_over_25ms) +\n"
            "               \",\\\"work_over_33ms\\\":\" + std::to_string(s.work_over_33ms) +\n"
            "               \",\\\"work_over_50ms\\\":\" + std::to_string(s.work_over_50ms) +\n"
            "               \",\\\"present_intervals\\\":\" + std::to_string(s.present_intervals) +\n"
            "               \",\\\"present_interval_sum\\\":\" + std::to_string(s.present_interval_sum) +\n"
            "               \",\\\"present_interval_sq\\\":\" + std::to_string(s.present_interval_sq) +\n"
            "               \",\\\"present_gap_20ms\\\":\" + std::to_string(s.present_gap_20ms) +\n"
            "               \",\\\"present_gap_25ms\\\":\" + std::to_string(s.present_gap_25ms) + \"}\";\n")
    done.append("frontend: per-frame work-time spike counters (docs/349)")

    # 5b4. PACE THE PRESENTS ON A CLOCK, NOT ON THE AUDIO CALLBACK (docs/349).
    #
    #      The loop emulates a frame, presents it, then drain_audio sleeps
    #      until the audio queue falls to its floor. The queue only falls when
    #      the SDL audio callback takes a block, and the device is opened with
    #      1024-frame blocks at 32,728 Hz: one every 31.3 ms, while a DS frame
    #      is 16.7 ms and makes ~547 frames of audio. So the loop is released
    #      once per callback and then runs TWO frames back to back. Measured
    #      from LB_SAMPLE profiles of both the harness pan and a recorded
    #      replay: the idle periods end at 28-32 ms intervals, almost never at
    #      ~17. The presents therefore come in pairs ~10 ms apart followed by a
    #      ~21 ms gap -- "60 fps" by count, and on a 60 Hz display the second
    #      of a pair often lands in the same refresh as the first, which the
    #      eye reads as a stuttering camera: motion looks uneven, as if the
    #      view were not actually moving smoothly.
    #
    #      LB_PACE=timer (the default, from docs/349's A/B; LB_PACE=audio
    #      restores the old loop exactly) waits for a steady deadline at the
    #      DS's own rate, 33,513,982 / 560,190 = 59.8261 Hz, BEFORE the
    #      composite, so the present follows a fixed period plus the (nearly
    #      constant) composite time. The audio queue still sets the long-run
    #      rate: the period is lengthened or shortened by up to 2% in
    #      proportion to how far the queue sits from a target one callback
    #      block above its floor -- the usual dynamic rate control, invisible
    #      -- and drain_audio's own wait is kept only as a 125 ms safety bound.
    #      Below the floor the loop presents at once and runs ahead, as the old
    #      loop did after a slow stretch; a frame that runs late is presented
    #      at once and the deadline restarts from it. Recorded replay: present
    #      interval sd 6.29 -> 0.69 ms, underruns 2 -> 0 (docs/349 section 9).
    #      Turbo and the boot prebuffer (audio not started) are unchanged.
    replace(front,
            "        const uint64_t phase1 = SDL_GetPerformanceCounter();\n",
            "        {   /* legobattles-decomp docs/349: LB_PACE */\n"
            "            static const bool lb_timer_pace = [] {\n"
            "                const char* v = std::getenv(\"LB_PACE\");\n"
            "                return !(v && std::strcmp(v, \"audio\") == 0);\n"
            "            }();\n"
            "            static uint64_t lb_deadline = 0;\n"
            "            if (lb_timer_pace && audio && audio_started &&\n"
            "                !turbo_active) {\n"
            "                const double lb_p0 = (double)frequency *\n"
            "                                     560190.0 / 33513982.0;\n"
            "                const double lb_q =\n"
            "                    (double)audio_queue_count(audio, audio_queue);\n"
            "                /* Aimed a whole callback block above the floor, so\n"
            "                   the level's low point (the device takes 1024\n"
            "                   frames at a time) stays at or above the floor.\n"
            "                   Up to 2% either way: invisible, and strong\n"
            "                   enough to hold the level. Half a block (the\n"
            "                   first revision) left 59 underruns in the\n"
            "                   reference replay against the audio loop's 2. */\n"
            "                const double lb_target =\n"
            "                    (double)audio_pace_floor + 1024.0;\n"
            "                const double lb_adj = std::clamp(\n"
            "                    (lb_q - lb_target) / 2048.0, -1.0, 1.0) * 0.02;\n"
            "                const uint64_t lb_period =\n"
            "                    (uint64_t)(lb_p0 * (1.0 + lb_adj));\n"
            "                const uint64_t lb_w0 = SDL_GetPerformanceCounter();\n"
            "                uint64_t lb_now = lb_w0;\n"
            "                /* SHORT OF AUDIO: present at once and let the\n"
            "                   emulator run ahead, exactly as the audio-paced\n"
            "                   loop did after a slow stretch. The first cut\n"
            "                   waited anyway and refilled only through the\n"
            "                   rate control: the reference replay went from 7\n"
            "                   underruns to 334. A cadence hitch after a slow\n"
            "                   frame is better than a gap in the sound. */\n"
            "                const bool lb_short = lb_q < (double)audio_pace_floor;\n"
            "                {   /* LB_PACELOG=1: where each underrun lands */\n"
            "                    static const bool lb_log =\n"
            "                        std::getenv(\"LB_PACELOG\") != nullptr;\n"
            "                    static uint64_t lb_seen = 0;\n"
            "                    const uint64_t lb_u = audio_queue.underruns.load(\n"
            "                        std::memory_order_relaxed);\n"
            "                    if (lb_log && lb_u != lb_seen)\n"
            "                        std::fprintf(stderr, \"[lb_pace] frame %llu \"\n"
            "                            \"underruns %llu queue %.0f floor %u\\n\",\n"
            "                            (unsigned long long)shown_frames,\n"
            "                            (unsigned long long)lb_u, lb_q,\n"
            "                            (unsigned)audio_pace_floor);\n"
            "                    lb_seen = lb_u;\n"
            "                }\n"
            "                /* A LATE FRAME RESTARTS THE SCHEDULE FROM NOW.\n"
            "                   Keeping the schedule through overruns shorter than\n"
            "                   a period (so the light frame after a heavy one\n"
            "                   lands back on time) was tried in docs/349: exact by\n"
            "                   the gate, but no clear gain -- present sd 1.94 vs\n"
            "                   2.46 ms on one scene, 3.42 / 0.90 vs 0.66 on the\n"
            "                   reference replay, all under another process's load\n"
            "                   -- and under sustained overload it catches up in\n"
            "                   bursts of back-to-back presents, which reads worse\n"
            "                   than a slightly lower rate. Restart it is. */\n"
            "                if (lb_deadline == 0 || lb_now > lb_deadline ||\n"
            "                    lb_short)\n"
            "                    lb_deadline = lb_now;   /* late: present now */\n"
            "                const uint64_t lb_spin = frequency / 500u;  /* 2 ms */\n"
            "                while (lb_now + lb_spin < lb_deadline) {\n"
            "                    SDL_Delay(1);\n"
            "                    lb_now = SDL_GetPerformanceCounter();\n"
            "                }\n"
            "                while (lb_now < lb_deadline)\n"
            "                    lb_now = SDL_GetPerformanceCounter();\n"
            "                lb_deadline += lb_period;\n"
            "                phase_drain_ticks += lb_now - lb_w0;   /* idle */\n"
            "            } else {\n"
            "                lb_deadline = 0;\n"
            "            }\n"
            "        }\n"
            "        const uint64_t phase1 = SDL_GetPerformanceCounter();\n")
    replace(front,
            "                audio, audio_queue, audio_started, audio_pace_floor,\n",
            "                audio, audio_queue, audio_started,\n"
            "                /* legobattles-decomp docs/349: under LB_PACE=timer\n"
            "                   the clock paces the loop; this wait is only a\n"
            "                   safety bound 4096 frames (125 ms) higher. */\n"
            "                [] {\n"
            "                    static const bool a = std::getenv(\"LB_PACE\") &&\n"
            "                        std::strcmp(std::getenv(\"LB_PACE\"), \"audio\") == 0;\n"
            "                    return a;\n"
            "                }() ? audio_pace_floor : audio_pace_floor + 4096u,\n")
    done.append("frontend: presents paced on a 59.8261 Hz clock with DRC (docs/349)")










    # 3b. Hand the overlay this frame's TOP-screen framebuffer.
    #
    #     The design puts the top screen in a movable and scalable imgui
    #     overlay that starts in the lower-right corner, inside the main
    #     window. The existing top-screen SDL_Texture cannot be reused for that:
    #     in the separate layout each screen has its OWN SDL_Renderer, a texture
    #     belongs to the renderer that created it, and the overlay is bound to
    #     the bottom one. So the raw pixels are handed over and re-uploaded into
    #     a texture the overlay owns.
    edit(front, "    PresentationTicks ticks{};",
         ["lb_overlay_set_top_pixels(top_pixels, top_width, kScreenHeight);"
          "  /* legobattles-decomp */"])
    done.append("frontend.cpp: top-screen framebuffer feed")

    # 3b2. The single-window layout.
    #
    #      The main game window is the touch screen, with the top
    #      screen as a movable overlay. In the stacked layout the runner draws
    #      both screens into ONE window, top above bottom, each 192 tall. That
    #      is the DS Vanilla arrangement and it is the right base: switching
    #      between one window and two would mean recreating SDL windows, but
    #      choosing what to draw INTO one window is just a rect.
    #
    #      Enhanced therefore skips the top screen and gives the bottom the
    #      whole canvas. The top screen is not lost -- lb_overlay draws it as a
    #      panel from the framebuffer it is handed anyway.
    #      The LOGICAL SIZE is the lever, not the destination rect. Drawing the
    #      bottom screen into a 256x384 rect would stretch 192 rows to 384 and
    #      distort the picture, which is precisely what Enhanced must avoid:
    #      the goal is more field of view, not stretched video. Setting the
    #      logical size to one screen instead lets SDL scale 256x192 up to the
    #      window at the correct aspect.
    replace(front,
            "        render_screen(presentation, 0, top_rect);\n"
            "        render_screen(presentation, 1, bottom_rect);",
            "        if (lb_setting_enhanced()) {  /* legobattles-decomp */\n"
            "            // No logical size. It scales mouse events as well as\n"
            "            // drawing, which is what made ImGui, SDL and the guest\n"
            "            // touch mapping disagree about what an (x, y) meant.\n"
            "            // Window pixels throughout; the destination rect goes to\n"
            "            // the touch mapping via lb_overlay_set_field_rect.\n"
            "            SDL_RenderSetLogicalSize(renderer, 0, 0);\n"
            "            int win_w = 0, win_h = 0;\n"
            "            SDL_GetRendererOutputSize(renderer, &win_w, &win_h);\n"
            "            // SIZE THE SURFACE TO THE WINDOW, once per frame.\n"
            "            //\n"
            "            // This is what makes \"adaptive window size\" one\n"
            "            // feature instead of a pile of them. The surface used\n"
            "            // to be a fixed 448x192 (21:9) that then had to be\n"
            "            // either cropped to fill a 16:9 window -- throwing\n"
            "            // away about a hundred columns it had just drawn -- or\n"
            "            // barred to keep them. Neither is right, and offering\n"
            "            // the choice was really just exposing the flaw.\n"
            "            //\n"
            "            // Rendering exactly as many columns as the window's\n"
            "            // aspect calls for dissolves the question: fit and\n"
            "            // fill become the same rectangle, nothing is cropped,\n"
            "            // nothing is barred, no column is drawn and discarded,\n"
            "            // and the HUD's visible edge IS the surface edge.\n"
            "            //\n"
            "            // 448 is the renderer's ceiling (melonDS clamps and\n"
            "            // sizes its scanline buffers to it) and 256 is the\n"
            "            // cartridge's own width, which is the floor because\n"
            "            // showing LESS than the DS would be a regression.\n"
            "            // SIZE THE SURFACE FROM THE WINDOW, NOT ITS ASPECT.\n"
            "            //\n"
            "            // Aspect alone gave 192 * aspect columns and a fixed\n"
            "            // 192 rows, so a bigger window at the same shape got\n"
            "            // the same picture ENLARGED -- the reported zooming\n"
            "            // in. Dividing the window by a pixel scale instead\n"
            "            // holds the scale constant and spends the extra window\n"
            "            // on field of view, which is what \"consistent scaling\"\n"
            "            // means.\n"
            "            //\n"
            "            // The ceiling is the VERTEX FORMAT, not a preference.\n"
            "            // FinalPosition is masked when it is latched, and the\n"
            "            // masks were 0x1FF in x and 0xFF in y -- 512x256.\n"
            "            // They are 0x7FF and 0x1FF now, so the ceiling is\n"
            "            // 2048x512 -- the x half widened again when the band\n"
            "            // stopped being centred, because a clamped band shifts\n"
            "            // by the WHOLE overhang rather than half of it, and\n"
            "            // 1728 was exactly what half of it could reach.\n"
            "            // Widening them is inert on the vanilla\n"
            "            // path: at 256x192 a projected x never reaches 512,\n"
            "            // so the mask only ever bites once the viewport is\n"
            "            // wider than the hardware's own, which is exactly the\n"
            "            // enhanced path. melonDS keeps a 13-bit HiresPosition\n"
            "            // beside it for its GL renderer on the same argument.\n"
            "            //\n"
            "            // FOUR places have to agree or the surface silently\n"
            "            // stops growing in one axis: the mask, melonDS's own\n"
            "            // SetRenderWidth/Height clamps, MaxRenderWidth/Height\n"
            "            // (which size the scanline buffers), and the runner's\n"
            "            // nds_gpu3d_set_output_* setters. The height half of\n"
            "            // the last one was missed once and cost a 1893x1030\n"
            "            // window 44 rows, with the width tracking correctly\n"
            "            // beside it -- a refusal there is silent.\n"
            "            // The scale is settled BEFORE the surface is sized, and\n"
            "            // capped so the DS's own 256x192 always fits. Sizing\n"
            "            // first and re-deriving the scale afterwards pillarboxed\n"
            "            // a short window: the surface's height floor of 192 no\n"
            "            // longer matched window/scale, so the fit shrank to suit\n"
            "            // and left bars down both sides.\n"
            "            float lb_pscale = lb_setting_pixel_scale();\n"
            "            {\n"
            "                const float lb_fitw = (float)win_w / 256.0f;\n"
            "                const float lb_fith = (float)win_h / 192.0f;\n"
            "                const float lb_capscale =\n"
            "                    lb_fitw < lb_fith ? lb_fitw : lb_fith;\n"
            "                // BR-0007: never let the surface hit the renderer's\n"
            "                // 1728x768 ceiling. A capped axis made cover scale\n"
            "                // the other one up, the surface overflowed the\n"
            "                // window, and the camera could not reach the\n"
            "                // cropped edge columns. Flooring the scale keeps\n"
            "                // the surface exactly the window.\n"
            "                {\n"
            "                    const float lb_fw = (float)win_w / 1728.0f;\n"
            "                    const float lb_fh = (float)win_h / 768.0f;\n"
            "                    const float lb_fl = lb_fw > lb_fh ? lb_fw : lb_fh;\n"
            "                    if (lb_pscale < lb_fl) lb_pscale = lb_fl;\n"
            "                }\n"
            "                if (lb_pscale > lb_capscale) lb_pscale = lb_capscale;\n"
            "                if (lb_pscale < 1.0f) lb_pscale = 1.0f;\n"
            "            }\n"
            "            {\n"
            "                const float lb_scale = lb_pscale;\n"
            "                int lb_want = (int)((float)win_w / lb_scale + 0.5f);\n"
            "                int lb_wanth = (int)((float)win_h / lb_scale + 0.5f);\n"
            "                // Even, not a multiple of 64. The compute renderer\n"
            "                // requires width %% 64 == 0, but it is disabled --\n"
            "                // raising its ceiling starts it and then segfaults,\n"
            "                // see the note beside kComputeMaxWidth -- so rounding\n"
            "                // to 64 here would cost up to 63 columns of field of\n"
            "                // view and buy nothing. Restore ~63 in the same change\n"
            "                // that makes the GPU path stable.\n"
            "                lb_want &= ~1;\n"
            "                lb_wanth &= ~1;\n"
            "                if (lb_want < 256) lb_want = 256;\n"
            #     THE CEILING IS 1728x768, and the extension tables in
            #     lb_widemap.cpp were resized in the same change.
            #
            #     Raising this without them was tried and measured: a cell
            #     outside those tables is DECLINED, and a declined cell falls
            #     through to the 32-entry hardware tilemap, which wraps. The
            #     counter read declined_out_of_table=21957120 (its own text
            #     says MUST be 0) and the picture grew strips of stale sprite
            #     tiles down the right edge with black rectangles punched
            #     into the terrain along the bottom. The tables now carry
            #     columns -100..132 and rows -44..68, with static_asserts
            #     tying them to these two numbers.
"                if (lb_want > 1728) lb_want = 1728;\n"
            "                if (lb_wanth < 192) lb_wanth = 192;\n"
            "                if (lb_wanth > 768) lb_wanth = 768;\n"
            "                // THE EXTRA ROWS ARE ON BY DEFAULT. They were\n"
            "                // opt-in for one step, because the render path\n"
            "                // worked -- 472x256 filling a 1892x1031 window\n"
            "                // with no zooming -- while rows outside the\n"
            "                // guest's own picture still read tilemap the game\n"
            "                // never maintains and came out as stale sprite\n"
            "                // tiles across the top of the screen. lb_widemap\n"
            "                // now supplies those rows, for the native columns\n"
            "                // as well as the widened ones, so the height\n"
            "                // adapts alongside the width and the window buys\n"
            "                // field of view in both axes.\n"
            "                // LB_NOVFOV=1 pins the height at the DS's own 192\n"
            "                // for an A/B against that.\n"
            "                if (std::getenv(\"LB_NOVFOV\")) lb_wanth = 192;\n"
            "                if (lb_want != (int)nds_gpu3d_output_width())\n"
            "                    nds_gpu3d_set_output_width((uint16_t)lb_want);\n"
            "                if (lb_wanth != (int)nds_gpu3d_output_height())\n"
            "                    nds_gpu3d_set_output_height((uint16_t)lb_wanth);\n"
            "            }\n"
            "            // The texture is the MAXIMUM size; the guest surface\n"
            "            // is whatever was just chosen, so present that much of\n"
            "            // it and leave the rest of the texture alone.\n"
            "            const int lb_gw = (int)nds_gpu3d_output_width();\n"
            "            const int lb_ghh = (int)nds_gpu3d_output_height();\n"
            "            lb_present_src_w[1] = lb_gw;\n"
            "            lb_present_src_h[1] = lb_ghh;\n"
            "            // COVER, never stretch. min() draws the whole surface\n"
            "            // and pads whatever is left over, so a 448x192 source\n"
            "            // letterboxes EVERY window that is not 2.33:1 -- the\n"
            "            // reported black bars, and they were structural rather\n"
            "            // than a bug anywhere in the compositor. max() fills\n"
            "            // the window and lets the excess fall outside it; SDL\n"
            "            // clips the destination rect to the target on its own,\n"
            "            // so the crop needs no source rect, and passing the\n"
            "            // FULL (possibly oversized) rect to the touch mapping\n"
            "            // keeps window pixels mapping to guest columns exactly\n"
            "            // as before. Neither branch scales the axes\n"
            "            // differently, so nothing is ever distorted.\n"
            "            const float lb_sx = (float)win_w / (float)lb_gw;\n"
            "            const float lb_sy = (float)win_h / (float)lb_ghh;\n"
            "            // FIT shows every column that was rendered and bars\n"
            "            // the remainder; FILL covers the window and crops.\n"
            "            // The surface is 448 wide whatever the window is, so\n"
            "            // on a 16:9 window FILL draws about a hundred columns\n"
            "            // and then discards them -- so it did not scale with\n"
            "            // the window to provide more field of view.\n"
            "            // min() keeps them. Neither branch scales the axes\n"
            "            // differently, so neither distorts.\n"
            "            // The surface was sized FROM this scale, so presenting\n"
            "            // at it fills the window by construction; cover only\n"
            "            // absorbs the rounding of the even-size clamps. When a\n"
            "            // clamp really bit -- a huge window against the 512x256\n"
            "            // ceiling -- cover is still what should happen, and the\n"
            "            // picture scales up again beyond that point.\n"
            "            float lb_s = lb_sx > lb_sy ? lb_sx : lb_sy;\n"
            "            // ...but never crop INTO the DS's own view. Covering a\n"
            "            // window narrower than 4:3, or wider than 2.33:1, would\n"
            "            // start cutting off columns or rows the cartridge\n"
            "            // itself showed, and showing less than the DS is a\n"
            "            // regression however well it fills the window. Those\n"
            "            // two shapes bar slightly instead; every aspect between\n"
            "            // 4:3 and 21:9 -- which is every ordinary monitor --\n"
            "            // fills exactly.\n"
            "            // The \"never show less than the DS\" cap that used to sit\n"
            "            // here is applied to lb_pscale ABOVE, before the surface\n"
            "            // is sized. Re-applying it now would fight the sizing it\n"
            "            // already governed and pillarbox short windows.\n"
            "            // docs/397: EXCEPT WHEN THE SURFACE DID NOT FOLLOW. With\n"
            "            // adaptive width off the surface stays the DS's 256x192,\n"
            "            // and covering a 16:9 window with it cut a quarter of\n"
            "            // the cartridge's rows off. Fit when covering would\n"
            "            // hide more than rounding of the guest's own picture.\n"
            "            if ((float)win_w / lb_s < (float)(lb_gw < 256 ? lb_gw : 256) - 2.0f ||\n"
            "                (float)win_h / lb_s < (float)(lb_ghh < 192 ? lb_ghh : 192) - 2.0f)\n"
            "                lb_s = lb_sx < lb_sy ? lb_sx : lb_sy;\n"
            "            const int lb_dw = (int)((float)lb_gw * lb_s + 0.5f);\n"
            "            const int lb_dh = (int)((float)lb_ghh * lb_s + 0.5f);\n"
            "            const SDL_Rect lb_only{(win_w - lb_dw) / 2,\n"
            "                                   (win_h - lb_dh) / 2,\n"
            "                                   lb_dw, lb_dh};\n"
            "            render_screen(presentation, 1, lb_only);\n"
            "            lb_overlay_set_field_rect(lb_only.x, lb_only.y,\n"
            "                                      lb_only.w, lb_only.h,\n"
            "                                      lb_gw, lb_ghh);\n"
            "            // HOW MANY GUEST COLUMNS ARE ACTUALLY ON SCREEN.\n"
            "            //\n"
            "            // Cover crops: when the destination is wider than the\n"
            "            // window, the surface's own left and right edges fall\n"
            "            // outside it. The HUD has to anchor to the edge the\n"
            "            // PLAYER sees, not to the edge of the surface -- a\n"
            "            // button moved to surface x 443 on a 16:9 window lands\n"
            "            // in the cropped region and simply disappears, which is\n"
            "            // the reported \"HUD buttons don't fully match the\n"
            "            // window size\".\n"
            "            {\n"
            "                int lb_vis = lb_gw;\n"
            "                if (lb_dw > win_w && lb_dw > 0)\n"
            "                    lb_vis = (int)((float)lb_gw * (float)win_w /\n"
            "                                   (float)lb_dw + 0.5f);\n"
            "                if (lb_vis > lb_gw) lb_vis = lb_gw;\n"
            "                // ...and the same question about ROWS, for the\n"
            "                // same reason: a bottom-anchored element moved to\n"
            "                // a cropped-off row is as invisible as one moved\n"
            "                // to a cropped-off column.\n"
            "                int lb_visr = lb_ghh;\n"
            "                if (lb_dh > win_h && lb_dh > 0)\n"
            "                    lb_visr = (int)((float)lb_ghh * (float)win_h /\n"
            "                                    (float)lb_dh + 0.5f);\n"
            "                if (lb_visr > lb_ghh) lb_visr = lb_ghh;\n"
            "                lb_hud_set_visible_size(lb_vis, lb_visr);\n"
            "            }\n"
            "            {   /* legobattles-decomp: LB_VERBOSE=1 geometry */\n"
            "                // The window, the guest surface and the rect the\n"
            "                // field is drawn into -- which is also exactly\n"
            "                // what the touch mapping uses. \"The view does not\n"
            "                // fill the window\" and \"clicks land in the wrong\n"
            "                // place\" are both answered by these three\n"
            "                // together, and until now they had to be\n"
            "                // reconstructed by measuring a screenshot.\n"
            "                static const char* lb_v = std::getenv(\"LB_VERBOSE\");\n"
            "                static uint32_t lb_last_log = 0;\n"
            "                if (lb_v) {\n"
            "                    const uint32_t lb_now = SDL_GetTicks();\n"
            "                    if (lb_now - lb_last_log >= 1000u) {\n"
            "                        lb_last_log = lb_now;\n"
            "                        std::fprintf(stderr,\n"
            "                            \"[lb_geom] t=%ums window=%dx%d \"\n"
            "                            \"guest=%dx%d field=%d,%d %dx%d \"\n"
            "                            \"scale=%.3f\\n\",\n"
            "                            (unsigned)lb_now, win_w, win_h,\n"
            "                            lb_gw, lb_ghh,\n"
            "                            lb_only.x, lb_only.y, lb_only.w,\n"
            "                            lb_only.h, (double)lb_s);\n"
            "                    }\n"
            "                }\n"
            "            }\n"
            "        } else {\n"
            "            SDL_RenderSetLogicalSize(renderer,\n"
            "                                     presentation.canvas_width,\n"
            "                                     kScreenHeight * 2);\n"
            "            render_screen(presentation, 0, top_rect);\n"
            "            render_screen(presentation, 1, bottom_rect);\n"
            "        }\n"
            "        // The overlay draws AFTER the branch, not inside it. It\n"
            "        // used to be anchored on the stacked bottom_rect call,\n"
            "        // which now only exists in the else arm -- so in Enhanced,\n"
            "        // the default, the panel silently stopped rendering.\n"
            "        lb_overlay_draw(presentation.windows[0], renderer);")
    done.append("frontend.cpp: single-window enhanced layout")

    # 3b2. BR-0010 / SG-0011: DS VANILLA IN WINDOW PIXELS, like Enhanced.
    #
    #      Vanilla used SDL_RenderSetLogicalSize(canvas_width, 384), which
    #      scales mouse events as well as drawing. The mouse gate and the
    #      original tail of set_touch_from_mouse both assumed logical
    #      coordinates, and the tail released at (0,0) -- the same defect
    #      docs/224 fixed for Enhanced (the cartridge acts on the release
    #      edge, at the position it reads then). Vanilla now draws into two
    #      rects computed in window pixels (lb_overlay_vanilla_layout, which
    #      also follows the Display tab's two size sliders and the screen
    #      swap), publishes the touch screen's rect, and maps the mouse
    #      through it, releasing where the pen last was.
    replace(front,
            "        } else {\n"
            "            SDL_RenderSetLogicalSize(renderer,\n"
            "                                     presentation.canvas_width,\n"
            "                                     kScreenHeight * 2);\n"
            "            render_screen(presentation, 0, top_rect);\n"
            "            render_screen(presentation, 1, bottom_rect);\n"
            "        }\n",
            "        } else {\n"
            "            /* legobattles-decomp BR-0010 / SG-0011: window pixels. */\n"
            "            SDL_RenderSetLogicalSize(renderer, 0, 0);\n"
            "            int lb_ww = 0, lb_wh = 0;\n"
            "            SDL_GetRendererOutputSize(renderer, &lb_ww, &lb_wh);\n"
            "            int lb_t[4], lb_b[4];\n"
            "            lb_overlay_vanilla_layout(lb_ww, lb_wh, lb_w0, lb_w1,\n"
            "                                      lb_t, lb_b);\n"
            "            const SDL_Rect lb_top_dst{lb_t[0], lb_t[1], lb_t[2], lb_t[3]};\n"
            "            const SDL_Rect lb_bot_dst{lb_b[0], lb_b[1], lb_b[2], lb_b[3]};\n"
            "            render_screen(presentation, 0, lb_top_dst);\n"
            "            render_screen(presentation, 1, lb_bot_dst);\n"
            "            (void)top_rect;\n"
            "            (void)bottom_rect;\n"
            "        }\n")
    replace(front,
            "    if (!down || x < left || x >= left + kScreenWidth ||\n"
            "        y < bottom_origin || y >= bottom_origin + kScreenHeight) {\n"
            "        nds_set_touch(0, 0, false);\n"
            "        return;\n"
            "    }\n",
            "    {   /* legobattles-decomp BR-0010: the published touch rect, in\n"
            "           window pixels; a release stays where the pen was. */\n"
            "        int lb_tx = 0, lb_ty = 0, lb_tw = 0, lb_th = 0;\n"
            "        if (!lb_setting_enhanced() &&\n"
            "            lb_overlay_touch_rect(&lb_tx, &lb_ty, &lb_tw, &lb_th)) {\n"
            "            if (!down || window_x < lb_tx || window_x >= lb_tx + lb_tw ||\n"
            "                window_y < lb_ty || window_y >= lb_ty + lb_th) {\n"
            "                nds_set_touch((uint16_t)lb_last_touch_x,\n"
            "                              (uint16_t)lb_last_touch_y, false);\n"
            "                return;\n"
            "            }\n"
            "            lb_last_touch_x = std::clamp<int>(\n"
            "                (window_x - lb_tx) * kScreenWidth / lb_tw, 0,\n"
            "                kScreenWidth - 1);\n"
            "            lb_last_touch_y = std::clamp<int>(\n"
            "                (window_y - lb_ty) * kScreenHeight / lb_th, 0,\n"
            "                kScreenHeight - 1);\n"
            "            nds_set_touch((uint16_t)lb_last_touch_x,\n"
            "                          (uint16_t)lb_last_touch_y, true);\n"
            "            return;\n"
            "        }\n"
            "    }\n"
            "    if (!down || x < left || x >= left + kScreenWidth ||\n"
            "        y < bottom_origin || y >= bottom_origin + kScreenHeight) {\n"
            "        nds_set_touch(lb_last_touch_x, lb_last_touch_y, false);\n"
            "        return;\n"
            "    }\n")
    done.append("frontend.cpp: vanilla in window pixels, published touch rect")

    # 3c. Vanilla / Enhanced, switchable while the game runs.
    #
    #     Enhanced lets the window take any size and widens the
    #     FIELD OF VIEW to suit rather than stretching the picture; Vanilla
    #     pins DS geometry. The runner already computes both -- an adaptive
    #     framebuffer and the native one -- and chooses per frame from
    #     options.adaptive_screens, so the switch is just which branch is taken.
    #
    #     options is a const reference, so rather than const_cast'ing it the two
    #     PER-FRAME read sites are gated on the toggle. The setup-time uses at
    #     lines ~739/750 are deliberately left alone: they allocate the sample
    #     targets the adaptive path needs, and those must exist for the whole
    #     session or switching back to Enhanced would have nothing to draw into.
    #
    #     PRECONDITION: the port must START in Enhanced, which port/game.toml
    #     does (Enhanced is the default). Starting Vanilla
    #     would never allocate the targets and the switch would be one-way.
    replace(front,
            "        if ((options.adaptive_screens & NDS_ADAPTIVE_TOP) &&",
            "        if ((options.adaptive_screens & NDS_ADAPTIVE_TOP) &&\n"
            "            lb_setting_enhanced() &&  /* legobattles-decomp */")
    replace(front,
            "        if (options.adaptive_screens & NDS_ADAPTIVE_BOTTOM)",
            "        if ((options.adaptive_screens & NDS_ADAPTIVE_BOTTOM) &&\n"
            "            lb_setting_enhanced())  /* legobattles-decomp */")
    done.append("frontend.cpp: live Vanilla/Enhanced switch")

    # 3c2. THE TOP SCREEN'S WIDTH MUST DESCRIBE THE TOP SCREEN'S BUFFER.
    #
    #      Upstream, a direct-present frame takes this branch:
    #
    #          else if (nds_gpu2d_direct_present_frame_active())
    #              top_width = nds_gpu3d_output_width();
    #
    #      top_pixels is still native_top -- g_fb[front][0], a Frame, which is
    #      256x192 and nothing else. Only the WIDTH is changed. Every consumer
    #      then strides a wide row over a narrow buffer:
    #
    #          SDL_UpdateTexture(textures[0], nullptr, top_pixels,
    #                            top_width * sizeof(uint32_t))
    #          lb_overlay_set_top_pixels(top_pixels, top_width, kScreenHeight)
    #
    #      At a 498-wide surface row y is read from offset y*498 instead of
    #      y*256, so the picture shears progressively, and the read runs
    #      498*192*4 = 382KB off a 196KB buffer. g_fb is
    #      std::array<std::array<Frame,2>,2>, so the memory immediately after
    #      the TOP screen's Frame is the BOTTOM screen's Frame: the top panel
    #      shears through the top screen and then continues straight into the
    #      bottom screen's pixels. That is the apparent "screenshot pipeline
    #      switching top and bottom screens" and the stretched
    #      Space Criminal Leader shot -- it is not the capture tooling at all,
    #      it is a genuine out-of-bounds read in the presenter that only looks
    #      like a plausible picture because the neighbouring array follows it.
    #
    #      The compute renderer is built (NDS_ENABLE_COMPUTE_RENDERER=ON), so
    #      nds_gpu2d_direct_present_frame_active() is reachable in ordinary
    #      play whenever the GL context is visible.
    #
    #      The honest width for a buffer that is 256 wide is 256. Direct
    #      present skips the adaptive compositor by design, so there is no
    #      wide top surface to describe; the branch is simply dropped.
    replace(front,
            "        else if (nds_gpu2d_direct_present_frame_active())\n"
            "            top_width = nds_gpu3d_output_width();",
            "        /* legobattles-decomp: NO. top_pixels is still the 256-wide\n"
            "           native frame here; see the note in apply_hooks.py 3c2.\n"
            "           Claiming nds_gpu3d_output_width() reads past the top\n"
            "           screen's Frame and into the bottom screen's. */")
    done.append("frontend.cpp: top_width no longer lies about a direct-present frame")

    # 3d. Rebindable keyboard input.
    #
    #     key_bit() is a twelve-case switch from SDL scancode to DS key bit.
    #     The overlay's table replaces it WHOLESALE with an early return, not by
    #     being consulted first: a partial override would leave the shipped key
    #     working alongside the new one, so rebinding A from Z to J would give
    #     two keys that press A.
    #
    #     The original switch is deliberately left in place below the return. It
    #     is now unreachable, but it is the reference for what the defaults are,
    #     and the overlay's table is seeded from exactly those values.
    edit(front, "uint16_t key_bit(SDL_Scancode key) {",
         ["return (uint16_t)lb_binding_key_bit((int)key);"
          "  /* legobattles-decomp: item 4 */"],
         indent="    ")
    done.append("frontend.cpp: rebindable keyboard input")

    #     The controller half, same shape and same reasoning.
    edit(front, "uint16_t controller_bit(SDL_GameControllerButton button) {",
         ["return (uint16_t)lb_binding_pad_bit((int)button);"
          "  /* legobattles-decomp: item 4 */"],
         indent="    ")
    done.append("frontend.cpp: rebindable controller input")

    #     BR-0001: Tab is the runner's hold-to-turbo key, tested BEFORE
    #     key_bit(), so a DS button bound to Tab never saw the key. Turbo now
    #     takes Tab only while the player has not bound it to a button.
    replace(front,
            "} else if (event.key.keysym.scancode == SDL_SCANCODE_TAB) {\n"
            "                    turbo_pressed = true;",
            "} else if (event.key.keysym.scancode == SDL_SCANCODE_TAB &&\n"
            "                           !lb_binding_key_bit((int)SDL_SCANCODE_TAB)) {\n"
            "                    turbo_pressed = true;  /* legobattles-decomp: BR-0001 */")
    replace(front,
            "if (event.key.keysym.scancode == SDL_SCANCODE_TAB) {\n"
            "                    turbo_pressed = false;",
            "if (event.key.keysym.scancode == SDL_SCANCODE_TAB &&\n"
            "                    !lb_binding_key_bit((int)SDL_SCANCODE_TAB)) {\n"
            "                    turbo_pressed = false;  /* legobattles-decomp: BR-0001 */")
    done.append("frontend.cpp: Tab turbo yields to a Tab binding (BR-0001)")

    #     SG-0016: "Skip the intro screens" runs the boot at turbo speed until
    #     the title shows (lb_boot_skip_wanted, lb_patches.cpp).
    replace(front,
            "        if (turbo_pressed != turbo_active) {\n"
            "            turbo_active = turbo_pressed;",
            "        const bool lb_turbo_want =\n"
            "            turbo_pressed || lb_boot_skip_wanted();  /* SG-0016 */\n"
            "        if (lb_turbo_want != turbo_active) {\n"
            "            turbo_active = lb_turbo_want;")
    done.append("frontend.cpp: turbo can be requested by the intro skip (SG-0016)")

    # 4. Draw, on the BOTTOM screen only. The ImGui SDL_Renderer backend binds
    #    to a single renderer, so the overlay must be drawn for exactly one of
    #    them; the port makes the touch screen the main game window, so the
    #    bottom is the right one. Each anchor identifies its layout uniquely.
    edit(front, "render_screen(presentation, 1, screen_rect);",
         ["lb_overlay_draw(presentation.windows[1], renderer);  /* lb */"])
    done.append("frontend.cpp: draw (separate layout)")

    edit(front, "render_screen(presentation, screen, screen_rect);",
         ["if (screen == 1)  /* legobattles-decomp */",
          "    lb_overlay_draw(presentation.windows[screen], renderer);"])
    done.append("frontend.cpp: draw (per-screen layout)")

    # 4z. AN INITIAL SIZE FOR THE BATTLEFIELD WINDOW, from the environment.
    #
    # The window is created at screen_width * kWindowScale, which is 1024x384,
    # and the only way to get another size was tools/lb_exp.py --window-size,
    # which drives a PowerShell resize after launch. That resize FAILS SILENTLY:
    # a run asking for 1600x900 reported "ASKED 1600x900, GOT 1024x384" and
    # printed nothing from the resize itself. lb_exp.py already carries a scar
    # from this -- "A whole performance run was spent that way".
    #
    # It matters beyond convenience. The camera clamp's failing case is a view
    # WIDER THAN THE WORLD (1536 px here), which needs a guest surface above
    # 1536 and therefore a window no resize was reliably delivering. The case
    # could not be tested at all.
    #
    # Set before the renderer is made, so the first frame is already the right
    # size and nothing has to survive a resize event.
    # windows[0], NOT windows[1]. The first attempt patched the
    # `presentation.separate` branch, which creates a second window for the
    # bottom screen -- and the shipped layout is NOT separate: the window is
    # titled plain "LEGO Battles" and carries both screens. The hook applied
    # cleanly, built, ran, and printed nothing, because that branch is dead in
    # the configuration that ships. Checking WHICH window the run actually
    # creates would have caught it before the build.
    #
    # RE-CENTRED AFTER THE RESIZE (docs/364). SDL centres the window at its
    # default size and SDL_SetWindowSize keeps the top-left, so a larger
    # LB_WINDOW grew off the display's right and bottom edges: 1473x830 sat
    # at (444,315) and reached (1945,1194) on a 1920x1080 display. Window
    # captures do not see the off-screen part, so every route capture at that
    # size had lost its bottom 114 rows and right 25 columns, which is where
    # the tutorial's text box is. Only the position changes, so emulation is
    # untouched.
    replace(front,
            "    if (!presentation.windows[0]) {\n"
            "        std::fprintf(stderr, \"[sdl] window failed: %s\\n\", SDL_GetError());\n"
            "        return false;\n"
            "    }",
            "    if (!presentation.windows[0]) {\n"
            "        std::fprintf(stderr, \"[sdl] window failed: %s\\n\", SDL_GetError());\n"
            "        return false;\n"
            "    }\n"
            "    /* legobattles-decomp: LB_WINDOW=WxH, the initial window size. */\n"
            "    if (const char* lb_ws = std::getenv(\"LB_WINDOW\")) {\n"
            "        int lb_ww = 0, lb_wh = 0;\n"
            "        if (std::sscanf(lb_ws, \"%dx%d\", &lb_ww, &lb_wh) == 2 &&\n"
            "            lb_ww >= 256 && lb_wh >= 192) {\n"
            "            SDL_SetWindowSize(presentation.windows[0], lb_ww, lb_wh);\n"
            "            SDL_SetWindowPosition(presentation.windows[0],\n"
            "                SDL_WINDOWPOS_CENTERED_DISPLAY(lb_display),\n"
            "                SDL_WINDOWPOS_CENTERED_DISPLAY(lb_display));\n"
            "            int lb_wx = 0, lb_wy = 0;\n"
            "            SDL_GetWindowPosition(presentation.windows[0], &lb_wx, &lb_wy);\n"
            "            std::fprintf(stderr,\n"
            "                         \"[lb_window] initial window %dx%d at %d,%d\\n\",\n"
            "                         lb_ww, lb_wh, lb_wx, lb_wy);\n"
            "            std::fflush(stderr);\n"
            "        }\n"
            "    }")
    done.append("frontend.cpp: LB_WINDOW sets the initial battlefield window size")

    # 4y. ZOOMING OUT STOPS AT THE WHOLE MAP, which is also what removes the
    #     black bars either side of the world.
    #
    #     Photographed at LB_WINDOW=1700x600 with pixel scale 1: terrain across
    #     the full width, and ~164 px of BLACK split between the two edges --
    #     the guest surface is 1700 px wide and the world is 1536. That is the
    #     camera nudging around at the edges and bumping into black, seen
    #     from the other side: there is simply more surface than
    #     there is world.
    #
    #     Capping the camera clamp cannot fix it. The clamp decides where the
    #     camera STOPS; it has no say in how much surface there is to fill.
    #     Flooring the scale does fix it, and needs no new concept:
    #
    #         scale >= win_w / world_w   =>   surface_w <= world_w
    #
    #     At 1700x600 against 1536x1024 the floor is 1700/1536 = 1.107, giving
    #     a 1536x542 surface that exactly fills the window. No margin, no black,
    #     and the camera keeps a valid range for free.
    #
    #     This IS the zoom-out limit an RTS has: out until the whole map is in
    #     shot, no further. lb_world_extent() returns 0 until a level is up, and
    #     then the floor simply does not apply -- menus are unaffected.
    #
    #     ...AND IT MUST NOT ZOOM IN PAST THE CAP (docs/364). The floor was
    #     written after the cap above and could undo it: when the world is
    #     smaller than the window at the cap, the floor raised the scale past
    #     it, the surface came out at world_w x 192, and the cover fit below
    #     cropped the DS's own rows at the top and the bottom. The campaign
    #     tutorial opens on a 13x12-tile world (312x192 px, tiles x 8..20,
    #     y 43..54, read at view+0x150/+0x158). At 1473x830 the floor was 4.72
    #     against a cap of 4.32, so 8 rows were lost at each end, and with them
    #     the second line of "Touch a unit with the stylus to select it." At
    #     3440x1440 it would be 11.0 against 7.5, about 61 of the 192 rows.
    #     A full-size 64x64 map cannot reach this at any shape between 1:4 and
    #     8:1. With the cap reapplied, the view is the world plus a margin in
    #     one axis, which the band already fills from the map around it.
    #     LB_FLOORCAP=0 restores the uncapped floor for an A/B.
    replace(front,
            "                if (lb_pscale > lb_capscale) lb_pscale = lb_capscale;\n"
            "                if (lb_pscale < 1.0f) lb_pscale = 1.0f;",
            "                if (lb_pscale > lb_capscale) lb_pscale = lb_capscale;\n"
            "                if (lb_pscale < 1.0f) lb_pscale = 1.0f;\n"
            "                /* legobattles-decomp: never show more than the map. */\n"
            "                {\n"
            "                    int lb_wow = 0, lb_woh = 0;\n"
            "                    if (lb_world_extent(&lb_wow, &lb_woh) &&\n"
            "                        lb_wow > 0 && lb_woh > 0) {\n"
            "                        const float lb_fw = (float)win_w / (float)lb_wow;\n"
            "                        const float lb_fh = (float)win_h / (float)lb_woh;\n"
            "                        const float lb_floor = lb_fw > lb_fh ? lb_fw : lb_fh;\n"
            "                        if (lb_pscale < lb_floor) lb_pscale = lb_floor;\n"
            "                        /* ...but never past the cap: zooming in past\n"
            "                           it crops the DS's own rows (docs/364). */\n"
            "                        static const bool lb_floorcap =\n"
            "                            !(std::getenv(\"LB_FLOORCAP\") &&\n"
            "                              std::getenv(\"LB_FLOORCAP\")[0] == '0');\n"
            "                        const float lb_ceil =\n"
            "                            lb_capscale > 1.0f ? lb_capscale : 1.0f;\n"
            "                        if (lb_floorcap && lb_pscale > lb_ceil)\n"
            "                            lb_pscale = lb_ceil;\n"
            "                    }\n"
            "                }")
    done.append("frontend.cpp: zoom-out stops at the whole map (no black margin)")

    # 5. Build wiring.
    imgui = imgui_dir.replace("\\", "/")
    edit(cmake, "add_executable(nds_runner",
         ["src/lb_overlay.cpp",
          "src/lb_patches.cpp",
          "src/lb_field.cpp",
          # The map-space visibility record (docs/247), kept out of
          # lb_widemap.cpp so that file stays a pure delete in phase 4.
          "src/lb_fog.cpp",
          # The shadow harness and the decompilation it grades (docs/333).
          # registry.c is C, not C++; CMake picks the compiler from the
          # extension and BATTLES_ON_RECOMP below points its loads at the bus.
          "src/lb_shadow.cpp",
          "src/battles/registry.c",
          "src/battles/divmod.c",
          "src/battles/leaf.c",
          "src/battles/softfloat.c",
          "src/lb_widemap.cpp",
          "src/lb_net.cpp",
          "src/lb_balance.cpp",
          "src/lb_hud.cpp",
          "src/lb_music.cpp",
          # lb_demo.cpp was listed TWICE here. CMake de-duplicates sources so it
          # built anyway, which is why it survived; it is still wrong.
          "src/lb_demo.cpp",
          # Team Builder and Ban Units (docs/384).
          "src/lb_teams.cpp",
          "src/lb_filedlg.cpp",
          "src/lb_prof.cpp",
          # The event pump on its own fiber (docs/385), for <windows.h>.
          "src/lb_winloop.cpp",
          # Level Palettes (docs/387).
          "src/lb_palette.cpp",
          # Mod maps from a mods folder (docs/391).
          "src/lb_mods.cpp",
          # Online chat and the swear filter (docs/415).
          "src/lb_chat.cpp",
          # HTTPS for the master server (docs/419), for <windows.h>.
          "src/lb_https.cpp",
          # A host's mod map from the community archive (docs/420).
          "src/lb_mapfetch.cpp",
          # The host-level WH: smooth online play (docs/423).
          "src/lb_vwh.cpp",
          # The master server's relay (docs/424), for <winsock2.h>.
          "src/lb_relay.cpp",
          # The notification sound (docs/427).
          "src/lb_sfx.cpp",
          # Post-match statistics (SG-0018).
          "src/lb_scores.cpp",
          # The picture of a match for the Discord log (docs/428).
          "src/lb_matchpic.cpp",
          # Map bans, Random maps, ban presets (docs/429).
          "src/lb_mapbans.cpp",
          # Discord Rich Presence (docs/430), for <windows.h>.
          "src/lb_discord.cpp",
          # A host sends a joiner its mod map (docs/432).
          "src/lb_maptx.cpp",
          # Download and install a release in place (docs/435).
          "src/lb_update.cpp",
          # The first-run setup (docs/394).
          "src/lb_setup.cpp",
          # The executable's icon (lbapp.png), resource 1.
          "src/lb_app.rc",
          "%s/imgui.cpp" % imgui,
          "%s/imgui_draw.cpp" % imgui,
          "%s/imgui_tables.cpp" % imgui,
          "%s/imgui_widgets.cpp" % imgui,
          "%s/backends/imgui_impl_sdl2.cpp" % imgui,
          "%s/backends/imgui_impl_sdlrenderer2.cpp" % imgui],
         indent="    ")
    done.append("CMakeLists.txt: sources")
    # The icon resource needs the RC language (windres).
    replace(cmake, "LANGUAGES C CXX)", "LANGUAGES C CXX RC)")
    done.append("CMakeLists.txt: the RC language, for the app icon")

    # ---- Keep the game window off the primary display -----------------------
    #
    # Every run this project launches used to pop a window onto the PRIMARY
    # display and take focus, interrupting whatever else was in use there.
    #
    # Two separate causes, so two fixes. Placement is SDL_WINDOWPOS_CENTERED,
    # which means centred on display 0. Activation is Windows' default for a
    # newly shown window, which SDL 2.0.18+ can suppress with a hint. Fixing
    # only one of them still interrupts the desktop.
    edit(front, "presentation.windows[0] = SDL_CreateWindow(",
         ["// WHICH DISPLAY (docs/396). A player's window opens on the PRIMARY",
          "// monitor: the one whose desktop origin is 0,0, which is how Windows",
          "// defines it (SDL's index 0 usually is, but its order is not",
          "// promised).",
          "//",
          "// A test names another display: LB_DISPLAY by index, or",
          "// LB_DISPLAY_W/H by its mode (the harness sets 1920x1080 for its",
          "// test display; a matching display wins, ties to the highest index).",
          "// Only then does the window also appear WITHOUT taking focus, so a",
          "// test never interrupts whatever is in use. Until docs/396 both were",
          "// the default, so a release opened on a secondary monitor.",
          "int lb_display = 0;",
          "{",
          "    const int lb_ndisp = SDL_GetNumVideoDisplays();",
          "    const char* lb_ew = std::getenv(\"LB_DISPLAY_W\");",
          "    const char* lb_eh = std::getenv(\"LB_DISPLAY_H\");",
          "    const char* lb_de = std::getenv(\"LB_DISPLAY\");",
          "    const bool lb_test = (lb_ew && lb_eh) || lb_de;",
          "    const int lb_dw = lb_ew ? std::atoi(lb_ew) : 0;",
          "    const int lb_dh = lb_eh ? std::atoi(lb_eh) : 0;",
          "    for (int i = 0; i < lb_ndisp; ++i) {",
          "        SDL_Rect lb_b{};",
          "        if (SDL_GetDisplayBounds(i, &lb_b) != 0) continue;",
          "        const bool lb_match = lb_ew && lb_eh && lb_b.w == lb_dw && lb_b.h == lb_dh;",
          "        const bool lb_primary = lb_b.x == 0 && lb_b.y == 0;",
          "        std::fprintf(stderr,",
          "                     \"[sdl] display %d: %dx%d at %d,%d%s%s\\n\",",
          "                     i, lb_b.w, lb_b.h, lb_b.x, lb_b.y,",
          "                     lb_primary ? \"  (primary)\" : \"\",",
          "                     lb_match ? \"  <- matches LB_DISPLAY_W/H\" : \"\");",
          "        if (lb_test ? lb_match : (lb_primary && !lb_display)) lb_display = i;",
          "    }",
          "    if (lb_de) {",
          "        const int lb_want = std::atoi(lb_de);",
          "        if (lb_want >= 0 && lb_want < lb_ndisp) lb_display = lb_want;",
          "    }",
          "    std::fprintf(stderr, \"[sdl] window on display %d of %d%s\\n\",",
          "                 lb_display, lb_ndisp, lb_test ? \" (test placement, no focus)\" : \"\");",
          "    if (lb_test) SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, \"1\");",
          "}"],
         after=False, indent="")
    done.append("frontend.cpp: window placement + no focus steal")

    replace(front,
            "        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,",
            "        SDL_WINDOWPOS_CENTERED_DISPLAY(lb_display),\n"
            "        SDL_WINDOWPOS_CENTERED_DISPLAY(lb_display),")
    done.append("frontend.cpp: centre on the chosen display")

    edit(cmake, "target_include_directories(nds_runner PRIVATE",
         [imgui, "%s/backends" % imgui],
         indent="    ")
    done.append("CMakeLists.txt: include directories")

    # src/battles/ is the decompilation, and it is written to compile in TWO
    # places: here, where its loads go through ndsrecomp's bus so the runner can
    # grade it, and in a native build, where the same macros are plain loads out
    # of a buffer the port owns. BATTLES_ON_RECOMP is what selects the first.
    # Without it the file would compile and read host addresses -- which is not
    # a build error, it is a segfault at the first lookup, so the definition is
    # load-bearing rather than cosmetic.
    edit(cmake, "target_link_libraries(nds_runner PRIVATE nds_banks",
         ["set_source_files_properties(",
          "    src/battles/registry.c",
          "    src/battles/divmod.c",
          "    src/battles/leaf.c",
          "    src/battles/softfloat.c",
          "    PROPERTIES COMPILE_DEFINITIONS BATTLES_ON_RECOMP)"])
    done.append("CMakeLists.txt: BATTLES_ON_RECOMP for the decompilation")

    # The native file picker in src/lb_filedlg.cpp needs comdlg32 for
    # GetOpenFileNameW / GetSaveFileNameW. Guarded by WIN32 so a Linux build is
    # unaffected -- lb_filedlg.cpp compiles to a stub returning 0 there and the
    # Saves tab keeps the text field it already had.
    edit(cmake, "target_link_libraries(nds_runner PRIVATE nds_banks",
         ["if(WIN32)",
          "    target_link_libraries(nds_runner PRIVATE comdlg32 winhttp)",   # docs/419: HTTPS
          # docs/420: inflate, statically. -l:libz.a, not find_library: the
          # compiler then takes the zlib beside its own headers. find_library
          # searched the shell's PATH, and the release runner's devkitPro
          # shell gave it devkitPro's MSYS-runtime libz.a, whose inflate fails
          # in the native game (v0.3.0: the Maps page listed nothing).
          "    target_link_libraries(nds_runner PRIVATE -l:libz.a)",
          "endif()"])
    done.append("CMakeLists.txt: comdlg32 for the native file picker")

    # docs/405: miniupnpc for UPnP port forwarding (src/lb_net.cpp), linked
    # STATICALLY so a release ships no extra DLL. The MSYS2 UCRT64 package
    # (mingw-w64-ucrt-x86_64-miniupnpc) installs libminiupnpc.a beside the
    # compiler; on Windows it needs iphlpapi and ws2_32.
    edit(cmake, "target_link_libraries(nds_runner PRIVATE nds_banks",
         ["if(WIN32)",
          "    get_filename_component(LB_TC_BIN \"${CMAKE_C_COMPILER}\" DIRECTORY)",
          "    find_library(LB_MINIUPNPC NAMES libminiupnpc.a miniupnpc",
          "                 HINTS \"${LB_TC_BIN}/../lib\" REQUIRED)",
          "    target_link_libraries(nds_runner PRIVATE ${LB_MINIUPNPC} iphlpapi ws2_32)",
          "endif()"])
    done.append("CMakeLists.txt: miniupnpc (static) for UPnP")

    # 3z. Attribute static-dispatch validation rejections.
    #
    #     docs/302: the ARM7 runs 87.6% interpreted, and the cause is NOT a
    #     missing entry point. Every one of the six hottest ARM7 interpreter
    #     entries IS in g_dispatch_lb_arm7_wram, with thumb=0, a generated
    #     function AND a validation record -- and the banks are registered
    #     under NDS_ARM7. That leaves exactly one statement that can drop
    #     them: the `continue` in lookup_in() when validation fails.
    #
    #     Which of its two checks fails decides the fix, and they want
    #     opposite ones -- no provenance means the autoload copy never marked
    #     the bytes written, a byte difference means the expected image is not
    #     what is in RAM. So count them apart, and keep the first rejected pc
    #     so the address can be disassembled afterwards.
    #
    #     THIS CANNOT LAG THE GAME. lookup_in runs only on the cache slow
    #     path, which a whole match enters 6,655 times on the ARM7 against
    #     22,613,797 cached-absent hits -- the result is cached per address
    #     for the rest of the session. Two increments there are unmeasurable,
    #     and the fast path is not touched at all.
    stats_h = os.path.join(runner_dir, "src", "dispatch_stats.h")
    replace(stats_h,
            "    uint64_t cache_slow_lookup = 0;",
            "    uint64_t cache_slow_lookup = 0;\n"
            "    // legobattles-decomp (docs/302): why lookup_in() refused a\n"
            "    // candidate it had already matched by address and mode.\n"
            "    uint64_t static_reject_provenance = 0;\n"
            "    uint64_t static_reject_bytes = 0;\n"
            "    uint64_t static_reject_first_pc = 0;\n"
            "    // legobattles-decomp (docs/322): absent dispatches split by\n"
            "    // WHERE they land. cache_hit_absent read 693M in a 19 fps\n"
            "    // run against 5.1M in a 58 fps one, and the tier-3 coverage\n"
            "    // put 91% of that in forty bytes of ARM9 ITCM -- which is\n"
            "    // two instruments agreeing, not one counting. Count it at\n"
            "    // the point of the decision, so any run says whether ITCM\n"
            "    // is resolving to absent, fast or slow.\n"
            "    uint64_t cache_absent_itcm = 0;\n"
            "    uint64_t cache_absent_itcm_pc = 0;")
    replace(rtarm,
            "            \",\\\"cache_slow_lookup\\\":\" +\n"
            "                std::to_string(s.cache_slow_lookup) +",
            "            \",\\\"cache_slow_lookup\\\":\" +\n"
            "                std::to_string(s.cache_slow_lookup) +\n"
            "            \",\\\"static_reject_provenance\\\":\" +\n"
            "                std::to_string(s.static_reject_provenance) +\n"
            "            \",\\\"static_reject_bytes\\\":\" +\n"
            "                std::to_string(s.static_reject_bytes) +\n"
            "            \",\\\"static_reject_first_pc\\\":\" +\n"
            "                std::to_string(s.static_reject_first_pc) +\n"
            "            \",\\\"cache_absent_itcm\\\":\" +\n"
            "                std::to_string(s.cache_absent_itcm) +\n"
            "            \",\\\"cache_absent_itcm_pc\\\":\" +\n"
            "                std::to_string(s.cache_absent_itcm_pc) +")
    # WAS THE ITCM ENTRY EVEN MATCHED?
    #
    # docs/322: three runs at 17-19 fps with 97-99.8% of absent dispatches in
    # ITCM, first pc 0x01FF80E8 every time, against one at 57.5 with zero. The
    # counters cannot say WHY: the slow run has FEWER byte rejections than the
    # fast one (47,875 against 66,918) and its static_reject_first_pc is not in
    # ITCM at all, so the refusal is not showing up as a rejection.
    #
    # candidate_count is the discriminator. Zero means no bank entry matched
    # the address at all -- a registration or range problem. Non-zero with an
    # inactive candidate means it matched and the validation gate refused it.
    # Printed once, on a path already taken only when a full lookup missed.
    replace(rtarm,
            "    const DispatchEntry* hit = lookup_static(\n"
            "        c, pc, thumb, &candidate_count, &inactive_candidate);",
            "    const DispatchEntry* hit = lookup_static(\n"
            "        c, pc, thumb, &candidate_count, &inactive_candidate);\n"
            "    /* legobattles-decomp (docs/322) */\n"
            "    if (!hit && pc >= 0x01FF8000u && pc < 0x02000000u) {\n"
            "        static bool lb_said_miss = false;\n"
            "        if (!lb_said_miss) {\n"
            "            lb_said_miss = true;\n"
            "            std::fprintf(stderr,\n"
            "                \"[lb_itcm] MISS pc=%08X thumb=%d candidates=%u \"\n"
            "                \"inactive=%d\\n\",\n"
            "                pc, thumb ? 1 : 0, unsigned(candidate_count),\n"
            "                inactive_candidate ? 1 : 0);\n"
            "            std::fflush(stderr);\n"
            "        }\n"
            "    }")
    done.append("runtime_arm.cpp: say whether a missed ITCM lookup matched anything")

    replace(rtarm,
            "        ++stats.cache_hit_absent;",
            "        ++stats.cache_hit_absent;\n"
            "        /* legobattles-decomp (docs/322): and WHERE. ARM9 ITCM is\n"
            "           0x01FF8000..0x01FFFFFF, where a DS game puts the loops\n"
            "           it most wants fast -- the hot one here is the Nitro\n"
            "           thread scheduler, entered with IRQs masked by\n"
            "           `msr cpsr_c, #0xD2`. An absent dispatch there costs far\n"
            "           more than one anywhere else, so it earns its own\n"
            "           counter rather than a share of a total. Two compares,\n"
            "           on the absent path only. */\n"
            "        if (pc >= 0x01FF8000u && pc < 0x02000000u) {\n"
            "            ++stats.cache_absent_itcm;\n"
            "            if (!stats.cache_absent_itcm_pc)\n"
            "                stats.cache_absent_itcm_pc = pc;\n"
            "        }")
    done.append("runtime_arm.cpp: absent dispatches attributed to ITCM")

    # Split the short-circuited condition so each half is attributable. The
    # evaluation order is unchanged: bus_live_bytes_equal still only runs when
    # provenance passed, which is what the original `||` did.
    replace(rtarm,
            "            if (!bus_range_has_write_provenance(validation->addr,\n"
            "                                                validation->size) ||\n"
            "                !bus_live_bytes_equal(validation->addr,\n"
            "                                      validation->expected,\n"
            "                                      validation->size)) {\n"
            "                if (inactive_candidate)",
            "            const bool lb_prov = bus_range_has_write_provenance(\n"
            "                validation->addr, validation->size);\n"
            "            const bool lb_same = lb_prov && bus_live_bytes_equal(\n"
            "                validation->addr, validation->expected,\n"
            "                validation->size);\n"
            "            if (!lb_same) {\n"
            "                NdsDispatchStats& lb_st =\n"
            "                    g_nds_dispatch_stats[g_nds_active];\n"
            "                if (!lb_prov) ++lb_st.static_reject_provenance;\n"
            "                else          ++lb_st.static_reject_bytes;\n"
            "                if (!lb_st.static_reject_first_pc)\n"
            "                    lb_st.static_reject_first_pc = pc;\n"
            "                /* legobattles-decomp (docs/322): and if it is\n"
            "                   ITCM, say which byte. cache_absent_itcm counts\n"
            "                   THAT ITCM went absent; this says WHY, and the\n"
            "                   two together discriminate the cases -- counter\n"
            "                   up with this silent means the entry was never\n"
            "                   matched at all, rather than matched and\n"
            "                   refused. Once per session, on a path already\n"
            "                   taken only when a lookup has failed. */\n"
            "                if (validation->addr >= 0x01FF8000u &&\n"
            "                    validation->addr < 0x02000000u) {\n"
            "                    static bool lb_said_itcm = false;\n"
            "                    if (!lb_said_itcm) {\n"
            "                        lb_said_itcm = true;\n"
            "                        uint32_t lb_off = 0;\n"
            "                        for (; lb_off < validation->size; ++lb_off)\n"
            "                            if (!bus_live_bytes_equal(\n"
            "                                    validation->addr + lb_off,\n"
            "                                    validation->expected + lb_off,\n"
            "                                    1u))\n"
            "                                break;\n"
            "                        std::fprintf(stderr,\n"
            "                            \"[lb_itcm] REJECTED pc=%08X range \"\n"
            "                            \"%08X+%u prov=%d first differing \"\n"
            "                            \"byte at +%u (expected %02X)\\n\",\n"
            "                            pc, validation->addr,\n"
            "                            unsigned(validation->size),\n"
            "                            lb_prov ? 1 : 0, unsigned(lb_off),\n"
            "                            lb_off < validation->size\n"
            "                                ? validation->expected[lb_off] : 0);\n"
            "                        /* AND WHAT IS ACTUALLY THERE. The\n"
            "                           expected byte is 0xEA, the opcode byte\n"
            "                           of an ARM `b`, at 0x01FF80FF -- inside\n"
            "                           the hot loop. Whether the live code is\n"
            "                           a second static variant this port\n"
            "                           could simply lift, or something built\n"
            "                           at runtime, is the difference between\n"
            "                           a config change and a design problem,\n"
            "                           and four words either side settle it. */\n"
            "                        uint32_t lb_w0 =\n"
            "                            (validation->addr + lb_off) & ~3u;\n"
            "                        if (lb_w0 >= validation->addr + 16u)\n"
            "                            lb_w0 -= 16u;\n"
            "                        else lb_w0 = validation->addr;\n"
            "                        for (uint32_t lb_i = 0; lb_i < 8u; ++lb_i) {\n"
            "                            const uint32_t lb_a = lb_w0 + lb_i * 4u;\n"
            "                            const uint32_t lb_e =\n"
            "                                uint32_t(validation->expected[lb_a - validation->addr]) |\n"
            "                                (uint32_t(validation->expected[lb_a - validation->addr + 1]) << 8) |\n"
            "                                (uint32_t(validation->expected[lb_a - validation->addr + 2]) << 16) |\n"
            "                                (uint32_t(validation->expected[lb_a - validation->addr + 3]) << 24);\n"
            "                            std::fprintf(stderr,\n"
            "                                \"[lb_itcm]   %08X live %08X expected %08X%s\\n\",\n"
            "                                lb_a, bus_read_u32(lb_a), lb_e,\n"
            "                                bus_read_u32(lb_a) != lb_e ? \"  <-- DIFFERS\" : \"\");\n"
            "                        }\n"
            "                        /* THE WHOLE SECTION, ONCE.\n"
            "                           Two different ITCM functions have been\n"
            "                           seen to diverge -- 0x01FF8000 at its\n"
            "                           prologue and 0x01FF8068 at +0x97 -- so\n"
            "                           a word either side of one of them does\n"
            "                           not describe the problem. ITCM here is\n"
            "                           0x320 bytes; dumping all of it lets\n"
            "                           the live image be diffed against\n"
            "                           out/eur/arm9/section_itcm_01ff8000.bin\n"
            "                           offline, which is what decides whether\n"
            "                           this is a second static variant the\n"
            "                           lift can simply carry. */\n"
            "                        /* TWO READ PATHS, SIDE BY SIDE.\n"
            "                           The gate and the dump both go through\n"
            "                           bus_read_u32; two instruments sharing a\n"
            "                           path agree while both are wrong.\n"
            "                           bus_debug_read8 is the debug server's\n"
            "                           own route and resolves independently,\n"
            "                           so a disagreement here means the READ\n"
            "                           is the bug and there is no writer to\n"
            "                           hunt. docs/323 says do this first. */\n"
            "                        {\n"
            "                            const uint32_t a =\n"
            "                                validation->addr + (lb_off & ~3u);\n"
            "                            const uint32_t dbg =\n"
            "                                (uint32_t)bus_debug_read8(9, a)\n"
            "                              | ((uint32_t)bus_debug_read8(9, a+1) << 8)\n"
            "                              | ((uint32_t)bus_debug_read8(9, a+2) << 16)\n"
            "                              | ((uint32_t)bus_debug_read8(9, a+3) << 24);\n"
            "                            std::fprintf(stderr,\n"
            "                                \"[lb_itcm] %08X bus_read_u32 %08X \"\n"
            "                                \"bus_debug_read8 %08X  %s\\n\",\n"
            "                                a, bus_read_u32(a), dbg,\n"
            "                                bus_read_u32(a) == dbg\n"
            "                                    ? \"agree (the bytes really differ)\"\n"
            "                                    : \"DISAGREE -- the read path is the bug\");\n"
            "                            std::fflush(stderr);\n"
            "                        }\n"
            "                        std::fprintf(stderr,\n"
            "                            \"[lb_itcmdump] 01FF8000+320 live:\\n\");\n"
            "                        for (uint32_t lb_r = 0; lb_r < 0x320u;\n"
            "                             lb_r += 16u) {\n"
            "                            std::fprintf(stderr,\n"
            "                                \"[lb_itcmdump] %08X %08X %08X %08X %08X\\n\",\n"
            "                                0x01FF8000u + lb_r,\n"
            "                                bus_read_u32(0x01FF8000u + lb_r),\n"
            "                                bus_read_u32(0x01FF8004u + lb_r),\n"
            "                                bus_read_u32(0x01FF8008u + lb_r),\n"
            "                                bus_read_u32(0x01FF800Cu + lb_r));\n"
            "                        }\n"
            "                        std::fflush(stderr);\n"
            "                    }\n"
            "                }\n"
            "                if (inactive_candidate)")
    done.append("runtime_arm.cpp: attribute static validation rejections")

    # 0zz. NAME THE WRITER OF THE TWO CORRUPT ITCM BYTES (docs/326).
    #
    # The corruption is one 16-bit 0xFFFF store landing on ITCM offsets 0xFF and
    # 0x100 (guest 0x01FF80FF), inside the IRQ dispatcher. It fires in roughly
    # one run in four, and when it does the ITCM function is refused, falls back
    # to the interpreter, and the frame rate collapses -- which invalidates every
    # performance measurement taken in that run.
    #
    # WHY HERE AND NOT A WATCHPOINT. LB_WATCH is served by watch_addr, which is
    # called only from the SLOW bus path. Every ITCM write is served by the
    # inline fast window in runtime_arm.h, so a watchpoint is structurally blind
    # to it -- that is why one LB_WATCH run produced no lines at all and the
    # silence went unexplained for a long time.
    #
    # WHY HERE AND NOT IN runtime_arm.h. That header is included by all 302
    # generated banks, so touching it rebuilds every one of them. This function
    # is in runtime_arm.cpp and its own comment says every guest RAM write
    # funnels through it, slow path and generated-bank fast path alike -- the
    # same coverage for one TU's rebuild.
    #
    # COST. The call already exists at every write site, so the branch below adds
    # nothing to the call sites; this is not the docs/204 case, where inlining a
    # body AT every site cost more in code size than the call it removed. It is
    # still gated on a global that defaults off, so a normal build pays one
    # predicted branch on a hot global.
    #
    # Reporting in this call is the point: the store has just happened, so R[15]
    # is still at or beside the instruction that did it. A byte comparison after
    # the fact can only say the bytes changed, never who changed them.
    replace(rt_cpp,
            "extern \"C\" void runtime_note_code_write(void) {",
            "/* legobattles-decomp: set from LB_ITCMW=1. See apply_hooks 0zz. */\n"
            "extern \"C\" unsigned g_lb_itcmw = 0u;\n"
            "extern \"C\" void runtime_note_code_write(void) {\n"
            "#ifdef LB_ITCMW_PROBE\n"
            "    /* COMPILE-TIME GATED, and it was runtime-gated first. A build\n"
            "       carrying only the disarmed `if (g_lb_itcmw)` measured 37.19\n"
            "       fps against a 51-52 baseline -- a load and a branch on EVERY\n"
            "       guest RAM write is not free, which is the same lesson the\n"
            "       trace-guard note in the runtime_arm.h stanza records. The\n"
            "       shipped build must carry nothing at all, so the hunt uses a\n"
            "       separate binary: build with -DLB_ITCMW_PROBE. */\n"
            "    if (g_lb_itcmw) {\n"
            "        static uint8_t* lb_itcm = nullptr;\n"
            "        static int lb_tried = 0;\n"
            "        if (!lb_tried) {\n"
            "            lb_tried = 1;\n"
            "            BusRegion lb_r{};\n"
            "            if (bus_get_region(\"itcm\", &lb_r) && lb_r.len > 0x101u)\n"
            "                lb_itcm = const_cast<uint8_t*>(lb_r.ptr);\n"
            "        }\n"
            "        if (lb_itcm) {\n"
            "            /* ARM only after the boot copy has put the real bytes\n"
            "               there, or every write before that reports a false\n"
            "               change against a zero-filled ITCM. */\n"
            "            static int lb_armed = 0;\n"
            "            const int lb_ok = lb_itcm[0xFFu] == 0xEAu &&\n"
            "                              lb_itcm[0x100u] == 0x01u;\n"
            "            if (!lb_armed) { lb_armed = lb_ok; }\n"
            "            else if (!lb_ok) {\n"
            "                lb_armed = 0;   /* one report per corruption */\n"
            "                std::fprintf(stderr,\n"
            "                    \"[lb_itcmw] ITCM 0xFF/0x100 became %02X %02X \"\n"
            "                    \"at PC=%08X LR=%08X\\n\",\n"
            "                    lb_itcm[0xFFu], lb_itcm[0x100u],\n"
            "                    g_cpu.R[15], g_cpu.R[14]);\n"
            "                std::fflush(stderr);\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "#endif")
    done.append("runtime_arm.cpp: name the ITCM byte writer (LB_ITCMW)")

    #     docs/361, continued: where the composite decides to show engine A's
    #     native picture. Here, after every stanza that writes this text.
    replace(gpu,
            "    if (output_width <= 256 && output_height <= 192) {\n"
            "        if (width) *width = 256;\n"
            "        return native;\n"
            "    }",
            "    if (output_width <= 256 && output_height <= 192) {\n"
            "        if (engine == 0)   /* docs/361 */\n"
            "            lb_natskip_present(screen, false, g_unit[0].dispcnt);\n"
            "        if (width) *width = 256;\n"
            "        return native;\n"
            "    }")
    replace(gpu,
            "    if (!supported_scene) {\n"
            "        /* legobattles-decomp: this path writes only part of the\n"
            "           surface, so it clears first. See above. */\n",
            "    lb_natskip_present(screen, supported_scene, u.dispcnt);  /* 361 */\n"
            "    if (!supported_scene) {\n"
            "        /* legobattles-decomp: this path writes only part of the\n"
            "           surface, so it clears first. See above. */\n")
    #     The composite fetches the native picture every call but reads it only
    #     on the paths above (lb_natskip_present completes it there) and here.
    replace(gpu,
            "    const uint32_t* native = nds_gpu2d_framebuffer(screen);",
            "    /* docs/361: raw; completed on the paths that show it. */\n"
            "    const uint32_t* native = nds_gpu2d_framebuffer_raw(screen);")
    replace(gpu,
            "    if ((g_lb_adaptive_screens & (1u << (screen & 1))) == 0u) {\n"
            "        if (width) *width = 256;\n"
            "        return native;\n"
            "    }",
            "    if ((g_lb_adaptive_screens & (1u << (screen & 1))) == 0u) {\n"
            "        nds_gpu2d_native_complete(screen);   /* docs/361 */\n"
            "        if (width) *width = 256;\n"
            "        return native;\n"
            "    }")
    #     The frontend fetches both native pictures every frame and replaces
    #     the adaptive one with the composite; a native picture it really
    #     presents is completed first.
    front_cpp = os.path.join(runner_dir, "src", "frontend.cpp")
    replace(front_cpp,
            "        const uint32_t* const native_top = nds_gpu2d_framebuffer(0);\n"
            "        const uint32_t* top_pixels = native_top;\n"
            "        const uint32_t* bottom_pixels = nds_gpu2d_framebuffer(1);\n",
            "        /* docs/361: raw -- completed below if really presented. */\n"
            "        const uint32_t* const native_top = nds_gpu2d_framebuffer_raw(0);\n"
            "        const uint32_t* top_pixels = native_top;\n"
            "        const uint32_t* bottom_pixels = nds_gpu2d_framebuffer_raw(1);\n")
    replace(front_cpp,
            "        observe_top_black_bands(native_top, shown_frames);\n",
            "        if (top_pixels == native_top)   /* docs/361 */\n"
            "            nds_gpu2d_native_complete(0);\n"
            "        if (bottom_pixels == nds_gpu2d_framebuffer_raw(1))\n"
            "            nds_gpu2d_native_complete(1);\n"
            "        observe_top_black_bands(native_top, shown_frames);\n")
    replace(front_cpp,
            "            /* The two physical LCDs, native, always 256x192. */\n",
            "            /* The two physical LCDs, native, always 256x192. */\n"
            "            nds_gpu2d_want_native();   /* docs/361: the true picture */\n")
    replace(os.path.join(runner_dir, "src", "debug_server.cpp"),
            "            : nds_gpu2d_framebuffer(screen);",
            "            : (nds_gpu2d_want_native(),   /* docs/361 */\n"
            "               nds_gpu2d_framebuffer(screen));")
    done.append("gpu2d/frontend/debug: native-picture readers complete or "
                "want it (docs/361)")

    # 0zzz. NAME THE ACCESSES TO THE NULL PAGE (docs/369).
    #
    # The game invariably crashed when entering combat. Every
    # crashing log (two build/logs/session-*.log captures
    # and the reproductions) starts the same way: the ITCM IRQ
    # dispatcher's bytes at 0x01FF80FF and 0x01FF8100 have become FF FF
    # ("[lb_itcm] REJECTED pc=01FF8068 ... first differing byte at +151"), and
    # wild accesses follow. Those bytes are reached through ITCM's low mirror.
    # CP15 gives the ITCM a 32 MB virtual span from address 0 ("[cp15] ITCM
    # region: base=0 vsize=33554432"), so 0x000000FF and 0x01FF80FF are the
    # same byte. The game's own protection regions (printed by this switch:
    # region 5 = 0100002F, 16 MB at 0x01000000; MPU on) start the ITCM at
    # 0x01000000, so on hardware an access below that aborts.
    #
    # This switch named the store. It was not MI_CpuFill8, as an earlier
    # uncommitted note here guessed, and no byte repair was needed once the
    # cause was known. It is func_02087350 writing a null handle (FF FF) back
    # through a handle pointer of 0x000000FF: the removed
    # projectile_damage_override, re-entered after a slice preemption, handed
    # the target resolver a leftover register as its argument (lb_patches.cpp,
    # apply_damage_observer).
    #
    # LB_NULLPAGE=1 reports every ARM9 data access below 0x01000000. It gives:
    #   * the exact pc (the banks set R15 at every instruction);
    #   * lr and r0-r7;
    #   * for the first 12, the shadow return stack (the callers).
    # The first report also prints the MPU regions the game programmed, which
    # checks the claim above. ITCM normally goes through the inline fast
    # window, which cannot see these accesses. Under this switch the window's
    # limit is 0, so ITCM goes through the slow path and every such access is
    # seen. LB_NULLPAGE=2 also drops the writes. That is an experiment, not a
    # fix. Off by default: the fast window and every access are unchanged.
    #
    # EXPECT TWO REPORTS IN EVERY RUN, and they are not the game's CPU: "R16/W16
    # 00000000 at pc 021100E8" is MI_StopDma(0)'s deliberate one-halfword DMA
    # from 0 to 0 (the NitroSDK's DMA0 workaround; pc is the bl after it).
    # The port's DMA reads and writes it through ITCM because resolve() maps
    # ITCM for every ARM9-side access. Hardware DMA cannot see the TCMs at
    # all. Harmless here, since it copies a halfword onto itself, but it is a
    # divergence: docs/369.
    replace(bus,
            "void unmapped(uint32_t addr, bool write, uint32_t width, uint32_t value) {\n",
            "/* legobattles-decomp (docs/369): see apply_hooks.py stanza 0zzz. */\n"
            "int lb_nullpage_mode() {\n"
            "    static int m = -1;\n"
            "    if (m < 0) {\n"
            "        const char* e = std::getenv(\"LB_NULLPAGE\");\n"
            "        m = (e && e[0] >= '1' && e[0] <= '9') ? (e[0] - '0') : 0;\n"
            "    }\n"
            "    return m;\n"
            "}\n"
            "/* True when the access should be dropped (LB_NULLPAGE=2, writes). */\n"
            "bool lb_nullpage_note(uint32_t addr, bool write, uint32_t width,\n"
            "                      uint32_t value) {\n"
            "    static unsigned n = 0;\n"
            "    ++n;\n"
            "    if (n == 1u) {\n"
            "        std::fprintf(stderr, \"[lb_null] MPU regions:\");\n"
            "        for (unsigned i = 0; i < 8u; ++i)\n"
            "            std::fprintf(stderr, \" %u=%08X\", i, cp15_debug_mpu_region(i));\n"
            "        std::fprintf(stderr, \" control=%08X\\n\", g_cp15.control);\n"
            "    }\n"
            "    if (n <= 48u || (n % 5000u) == 0u) {\n"
            "        std::fprintf(stderr,\n"
            "            \"[lb_null] #%u %s%u %08X = %08X at pc %08X lr %08X |\"\n"
            "            \" r0 %08X r1 %08X r2 %08X r3 %08X r4 %08X r5 %08X\"\n"
            "            \" r6 %08X r7 %08X sp %08X\\n\",\n"
            "            n, write ? \"W\" : \"R\", width * 8u, addr, value,\n"
            "            g_cpu.R[15], g_cpu.R[14], g_cpu.R[0], g_cpu.R[1],\n"
            "            g_cpu.R[2], g_cpu.R[3], g_cpu.R[4], g_cpu.R[5],\n"
            "            g_cpu.R[6], g_cpu.R[7], g_cpu.R[13]);\n"
            "        if (n <= 12u) {\n"
            "            const uint32_t d = runtime_call_stack_depth();\n"
            "            const uint32_t* s = runtime_call_stack_data();\n"
            "            std::fprintf(stderr, \"[lb_null]   callers, innermost first\"\n"
            "                                 \" (depth %u):\", d);\n"
            "            for (uint32_t i = 0; i < d && i < 16u; ++i)\n"
            "                std::fprintf(stderr, \" %08X\", s[d - 1u - i]);\n"
            "            std::fprintf(stderr, \"\\n\");\n"
            "        }\n"
            "        std::fflush(stderr);\n"
            "    }\n"
            "    return write && lb_nullpage_mode() >= 2;\n"
            "}\n"
            "\n"
            "void unmapped(uint32_t addr, bool write, uint32_t width, uint32_t value) {\n")
    for width in (4, 2, 1):
        replace(bus,
                "    ring_push(0, %d, addr, v);\n" % width,
                "    if (addr < 0x01000000u && g_nds_active == NDS_ARM9 &&   /* docs/369 */\n"
                "        lb_nullpage_mode() > 0)\n"
                "        lb_nullpage_note(addr, false, %du, v);\n"
                "    ring_push(0, %d, addr, v);\n" % (width, width))
    for width in (4, 2, 1):
        replace(bus,
                "    ring_push(1, %d, addr, val);\n" % width,
                "    ring_push(1, %d, addr, val);\n"
                "    if (addr < 0x01000000u && g_nds_active == NDS_ARM9 &&   /* docs/369 */\n"
                "        lb_nullpage_mode() > 0 &&\n"
                "        lb_nullpage_note(addr, true, %du, val))\n"
                "        return;\n" % (width, width))
    replace(bus,
            "            ? std::min<uint32_t>(g_cp15.itcm_size, 0x02000000u)\n"
            "            : 0u;\n",
            "            ? std::min<uint32_t>(g_cp15.itcm_size, 0x02000000u)\n"
            "            : 0u;\n"
            "    if (lb_nullpage_mode() > 0) g_busf_itcm_limit = 0u;   /* docs/369 */\n")
    done.append("bus.cpp: LB_NULLPAGE names the ARM9 accesses below "
                "0x01000000 (docs/369)")

    # 0zzzz. THE CARTRIDGE'S WIDGET HIT TEST MOVES WITH THE PICTURE (docs/371).
    #
    # Clicking within the original aspect still hit moved HUD
    # elements' original click points. The frontend used to correct HUD
    # clicks from outside the game. It rewrote a click on a moved element to
    # the element's native point (lb_hud_unmap_touch). It dropped a press on
    # the spot the element left (lb_hud_touch_phantom). Both read the last
    # composited frame's polygons, so both went blind on the frames the
    # cartridge draws no HUD. A dropped press is not the field click the
    # player aimed, either.
    #
    # The widget rectangle test (FUN_020EA1A4) is now answered for where the
    # widget is drawn (lb_patches.cpp widget_rect_observer, lb_hud.cpp
    # lb_hud_hit_unmap). While that is live, the frontend delivers the true
    # coordinate. A press on a moved button's face hits it. A press on the
    # vacated spot misses every widget and reaches the field. With the
    # correction off (Enhanced off, LB_HUDHIT=0, or a demo recorded before it)
    # this is the old path unchanged.
    replace(front,
            "if (!lb_hud_unmap_touch(&gxi, &gyi) &&",
            "if (!lb_hud_hit_live() &&   /* docs/371 */\n"
            "                                          "
            "!lb_hud_unmap_touch(&gxi, &gyi) &&")
    done.append("frontend.cpp: deliver true HUD clicks while the widget hit "
                "test is corrected (docs/371)")

    # 0zzzzz. 3D X LANDS ON THE PIXEL THE DS WOULD USE, PLUS THE BAND (docs/374).
    #
    # Sprites nudged at the world border. The DS maps a
    # whole-pixel vertex exactly: the ortho gives X = 32768*x - 2^22 with
    # w = 2^22, and the 256-wide viewport floors that to x. The widened path
    # instead does X*256/RenderWidth in SubmitVertex (a C truncating divide),
    # adds 2*w*shift/RenderWidth (another), and floors (posX*RW)/den here.
    # Modelled exactly (a sprites_vtxround.py model), 67.6% of whole-pixel
    # columns at RW 736 land 1 px LEFT of x + band, and 55-69% at 842, 960
    # and 1536. The pattern depends on the column and on the band separately.
    # So wherever the band moves against a still camera, a still sprite flips
    # by a pixel against its still ground: 18.4% of camera steps at 736.
    #
    # docs/373 removed the common case (the band no longer slides while the
    # camera pans into an edge). The rounding itself is removed here: a
    # 1/256 px bias before the floor makes every case land on x + band. The
    # model checked 1.32M whole-pixel and 1.32M half-pixel columns (odd-width
    # sprites, where the DS floors x.5 down) for every even RW from 258 to
    # 1728 and every band shift: 0 wrong (sprites_biasfix.py). Only the
    # widened viewport takes it; the native 256-wide path is untouched.
    # LB_VTXROUND=0 turns it off. Y is not this simple (a bias leaves 27% of
    # rows off; the model needs the exact form) and is left as it was.
    replace(os.path.join(runner_dir, "vendor", "melonds", "GPU3D.cpp"),
            "                s32 lb_px = (s32)((posX * viewport_width) / den)\n",
            "                /* docs/374: see apply_hooks.py stanza 0zzzzz. */\n"
            "                static const bool lb_vtxround = [] {\n"
            "                    const char* v = getenv(\"LB_VTXROUND\");\n"
            "                    return !(v && v[0] == '0');\n"
            "                }();\n"
            "                const u32 lb_round =\n"
            "                    (enhanced_full_viewport && lb_vtxround) ? den / 256u : 0u;\n"
            "                s32 lb_px = (s32)((posX * viewport_width + lb_round) / den)\n")
    done.append("GPU3D.cpp: widened 3D X rounds to the DS's pixel plus the band "
                "(docs/374)")

    # 0zzzzzz. THE HUD IS RASTERISED ON ITS OWN (docs/375).
    #
    # Sprites partially truncated (the Eagle Command). lb_hud moves
    # the in-field HUD to the window's corners by moving PIXELS of the 3D
    # line, and it had to vacate each moved element's native rect by zeroing
    # every opaque pixel there -- whoever drew it. Measured on a held battle
    # with the base selected: the Eagle Command and its health bar lost
    # exactly the brick column's native rect (native x 4..20, y 16..64). A
    # world pixel that showed through a transparent part of an element also
    # travelled to the corner with it. Neither can be told apart in one flat
    # line, and the pixels the HUD covered on the DS were never drawn at all.
    #
    # So the soft renderer draws the HUD's polygons -- lb_hud's own test,
    # min FinalZ under hud_z_threshold() -- after the world's, into a second
    # colour/depth/attribute set, on the rows they cover, and merges them back
    # with the renderer's own AlphaBlend once each row's final passes are done.
    # GetLine therefore hands every consumer the same picture as before, and
    # lb_SplitRows gives lb_hud_shift_line the world-only row (to vacate with)
    # and the HUD-only row (to move) behind a row it holds. Only on widened
    # frames (RenderWidth > 256), only with edge marking off (its neighbour
    # tests would see the empty HUD set), only while Enhanced and HUD anchoring
    # are on; LB_HUDSPLIT=0 turns it off. A row the compositor holds as a COPY
    # (LB_PAIR, LB_SNAPCOPY, a scrolled 3D layer) is not in the colour store,
    # so it takes the old path.
    soft_h = os.path.join(runner_dir, "vendor", "melonds", "GPU3D_Soft.h")
    soft_cpp = os.path.join(runner_dir, "vendor", "melonds", "GPU3D_Soft.cpp")
    replace(soft_h,
            "    bool OwnsColorLine(const u32* line) const noexcept;   /* docs/359 */\n",
            "    bool OwnsColorLine(const u32* line) const noexcept;   /* docs/359 */\n"
            "    /* docs/375: the world-only and HUD-only rows behind a row\n"
            "       GetLine handed out. 0: not a split frame. 1: both rows,\n"
            "       valid in [x0, x1). 2: a split frame with no HUD there. */\n"
            "    int lb_SplitRows(const u32* line, const u32** world,\n"
            "                     const u32** hud, int* x0, int* x1) const noexcept;\n")
    replace(soft_h,
            "    u32 DepthBuffer[BufferSize * 2];\n"
            "    u32 AttrBuffer[BufferSize * 2];\n",
            "    /* legobattles-decomp docs/375: pointers, so the HUD's polygons\n"
            "       can be drawn into a set of their own. The HUD colour set is\n"
            "       per colour store, like ColorStore; depth and attributes are\n"
            "       only needed while a frame renders. See apply_hooks.py. */\n"
            "    u32 lb_DepthStore[BufferSize * 2];\n"
            "    u32 lb_AttrStore[BufferSize * 2];\n"
            "    u32* DepthBuffer = lb_DepthStore;\n"
            "    u32* AttrBuffer = lb_AttrStore;\n"
            "    u32 lb_HudColor[2][BufferSize * 2];\n"
            "    u32 lb_HudDepth[BufferSize * 2];\n"
            "    u32 lb_HudAttr[BufferSize * 2];\n"
            "    u32 lb_WorldOut[2][BufferSize];\n"
            "    u8 lb_RowSplit[2][NumScanlines];\n"
            "    u8 lb_FrameSplit[2] = {0, 0};\n"
            "    int lb_HudX0[2] = {0, 0};\n"
            "    int lb_HudX1[2] = {0, 0};\n"
            "    u8 lb_PolyHud[2048];\n"
            "    u16 lb_HudIdx[2048];\n"
            "    int lb_HudCount = 0;\n"
            "    bool lb_SplitFrame = false;\n"
            "    int lb_Slot = 0;\n"
            "    u32 lb_ClearZ = 0;\n"
            "    u32 lb_ClearAttr = 0;\n"
            "    void lb_UseHudBuffers(bool hud) noexcept;\n"
            "    void lb_FinishRow(const GPU3D& gpu3d, s32 y);\n")
    replace(soft_cpp,
            "#include \"GPU.h\"\n",
            "#include \"GPU.h\"\n"
            "\n"
            "/* legobattles-decomp docs/375: src/lb_hud.cpp decides what the HUD\n"
            "   is and whether it is drawn on its own. */\n"
            "extern \"C\" unsigned lb_hud_z_threshold(void);\n"
            "extern \"C\" int lb_hud_split_wanted(void);\n")
    replace(soft_cpp,
            "    ColorBuffer = (DisplayedBuffer == ColorStore[0]) ? ColorStore[1]\n"
            "                                                     : ColorStore[0];\n",
            "    ColorBuffer = (DisplayedBuffer == ColorStore[0]) ? ColorStore[1]\n"
            "                                                     : ColorStore[0];\n"
            "    lb_Slot = (ColorBuffer == ColorStore[0]) ? 0 : 1;   /* docs/375 */\n")
    replace(soft_cpp,
            "void SoftRenderer::Reset(GPU& gpu)\n"
            "{\n",
            "/* legobattles-decomp docs/375: THE HUD IS RASTERISED ON ITS OWN.\n"
            "   See apply_hooks.py stanza 0zzzzzz. */\n"
            "void SoftRenderer::lb_UseHudBuffers(bool hud) noexcept\n"
            "{\n"
            "    if (hud)\n"
            "    {\n"
            "        ColorBuffer = lb_HudColor[lb_Slot];\n"
            "        DepthBuffer = lb_HudDepth;\n"
            "        AttrBuffer = lb_HudAttr;\n"
            "    }\n"
            "    else\n"
            "    {\n"
            "        ColorBuffer = ColorStore[lb_Slot];\n"
            "        DepthBuffer = lb_DepthStore;\n"
            "        AttrBuffer = lb_AttrStore;\n"
            "    }\n"
            "}\n"
            "\n"
            "/* Both final passes of one row, then the HUD blended back over the\n"
            "   world exactly as a translucent pixel is plotted, keeping the\n"
            "   world-only row for lb_SplitRows. */\n"
            "void SoftRenderer::lb_FinishRow(const GPU3D& gpu3d, s32 y)\n"
            "{\n"
            "    ScanlineFinalPass(gpu3d, y);\n"
            "    if (!lb_SplitFrame || y < 0 || y >= NumScanlines ||\n"
            "        !lb_RowSplit[lb_Slot][y])\n"
            "        return;\n"
            "    lb_UseHudBuffers(true);\n"
            "    ScanlineFinalPass(gpu3d, y);\n"
            "    lb_UseHudBuffers(false);\n"
            "    const u32* hud = lb_HudColor[lb_Slot];\n"
            "    u32* world = lb_WorldOut[lb_Slot];\n"
            "    const int base = FirstPixelOffset + y * ScanlineWidth;\n"
            "    for (int x = lb_HudX0[lb_Slot]; x < lb_HudX1[lb_Slot]; x++)\n"
            "    {\n"
            "        const int a = base + x;\n"
            "        const u32 w = ColorBuffer[a];\n"
            "        world[a] = w;\n"
            "        const u32 h = hud[a];\n"
            "        if (h >> 24)\n"
            "            ColorBuffer[a] = AlphaBlend(gpu3d, h, w, h >> 24);\n"
            "    }\n"
            "}\n"
            "\n"
            "int SoftRenderer::lb_SplitRows(const u32* line, const u32** world,\n"
            "                               const u32** hud, int* x0,\n"
            "                               int* x1) const noexcept\n"
            "{\n"
            "    for (int s = 0; s < 2; s++)\n"
            "    {\n"
            "        const u32* lo = &ColorStore[s][0];\n"
            "        if (line < lo || line >= lo + BufferSize) continue;\n"
            "        const long off = (long)(line - lo) - FirstPixelOffset;\n"
            "        if (off < 0 || (off % ScanlineWidth) != 0) return 0;\n"
            "        const long row = off / ScanlineWidth;\n"
            "        if (row >= NumScanlines || !lb_FrameSplit[s]) return 0;\n"
            "        if (!lb_RowSplit[s][row]) return 2;\n"
            "        if (world) *world = &lb_WorldOut[s][off + FirstPixelOffset];\n"
            "        if (hud) *hud = &lb_HudColor[s][off + FirstPixelOffset];\n"
            "        if (x0) *x0 = lb_HudX0[s];\n"
            "        if (x1) *x1 = lb_HudX1[s];\n"
            "        return 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
            "\n"
            "void SoftRenderer::Reset(GPU& gpu)\n"
            "{\n"
            "    DepthBuffer = lb_DepthStore;   /* docs/375 */\n"
            "    AttrBuffer = lb_AttrStore;\n"
            "    lb_Slot = 0;\n"
            "    lb_SplitFrame = false;\n"
            "    lb_FrameSplit[0] = lb_FrameSplit[1] = 0;\n"
            "    memset(lb_RowSplit, 0, sizeof(lb_RowSplit));\n")
    replace(soft_cpp,
            "void SoftRenderer::RenderScanline(const GPU& gpu, s32 y, int npolys)\n"
            "{\n"
            "    for (int i = 0; i < npolys; i++)\n"
            "    {\n"
            "        RendererPolygon* rp = &PolygonList[i];\n",
            "void SoftRenderer::RenderScanline(const GPU& gpu, s32 y, int npolys)\n"
            "{\n"
            "    /* docs/375: on a split frame the HUD's polygons are skipped\n"
            "       here and drawn after the world's, into their own set. */\n"
            "    for (int i = 0; i < npolys; i++)\n"
            "    {\n"
            "        if (lb_SplitFrame && lb_PolyHud[i]) continue;\n"
            "        RendererPolygon* rp = &PolygonList[i];\n")
    replace(soft_cpp,
            "            else\n"
            "                RenderPolygonScanline(gpu, rp, y);\n"
            "        }\n"
            "    }\n"
            "}\n",
            "            else\n"
            "                RenderPolygonScanline(gpu, rp, y);\n"
            "        }\n"
            "    }\n"
            "    if (!lb_SplitFrame || y < 0 || y >= NumScanlines) return;\n"
            "    bool lb_any = false;\n"
            "    for (int k = 0; k < lb_HudCount; k++)\n"
            "    {\n"
            "        RendererPolygon* rp = &PolygonList[lb_HudIdx[k]];\n"
            "        Polygon* polygon = rp->PolyData;\n"
            "        if (!(y >= polygon->YTop && (y < polygon->YBottom || (y == polygon->YTop && polygon->YBottom == polygon->YTop))))\n"
            "            continue;\n"
            "        if (!lb_any)\n"
            "        {\n"
            "            /* The HUD set is cleared only on the rows, and in the\n"
            "               columns, the HUD's polygons reach. */\n"
            "            lb_any = true;\n"
            "            lb_UseHudBuffers(true);\n"
            "            const int base = FirstPixelOffset + y * ScanlineWidth;\n"
            "            for (int x = lb_HudX0[lb_Slot]; x < lb_HudX1[lb_Slot]; x++)\n"
            "            {\n"
            "                const int a = base + x;\n"
            "                ColorBuffer[a] = 0;\n"
            "                ColorBuffer[a + BufferSize] = 0;\n"
            "                DepthBuffer[a] = lb_ClearZ;\n"
            "                DepthBuffer[a + BufferSize] = lb_ClearZ;\n"
            "                AttrBuffer[a] = lb_ClearAttr;\n"
            "                AttrBuffer[a + BufferSize] = lb_ClearAttr;\n"
            "            }\n"
            "        }\n"
            "        RenderPolygonScanline(gpu, rp, y);\n"
            "    }\n"
            "    if (lb_any) lb_UseHudBuffers(false);\n"
            "    lb_RowSplit[lb_Slot][y] = lb_any ? 1 : 0;\n"
            "}\n")
    replace(soft_cpp,
            "        SetupPolygon(&PolygonList[j++], polygons[i]);\n"
            "    }\n",
            "        SetupPolygon(&PolygonList[j++], polygons[i]);\n"
            "    }\n"
            "\n"
            "    /* docs/375: which polygons are the HUD -- lb_hud's own test --\n"
            "       and whether this frame draws them on their own. */\n"
            "    lb_SplitFrame = false;\n"
            "    lb_HudCount = 0;\n"
            "    if (RenderWidth > 256 && !(gpu.GPU3D.RenderDispCnt & (1<<5)) &&\n"
            "        lb_hud_split_wanted())\n"
            "    {\n"
            "        const u32 zmax = lb_hud_z_threshold();\n"
            "        int hx0 = RenderWidth, hx1 = 0;\n"
            "        for (int i = 0; i < j; i++)\n"
            "        {\n"
            "            const Polygon* p = PolygonList[i].PolyData;\n"
            "            u32 minz = 0xFFFFFFFFu;\n"
            "            int px0 = 0x7FFFFFFF, px1 = -0x7FFFFFFF;\n"
            "            for (u32 v = 0; v < p->NumVertices; v++)\n"
            "            {\n"
            "                minz = std::min(minz, (u32)p->FinalZ[v]);\n"
            "                px0 = std::min(px0, (int)p->Vertices[v]->FinalPosition[0]);\n"
            "                px1 = std::max(px1, (int)p->Vertices[v]->FinalPosition[0]);\n"
            "            }\n"
            "            const bool hud = minz < zmax && !p->IsShadowMask && !p->IsShadow;\n"
            "            lb_PolyHud[i] = hud ? 1 : 0;\n"
            "            if (!hud) continue;\n"
            "            lb_HudIdx[lb_HudCount++] = (u16)i;\n"
            "            hx0 = std::min(hx0, px0 - 1);\n"
            "            hx1 = std::max(hx1, px1 + 2);\n"
            "        }\n"
            "        if (lb_HudCount > 0)\n"
            "        {\n"
            "            lb_SplitFrame = true;\n"
            "            lb_HudX0[lb_Slot] = std::max(0, hx0);\n"
            "            lb_HudX1[lb_Slot] = std::min(RenderWidth, hx1);\n"
            "            lb_ClearZ = ((gpu.GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;\n"
            "            lb_ClearAttr = (gpu.GPU3D.RenderClearAttr1 & 0x3F000000) |\n"
            "                           (gpu.GPU3D.RenderClearAttr1 & 0x8000);\n"
            "        }\n"
            "    }\n"
            "    lb_FrameSplit[lb_Slot] = lb_SplitFrame ? 1 : 0;\n"
            "    memset(lb_RowSplit[lb_Slot], 0, sizeof(lb_RowSplit[lb_Slot]));\n")
    replace(soft_cpp,
            "        ScanlineFinalPass(gpu.GPU3D, y-1);\n",
            "        lb_FinishRow(gpu.GPU3D, y-1);   /* docs/375 */\n")
    replace(soft_cpp,
            "    ScanlineFinalPass(gpu.GPU3D, RenderHeight - 1);\n",
            "    lb_FinishRow(gpu.GPU3D, RenderHeight - 1);   /* docs/375 */\n")
    # What the split costs, timed on the render thread itself: LB_3DWAIT=1
    # adds the mean render of split and of unsplit frames to its 300-frame
    # line, so LB_HUDSPLIT=ab (the two in turn, 600 frames each) compares them
    # under one load.
    replace(soft_cpp,
            "std::atomic<uint64_t> lb_line_us{0}, lb_line_n{0};\n",
            "std::atomic<uint64_t> lb_line_us{0}, lb_line_n{0};\n"
            "std::atomic<uint64_t> lb_split_us[2], lb_split_n[2];   /* docs/375 */\n"
            "void lb_3dwait_split(LbClock::time_point t0, bool split) {\n"
            "    lb_split_us[split ? 1 : 0] += lb_us_since(t0);\n"
            "    ++lb_split_n[split ? 1 : 0];\n"
            "}\n")
    replace(soft_cpp,
            "            if (lb_3dwait_on()) lb_3dwait_render(lb_r0);\n",
            "            if (lb_3dwait_on()) lb_3dwait_render(lb_r0);\n"
            "            if (lb_3dwait_on()) lb_3dwait_split(lb_r0, lb_SplitFrame);\n")
    replace(soft_cpp,
            "    lb_144_us = 0;\n"
            "    lb_144_n = 0;\n"
            "}\n",
            "    lb_144_us = 0;\n"
            "    lb_144_n = 0;\n"
            "    const uint64_t sn0 = lb_split_n[0].exchange(0);\n"
            "    const uint64_t su0 = lb_split_us[0].exchange(0);\n"
            "    const uint64_t sn1 = lb_split_n[1].exchange(0);\n"
            "    const uint64_t su1 = lb_split_us[1].exchange(0);\n"
            "    std::fprintf(stderr, \"[lb_3dwait] docs/375: render %.3f ms \"\n"
            "        \"unsplit (%llu), %.3f ms with the HUD split (%llu)\\n\",\n"
            "        sn0 ? su0 / 1000.0 / sn0 : 0.0, (unsigned long long)sn0,\n"
            "        sn1 ? su1 / 1000.0 / sn1 : 0.0, (unsigned long long)sn1);\n"
            "    std::fflush(stderr);\n"
            "}\n")
    edit(os.path.join(runner_dir, "src", "gpu3d.cpp"),
         "const uint32_t* nds_gpu3d_wide_attr_line(int line) {",
         ["/* legobattles-decomp docs/375: the world-only and HUD-only rows",
          "   behind a 3D row the compositor holds; see GPU3D_Soft.h. */",
          "extern \"C\" int nds_gpu3d_split_rows(const uint32_t* line,",
          "                                    const uint32_t** world,",
          "                                    const uint32_t** hud, int* x0,",
          "                                    int* x1) {",
          "    if (!line || g_nds.GPU.GPU3D.IsRendererAccelerated()) return 0;",
          "    auto* renderer = dynamic_cast<melonDS::SoftRenderer*>(",
          "        &g_nds.GPU.GPU3D.GetCurrentRenderer());",
          "    return renderer",
          "        ? renderer->lb_SplitRows(line, world, hud, x0, x1) : 0;",
          "}",
          ""],
         after=False)
    done.append("GPU3D_Soft: the HUD's polygons drawn into their own set and "
                "merged back; lb_SplitRows for lb_hud (docs/375)")

    # 0zzzzzzz. LB_SPULOG: HOW MANY SOUNDS ACTUALLY START (docs/376).
    #
    # Unit and building sounds did not play. The music is a
    # stream that keys its channels on once, so a key-on (SOUNDxCNT bit 31
    # going 0 -> 1) is, to a close approximation, one sound-effect note. This
    # counts them per channel and prints the counts every ten seconds of mixed
    # audio, so an A/B (LB_SNDCULL=0 against the widened window) can be read
    # off the log instead of by ear against the music.
    spu = os.path.join(runner_dir, "src", "spu.cpp")
    replace(spu,
            "#include <cstring>\n",
            "#include <cstring>\n"
            "#include <cstdio>    /* legobattles-decomp: LB_SPULOG (docs/376) */\n"
            "#include <cstdlib>\n")
    replace(spu,
            "struct Channel {\n",
            "static void lb_spu_keyon(const void* ch);   /* docs/376 */\n"
            "\n"
            "struct Channel {\n")
    replace(spu,
            "        if ((value & 0x80000000u) && !(old & 0x80000000u)) key_on = true;\n",
            "        if ((value & 0x80000000u) && !(old & 0x80000000u)) {\n"
            "            key_on = true;\n"
            "            lb_spu_keyon(this);   /* docs/376 */\n"
            "        }\n")
    replace(spu,
            "std::array<Channel, 16> g_channel{};\n",
            "std::array<Channel, 16> g_channel{};\n"
            "\n"
            "/* legobattles-decomp docs/376: LB_SPULOG=1 counts key-ons per\n"
            "   channel; see apply_hooks.py stanza 0zzzzzzz. */\n"
            "static bool lb_spulog_on() {\n"
            "    static const bool on = std::getenv(\"LB_SPULOG\") != nullptr;\n"
            "    return on;\n"
            "}\n"
            "static unsigned g_lb_keyons[16];\n"
            "static void lb_spu_keyon(const void* ch) {\n"
            "    if (!lb_spulog_on()) return;\n"
            "    const long i = static_cast<const Channel*>(ch) - g_channel.data();\n"
            "    if (i >= 0 && i < 16) ++g_lb_keyons[i];\n"
            "}\n")
    replace(spu,
            "    while (g_mix_count < due) { mix(); ++g_mix_count; }\n",
            "    while (g_mix_count < due) { mix(); ++g_mix_count; }\n"
            "    /* docs/376: every ten seconds of mixed audio (32768 mixes a\n"
            "       second), the key-ons since the last line. */\n"
            "    static uint64_t lb_spulog_next = 327680u;\n"
            "    if (lb_spulog_on() && g_mix_count >= lb_spulog_next) {\n"
            "        lb_spulog_next = g_mix_count + 327680u;\n"
            "        unsigned total = 0;\n"
            "        char buf[256];\n"
            "        int n = 0;\n"
            "        for (int i = 0; i < 16; ++i) {\n"
            "            total += g_lb_keyons[i];\n"
            "            n += std::snprintf(buf + n, sizeof buf - (size_t)n,\n"
            "                               \" %u\", g_lb_keyons[i]);\n"
            "            g_lb_keyons[i] = 0;\n"
            "        }\n"
            "        std::fprintf(stderr, \"[lb_spulog] key-ons in the last 10 s: \"\n"
            "                     \"%u, by channel:%s\\n\", total, buf);\n"
            "        std::fflush(stderr);\n"
            "    }\n")
    done.append("spu.cpp: LB_SPULOG counts channel key-ons (docs/376)")

    # 0zzzzzzzz. UI LAYER 0 IS DRAWN WITH THE HUD, AND FLAGGED (docs/380).
    #
    # The shroud and the placement footprint are in front of the 3D layer, and
    # the cartridge clears their cells under every UI element on its own
    # 32x24 grid only. The placement checkmark docs/372 keeps in the band is a
    # UI element on layer 0 -- LB_POLYLIST during a placement: 16x15 at guest
    # x -164, depth 4,275,712 (z 502), the command icons' layer -- so the
    # footprint drew over it there. lb_hud covers HUD pixels outside the
    # cartridge's grid, but it can only see what the split's HUD set holds, and
    # layer 0 was left in the world set (it is not the HUD lb_hud moves).
    #
    # So the split takes layer 0 too (lb_hud_ui_threshold), and the merge flags
    # each HUD-set pixel whose depth is at or past the HUD's own threshold with
    # bit 31 of its colour -- above the 5-bit alpha, which nothing else reads
    # there. lb_hud moves and vacates only unflagged pixels, and covers all of
    # them in the band. Layer 0 is nearer than every world sprite (z <= 448),
    # so HUD-over-world merging gives the picture the single pass gave.
    replace(soft_cpp,
            "extern \"C\" int lb_hud_split_wanted(void);\n",
            "extern \"C\" int lb_hud_split_wanted(void);\n"
            "extern \"C\" unsigned lb_hud_ui_threshold(void);   /* docs/380 */\n")
    replace(soft_h,
            "    u32 lb_ClearAttr = 0;\n",
            "    u32 lb_ClearAttr = 0;\n"
            "    u32 lb_HudZ = 0;   /* docs/380: the HUD's own depth threshold */\n")
    replace(soft_cpp,
            "        const u32 zmax = lb_hud_z_threshold();\n",
            "        const u32 zmax = lb_hud_ui_threshold();   /* docs/380: + UI layer 0 */\n"
            "        lb_HudZ = lb_hud_z_threshold();\n")
    replace(soft_cpp,
            "    const u32* hud = lb_HudColor[lb_Slot];\n"
            "    u32* world = lb_WorldOut[lb_Slot];\n",
            "    u32* hud = lb_HudColor[lb_Slot];\n"
            "    u32* world = lb_WorldOut[lb_Slot];\n")
    replace(soft_cpp,
            "        const u32 h = hud[a];\n"
            "        if (h >> 24)\n"
            "            ColorBuffer[a] = AlphaBlend(gpu3d, h, w, h >> 24);\n",
            "        const u32 h = hud[a];\n"
            "        const u32 ha = (h >> 24) & 0x1F;\n"
            "        if (ha)\n"
            "        {\n"
            "            ColorBuffer[a] = AlphaBlend(gpu3d, h, w, ha);\n"
            "            /* docs/380: UI layer 0, not the HUD proper: flag it. */\n"
            "            if (lb_HudDepth[a] >= lb_HudZ) hud[a] = h | 0x80000000u;\n"
            "        }\n")
    done.append("GPU3D_Soft: UI layer 0 drawn with the HUD and flagged (docs/380)")

    # 0zzzzzzzzz. 3D LANDS ON THE DS'S OWN PIXEL, PLUS THE BAND (docs/381).
    #
    # Three faults in the widened viewport transform, one mechanism.
    #
    # 1. THE PRODUCT WRAPPED AT COLUMN 1024. G3_SetupScreenOrtho (020FA694)
    #    loads OrthoW(0,192,0,256,n,1024, scaleW 1024.0), so w = 2^22 and,
    #    after the divider's >>1, posX*RenderWidth is column*2^22: in u32 it
    #    passed 2^32 at surface column 1024 and every vertex right of it
    #    landed 1024 columns LEFT. Any surface 1025..1728 wide (MaxRenderWidth)
    #    -- e.g. 1360 and 1720 -- lost its 3D past that column and drew
    #    it over the left of the picture. Rows would wrap past 1024 too, but
    #    MaxRenderHeight is 768. The widened path now multiplies in 64 bits;
    #    the native path keeps the DS's 32-bit product.
    #
    # 2. Y WAS ONLY APPROXIMATELY THE DS's ROW (docs/374 fixed X by a bias; a
    #    bias leaves 27% of rows off, sprites_biasfix_y2.py). Each vertex now
    #    carries its UNSCALED clip X/Y (lb_Native, from the matrix before the
    #    widened Y scaling, interpolated through the clipper), and the
    #    transform runs the DS's own viewport arithmetic on it -- the same >>1,
    #    the same floor -- in signed 64 bits, then adds the latched integer
    #    band (the same g_list_left/top the clip-space shift used). Exact by
    #    construction wherever the DS draws the vertex. A clipped vertex on the
    #    surface's edge keeps the edge, or the edge would gain a 1-px gap.
    #    LB_VTXEXACT=0 turns this off (docs/374's rounding, still 64-bit).
    #
    # 3. THE GUEST COULD READ THE WIDENED MATRIX. melonDS answers CLIPMTX_RESULT
    #    (0x04000640..) and PosTest from ClipMatrix, whose Y column the widened
    #    frustum scales. The cartridge links G3X_GetClipMtx (0210C350), called
    #    from NitroSystem's billboard SBC handlers, which read the position
    #    matrix that way (identity projection) and load a rebuilt one back --
    #    to be scaled a second time. The guest now reads the DS's own matrix.
    #    LB_CLIPREAD=scaled restores the old answer; LB_CLIPREAD=ab alternates
    #    them every 600 composited frames, for an A/B within one session.
    #    BoxTest is only counted.
    #    LB_GXREADLOG=1 says whether any of these run; LB_VTXLOG=1 counts the
    #    vertices each fault touches.
    mel_gpu3d_h = os.path.join(runner_dir, "vendor", "melonds", "GPU3D.h")
    mel_gpu3d_cpp = os.path.join(runner_dir, "vendor", "melonds", "GPU3D.cpp")
    replace(mel_gpu3d_h,
            "    bool Clipped;\n",
            "    bool Clipped;\n"
            "\n"
            "    // legobattles-decomp docs/381: the clip X and Y the DS itself\n"
            "    // computes, before the widened frustum scales and shifts them.\n"
            "    s32 lb_Native[2];\n")
    replace(mel_gpu3d_h,
            "    bool ClipMatrixDirty = false;\n",
            "    bool ClipMatrixDirty = false;\n"
            "    /* legobattles-decomp docs/381: ProjMatrix x PosMatrix before the\n"
            "       widened frustum scales its Y column -- the DS's own matrix. */\n"
            "    s32 lb_ClipNative[16] {};\n")
    replace(mel_gpu3d_cpp,
            "    MatrixMult4x4(ClipMatrix, PosMatrix);\n",
            "    MatrixMult4x4(ClipMatrix, PosMatrix);\n"
            "    memcpy(lb_ClipNative, ClipMatrix, 16*4);   /* docs/381 */\n")
    edit(mel_gpu3d_cpp, "void GPU3D::UpdateClipMatrix() noexcept", [
        "/* legobattles-decomp docs/381: THE DS'S OWN VIEWPORT ARITHMETIC, EXTENDED.",
        "",
        "   The widened transform scales clip X by 256/RenderWidth (SubmitVertex)",
        "   and the clip matrix's Y column by (192<<12)/RenderHeight",
        "   (UpdateClipMatrix), shifts both by the band in clip space, and floors",
        "   (pos*RW)/den. Each step truncates, so a vertex lands only near the",
        "   DS's pixel plus the band. This computes the DS's answer instead: the",
        "   vertex's unscaled clip coordinate through the native viewport's own",
        "   arithmetic (the >>1 when w > 0xFFFF, the floor), in signed 64 bits so",
        "   it continues past the native frustum. See apply_hooks.py 0zzzzzzzzz. */",
        "static inline s32 lb_ds_axis(s64 pos, u32 w, u32 size) noexcept",
        "{",
        "    s64 den = w;",
        "    if (w > 0xFFFF)",
        "    {",
        "        pos >>= 1;",
        "        den >>= 1;",
        "    }",
        "    den <<= 1;",
        "    if (den <= 0) return 0;",
        "    const s64 num = pos * (s64)size;",
        "    s64 q = num / den;",
        "    if ((num % den) != 0 && num < 0) --q;",
        "    return (s32)q;",
        "}",
        "",
        "static bool lb_vtx_exact() noexcept",
        "{",
        "    static const bool on = [] {",
        "        const char* v = getenv(\"LB_VTXEXACT\");",
        "        return !(v && v[0] == '0');",
        "    }();",
        "    return on;",
        "}",
        "",
        "static bool lb_vtx_log_on() noexcept",
        "{",
        "    static const bool on = getenv(\"LB_VTXLOG\") != nullptr;",
        "    return on;",
        "}",
        "",
        "/* LB_VTXLOG=1: a sample line at the 1st, 2nd, 4th, 8th... occurrence of",
        "   each case, with the running count of widened vertices alongside.",
        "   kind -1 counts a vertex; 0 is a product that wrapped at 2^32 before",
        "   docs/381; 1 is the exact pixel differing from the rounded one; 2 is a",
        "   clipped edge vertex keeping the edge. */",
        "static void lb_vtx_note(int kind, int axis, s32 native, s32 band,",
        "                        s32 old_px, s32 new_px, bool clipped) noexcept",
        "{",
        "    static u64 total[2];",
        "    static u64 count[3][2];",
        "    if (kind < 0)",
        "    {",
        "        ++total[axis];",
        "        return;",
        "    }",
        "    const u64 c = ++count[kind][axis];",
        "    if ((c & (c - 1)) != 0) return;",
        "    static const char* const what[3] = {",
        "        \"wrapped at 2^32 before docs/381\", \"exact differs from rounded\",",
        "        \"clipped edge kept\" };",
        "    fprintf(stderr, \"[lb_vtx] %c %s #%llu of %llu: native %d band %d \"",
        "            \"rounded %d exact %d%s\\n\", axis ? 'y' : 'x', what[kind],",
        "            (unsigned long long)c, (unsigned long long)total[axis],",
        "            native, band, old_px, new_px, clipped ? \" (clipped)\" : \"\");",
        "}",
        "",
        "static s32 lb_exact_axis(int axis, s32 native, u32 w, u32 guest_size,",
        "                         s32 band, s32 rounded, s32 limit,",
        "                         bool clipped) noexcept",
        "{",
        "    /* A vertex the clipper put ON the surface's edge keeps the edge: its",
        "       native coordinate is interpolated and can land a pixel inside,",
        "       which would open a one-pixel gap along the surface's edge. */",
        "    if (clipped && (rounded <= 0 || rounded >= limit))",
        "    {",
        "        if (lb_vtx_log_on())",
        "            lb_vtx_note(2, axis, native, band, rounded, rounded, true);",
        "        return rounded;",
        "    }",
        "    const s64 pos = axis ? ((s64)w - native) : ((s64)native + w);",
        "    s32 v = lb_ds_axis(pos, w, guest_size) + band;",
        "    if (v < 0) v = 0;",
        "    if (v > limit) v = limit;",
        "    if (lb_vtx_log_on() && v != rounded)",
        "        lb_vtx_note(1, axis, native, band, rounded, v, clipped);",
        "    return v;",
        "}",
        "",
        "/* LB_GXREADLOG=1: does the cartridge read back anything the widened",
        "   frustum changes? A line at the 1st, 2nd, 4th... occurrence of each. */",
        "static void lb_gxread_note(int kind, u32 addr, const s32* proj) noexcept",
        "{",
        "    static const bool on = getenv(\"LB_GXREADLOG\") != nullptr;",
        "    if (!on) return;",
        "    static u64 count[4];",
        "    const u64 c = ++count[kind];",
        "    if ((c & (c - 1)) != 0) return;",
        "    static const char* const what[4] = {",
        "        \"CLIPMTX_RESULT read\", \"POS_RESULT read\", \"PosTest\", \"BoxTest\" };",
        "    bool ident = proj != nullptr;",
        "    for (int i = 0; proj && i < 16; ++i)",
        "        if (proj[i] != ((i % 5) == 0 ? 0x1000 : 0)) ident = false;",
        "    fprintf(stderr, \"[lb_gxread] %s #%llu addr %08X%s\\n\", what[kind],",
        "            (unsigned long long)c, addr,",
        "            proj ? (ident ? \" (projection = identity)\"",
        "                          : \" (projection not identity)\") : \"\");",
        "}",
        "",
        "/* LB_CLIPREAD=scaled: the guest reads the widened frustum's matrix, as",
        "   before docs/381. LB_CLIPREAD=ab alternates the two every 600 composited",
        "   frames (native first), so one session shows a skinned model both ways",
        "   seconds apart: dump it mid-phase, at odd multiples of 300. */",
        "extern \"C\" unsigned lb_dump_frame_get(void);   /* src/lb_widemap.cpp */",
        "static bool lb_clipread_native() noexcept",
        "{",
        "    static const int mode = [] {",
        "        const char* v = getenv(\"LB_CLIPREAD\");",
        "        if (v && v[0] == 's') return 1;",
        "        if (v && v[0] == 'a') return 2;",
        "        return 0;",
        "    }();",
        "    if (mode != 2) return mode == 0;",
        "    const unsigned f = lb_dump_frame_get();",
        "    const bool native = ((f / 600u) & 1u) == 0u;",
        "    static int said = -1;",
        "    if (said != (int)native)",
        "    {",
        "        said = (int)native;",
        "        fprintf(stderr, \"[lb_clipread] ab: %s from dump frame %u\\n\",",
        "                native ? \"native\" : \"scaled\", f);",
        "    }",
        "    return native;",
        "}",
        ""],
        after=False)
    replace(mel_gpu3d_cpp,
            "    INTERPOLATE(Color[0]);\n",
            "    /* legobattles-decomp docs/381: the unscaled clip X/Y ride along. */\n"
            "    outbuf->lb_Native[0] = (s32)(vin->lb_Native[0] + (((s64)vout->lb_Native[0] - vin->lb_Native[0]) * factor_num) / factor_den);\n"
            "    outbuf->lb_Native[1] = (s32)(vin->lb_Native[1] + (((s64)vout->lb_Native[1] - vin->lb_Native[1]) * factor_num) / factor_den);\n"
            "\n"
            "    INTERPOLATE(Color[0]);\n")
    edit(mel_gpu3d_cpp,
         "vertextrans->Position[3] = (vertex[0]*ClipMatrix[3] + vertex[1]*ClipMatrix[7]",
         ["/* legobattles-decomp docs/381: the clip X/Y the DS computes, before",
          "   the widened frustum scales and shifts them. */",
          "vertextrans->lb_Native[0] = vertextrans->Position[0];",
          "vertextrans->lb_Native[1] = (s32)((vertex[0]*lb_ClipNative[1] + vertex[1]*lb_ClipNative[5] + vertex[2]*lb_ClipNative[9] + vertex[3]*lb_ClipNative[13]) >> 12);"])
    replace(mel_gpu3d_cpp,
            "                s32 lb_px = (s32)((posX * viewport_width + lb_round) / den)\n"
            "                          + (s32)viewport_x + lb_xbias;\n"
            "                if (lb_px < 0) lb_px = 0;\n"
            "                posX = (u32)lb_px;\n",
            "                /* docs/381: 64-bit on the widened path. w is 2^22 here,\n"
            "                   so this product is column*2^22 and passed 2^32 at\n"
            "                   column 1024. The native path keeps the DS's 32 bits. */\n"
            "                const u64 lb_xprod = enhanced_full_viewport\n"
            "                    ? (u64)posX * viewport_width\n"
            "                    : (u64)(u32)(posX * viewport_width);\n"
            "                s32 lb_px = (s32)((lb_xprod + lb_round) / den)\n"
            "                          + (s32)viewport_x + lb_xbias;\n"
            "                if (lb_px < 0) lb_px = 0;\n"
            "                if (enhanced_full_viewport && lb_vtx_log_on())\n"
            "                {\n"
            "                    lb_vtx_note(-1, 0, 0, 0, 0, 0, false);\n"
            "                    if (lb_xprod > 0xFFFFFFFFull)\n"
            "                        lb_vtx_note(0, 0, vtx->lb_Native[0], lb_band,\n"
            "                                    (s32)((u32)((u32)lb_xprod + lb_round) / den),\n"
            "                                    lb_px, vtx->Clipped);\n"
            "                }\n"
            "                if (enhanced_full_viewport && lb_vtx_exact())\n"
            "                    lb_px = lb_exact_axis(0, vtx->lb_Native[0], w, 256u,\n"
            "                                          lb_band, lb_px,\n"
            "                                          (s32)RenderWidth, vtx->Clipped);\n"
            "                posX = (u32)lb_px;\n")
    replace(mel_gpu3d_cpp,
            "                s32 lb_py = (s32)((posY * viewport_height) / den)\n"
            "                          + (s32)viewport_y + lb_ybias;\n"
            "                if (lb_py < 0) lb_py = 0;\n"
            "                posY = (u32)lb_py;\n",
            "                /* docs/381: 64-bit on the widened path, as X above. */\n"
            "                const u64 lb_yprod = enhanced_full_viewport_y\n"
            "                    ? (u64)posY * viewport_height\n"
            "                    : (u64)(u32)(posY * viewport_height);\n"
            "                s32 lb_py = (s32)(lb_yprod / den)\n"
            "                          + (s32)viewport_y + lb_ybias;\n"
            "                if (lb_py < 0) lb_py = 0;\n"
            "                if (enhanced_full_viewport_y && lb_vtx_log_on())\n"
            "                {\n"
            "                    lb_vtx_note(-1, 1, 0, 0, 0, 0, false);\n"
            "                    if (lb_yprod > 0xFFFFFFFFull)\n"
            "                        lb_vtx_note(0, 1, vtx->lb_Native[1], lb_band_y,\n"
            "                                    (s32)((u32)lb_yprod / den),\n"
            "                                    lb_py, vtx->Clipped);\n"
            "                }\n"
            "                if (enhanced_full_viewport_y && lb_vtx_exact())\n"
            "                    lb_py = lb_exact_axis(1, vtx->lb_Native[1], w, 192u,\n"
            "                                          lb_band_y, lb_py,\n"
            "                                          (s32)RenderHeight, vtx->Clipped);\n"
            "                posY = (u32)lb_py;\n")
    replace(mel_gpu3d_cpp,
            "    if (addr >= 0x04000640 && addr < 0x04000680)\n"
            "    {\n"
            "        UpdateClipMatrix();\n"
            "        return ClipMatrix[(addr & 0x3C) >> 2];\n"
            "    }\n",
            "    if (addr >= 0x04000640 && addr < 0x04000680)\n"
            "    {\n"
            "        UpdateClipMatrix();\n"
            "        /* docs/381: the DS's clip matrix, not the widened frustum's. */\n"
            "        lb_gxread_note(0, addr, ProjMatrix);\n"
            "        return lb_clipread_native() ? lb_ClipNative[(addr & 0x3C) >> 2]\n"
            "                                    : ClipMatrix[(addr & 0x3C) >> 2];\n"
            "    }\n")
    replace(mel_gpu3d_cpp,
            "u32 GPU3D::Read32(u32 addr) noexcept\n"
            "{\n",
            "u32 GPU3D::Read32(u32 addr) noexcept\n"
            "{\n"
            "    if (addr >= 0x04000620 && addr < 0x04000630)\n"
            "        lb_gxread_note(1, addr, nullptr);   /* docs/381 */\n")
    replace(mel_gpu3d_cpp,
            "    UpdateClipMatrix();\n"
            "    PosTestResult[0] = (vertex[0]*ClipMatrix[0] + vertex[1]*ClipMatrix[4] + vertex[2]*ClipMatrix[8] + vertex[3]*ClipMatrix[12]) >> 12;\n"
            "    PosTestResult[1] = (vertex[0]*ClipMatrix[1] + vertex[1]*ClipMatrix[5] + vertex[2]*ClipMatrix[9] + vertex[3]*ClipMatrix[13]) >> 12;\n"
            "    PosTestResult[2] = (vertex[0]*ClipMatrix[2] + vertex[1]*ClipMatrix[6] + vertex[2]*ClipMatrix[10] + vertex[3]*ClipMatrix[14]) >> 12;\n"
            "    PosTestResult[3] = (vertex[0]*ClipMatrix[3] + vertex[1]*ClipMatrix[7] + vertex[2]*ClipMatrix[11] + vertex[3]*ClipMatrix[15]) >> 12;\n",
            "    UpdateClipMatrix();\n"
            "    lb_gxread_note(2, 0x040005C4, nullptr);   /* docs/381 */\n"
            "    const s32* lb_cm = lb_clipread_native() ? lb_ClipNative : ClipMatrix;\n"
            "    PosTestResult[0] = (vertex[0]*lb_cm[0] + vertex[1]*lb_cm[4] + vertex[2]*lb_cm[8] + vertex[3]*lb_cm[12]) >> 12;\n"
            "    PosTestResult[1] = (vertex[0]*lb_cm[1] + vertex[1]*lb_cm[5] + vertex[2]*lb_cm[9] + vertex[3]*lb_cm[13]) >> 12;\n"
            "    PosTestResult[2] = (vertex[0]*lb_cm[2] + vertex[1]*lb_cm[6] + vertex[2]*lb_cm[10] + vertex[3]*lb_cm[14]) >> 12;\n"
            "    PosTestResult[3] = (vertex[0]*lb_cm[3] + vertex[1]*lb_cm[7] + vertex[2]*lb_cm[11] + vertex[3]*lb_cm[15]) >> 12;\n")
    replace(mel_gpu3d_cpp,
            "void GPU3D::BoxTest(const u32* params) noexcept\n"
            "{\n",
            "void GPU3D::BoxTest(const u32* params) noexcept\n"
            "{\n"
            "    lb_gxread_note(3, 0x040005C0, nullptr);   /* docs/381: counted only */\n")
    done.append("GPU3D.cpp: widened 3D on the DS's own pixel plus the band, the "
                "2^32 wrap at column 1024, the guest's clip-matrix reads (docs/381)")

    # 0zzzzzzzzzz. EVEN PIXELS AT A NON-INTEGER PRESENTATION SCALE (docs/382).
    #
    # The Enhanced field is presented at the cover scale max(win/surface),
    # nearest-neighbour. The surface is sized from the window at the pixel
    # scale, so that is normally ~2.00. But "never show more than the map"
    # floors the scale at max(win_w/world_w, win_h/world_h): on a 64-cell map
    # (1536 px) a 3440-wide window presents at 2.24x, and nearest sampling
    # gives 369 of 1536 columns and 148 of 616 rows three screen pixels
    # instead of two (a scalesim.py model) -- uneven lines, uneven dither,
    # a shimmer as the camera scrolls.
    #
    # When the scale is at least 0.05 from an integer, the field is drawn
    # "sharp": nearest into a render target at the next integer scale, then
    # one linear pass down to the same destination rect. Every source pixel
    # gets the same width, and only its edge is blended. Near-integer scales
    # (the usual 2.005) and the supersampling path are untouched; the rect,
    # and so touch mapping and the HUD's anchors, are unchanged.
    # LB_PRESENT=nearest restores the old path.
    front_cpp2 = os.path.join(runner_dir, "src", "frontend.cpp")
    edit(front_cpp2, "int lb_present_src_w[2] = {0, 0};   /* legobattles-decomp */", [
        "/* legobattles-decomp docs/382: the field at a non-integer scale, sharp.",
        "   See apply_hooks.py stanza 0zzzzzzzzzz. */",
        "void render_screen(FrontendPresentation& presentation, int screen,",
        "                   const SDL_Rect& destination);",
        "static SDL_Texture* lb_sharp_target = nullptr;",
        "static SDL_Renderer* lb_sharp_renderer = nullptr;",
        "static int lb_sharp_w = 0, lb_sharp_h = 0;",
        "static bool lb_sharp_failed = false;",
        "static bool lb_present_sharp_on() {",
        "    static const bool on = [] {",
        "        const char* v = std::getenv(\"LB_PRESENT\");",
        "        return !(v && v[0] == 'n');",
        "    }();",
        "    return on;",
        "}",
        "static void lb_render_field(FrontendPresentation& presentation,",
        "                            const SDL_Rect& destination, float scale,",
        "                            int gw, int gh) {",
        "    const int whole = (int)scale;",
        "    const float frac = scale - (float)whole;",
        "    SDL_Renderer* r = presentation.renderers[1];",
        "    SDL_Texture* src = presentation.textures[1];",
        "    if (!lb_present_sharp_on() || lb_sharp_failed || frac < 0.05f ||",
        "        frac > 0.95f || whole < 1 || gw <= 0 || gh <= 0 || !r || !src ||",
        "        presentation.sample_targets[1]) {",
        "        render_screen(presentation, 1, destination);",
        "        return;",
        "    }",
        "    /* A source already filtered linearly (antialiasing on) is the",
        "       player's choice of smoothing: leave it to the old path. */",
        "    SDL_ScaleMode lb_mode = SDL_ScaleModeNearest;",
        "    if (SDL_GetTextureScaleMode(src, &lb_mode) == 0 &&",
        "        lb_mode != SDL_ScaleModeNearest) {",
        "        render_screen(presentation, 1, destination);",
        "        return;",
        "    }",
        "    const int k = whole + 1;",
        "    const int tw = gw * k, th = gh * k;",
        "    if (!lb_sharp_target || lb_sharp_renderer != r ||",
        "        lb_sharp_w != tw || lb_sharp_h != th) {",
        "        /* A texture of a renderer that has since been replaced went",
        "           with it; only one of this renderer's own is destroyed. */",
        "        if (lb_sharp_target && lb_sharp_renderer == r)",
        "            SDL_DestroyTexture(lb_sharp_target);",
        "        lb_sharp_target = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,",
        "                                            SDL_TEXTUREACCESS_TARGET,",
        "                                            tw, th);",
        "        lb_sharp_renderer = r;",
        "        lb_sharp_w = tw;",
        "        lb_sharp_h = th;",
        "        if (!lb_sharp_target) {",
        "            std::fprintf(stderr, \"[lb_present] sharp target %dx%d failed \"",
        "                         \"(%s); presenting nearest\\n\", tw, th, SDL_GetError());",
        "            lb_sharp_failed = true;",
        "            render_screen(presentation, 1, destination);",
        "            return;",
        "        }",
        "        SDL_SetTextureScaleMode(lb_sharp_target, SDL_ScaleModeLinear);",
        "        std::fprintf(stderr, \"[lb_present] field %dx%d at %.3fx: sharp, \"",
        "                     \"an exact %dx prescale then one linear pass\\n\",",
        "                     gw, gh, (double)scale, k);",
        "    }",
        "    SDL_SetTextureScaleMode(src, SDL_ScaleModeNearest);",
        "    SDL_SetRenderTarget(r, lb_sharp_target);",
        "    const SDL_Rect lb_src{0, 0, gw, gh};",
        "    SDL_RenderCopy(r, src, &lb_src, nullptr);",
        "    SDL_SetRenderTarget(r, nullptr);",
        "    SDL_RenderCopy(r, lb_sharp_target, nullptr, &destination);",
        "}",
        ""],
        after=False)
    replace(front_cpp2,
            "            render_screen(presentation, 1, lb_only);\n",
            "            lb_render_field(presentation, lb_only, lb_s, lb_gw, lb_ghh);   /* docs/382 */\n")
    done.append("frontend.cpp: the field is presented sharp at a non-integer "
                "scale (docs/382)")

    # 0zzzzzzzzzzz. THE PLACEMENT FOOTPRINT ABOVE AND BELOW THE DS's ROWS
    # (docs/383).
    #
    # BG3 carries nothing but the placement footprint, and every BG3 row
    # outside the guest's own 192 was cleared outright -- so a site dragged
    # into the top or bottom band had no footprint while its checkmark
    # (docs/372) was still drawn beside it. lb_widemap's harvest now shifts
    # the guest's camera in both axes (lb_fp_band_shift2), so those rows ask
    # it first and draw nothing only where it has no footprint cell.
    gpu2d_383 = os.path.join(runner_dir, "src", "gpu2d.cpp")
    replace(gpu2d_383,
            "            if (engine == 0 && bg == 3 && lb_extra_row)\n"
            "                lb_offmap = true;\n",
            "            if (engine == 0 && bg == 3 && lb_extra_row) {\n"
            "                /* docs/383: the footprint, above and below. */\n"
            "                if (lb_fp_band_tile(lb_ht, lb_tr, &lb_e))\n"
            "                    { tile = lb_e; lb_done = true; }\n"
            "                else\n"
            "                    lb_offmap = true;\n"
            "            }\n")
    done.append("gpu2d.cpp: the placement footprint in the rows above and "
                "below the guest's own (docs/383)")

    # docs/383: A FAST CLICK RELEASED AT THE TOP-LEFT CORNER. A mouse click
    # whose button came up within two frames of the press is released later,
    # by the frame loop, so the guest sees it -- but that deferred release sent
    # nds_set_touch(0, 0, false): no out-of-band coordinate, native (0,0). The
    # cartridge acts on the RELEASE edge (set_touch_from_mouse's own note), so a
    # fast enough click landed at the native top-left. It now releases the way
    # every other release does, at the press position with its extended
    # coordinate.
    front_383 = os.path.join(runner_dir, "src", "frontend.cpp")
    replace(front_383,
            "        if (touch_release_pending && touch_frames_held >= 2) {\n"
            "            nds_set_touch(0, 0, false);\n"
            "            touch_release_pending = false;\n"
            "        }\n",
            "        if (touch_release_pending && touch_frames_held >= 2) {\n"
            "            /* docs/383: where it was pressed, with its out-of-band\n"
            "               coordinate, like every other release. */\n"
            "            set_touch_from_mouse(last_touch_event_x, last_touch_event_y,\n"
            "                                 false, options.screen_layout,\n"
            "                                 bottom_logical_width);\n"
            "            touch_release_pending = false;\n"
            "        }\n")
    done.append("frontend.cpp: a fast click is released where it was pressed "
                "(docs/383)")

    # 0zzzzzzzzzzzz. TEAM BUILDER AND BAN UNITS (docs/384).
    #
    # (a) THE PLAYER'S OWN ROM, for the army screen's head icons. main.cpp
    #     reads the whole cartridge into `rom`, a local of main() that is only
    #     ever read and outlives the frontend loop (which runs inside main), so
    #     lb_teams.cpp can read NitroFS files from the first frame without a
    #     second copy or any file I/O. Nothing cartridge-derived enters the
    #     repository.
    main_384 = os.path.join(runner_dir, "src", "main.cpp")
    replace(main_384,
            '#include "wifi_net.h"\n',
            '#include "wifi_net.h"\n'
            'extern "C" void lb_rom_attach(const unsigned char*, size_t);'
            '  /* docs/384 */\n')
    replace(main_384,
            "    auto rom = rom_path.empty() ? std::vector<uint8_t>{} : "
            "read_file(rom_path);\n",
            "    auto rom = rom_path.empty() ? std::vector<uint8_t>{} : "
            "read_file(rom_path);\n"
            "    lb_rom_attach(rom.empty() ? nullptr : rom.data(), rom.size());"
            "  /* docs/384 */\n")
    done.append("main.cpp: the ROM image is handed to lb_teams (docs/384)")

    # (b) THE HOST'S BANS REACH THE PLAYERS. Nothing carried a host-decided
    #     setting to a joining client before this (docs/384). Bans must be
    #     applied identically on every machine -- the game is lockstep, and
    #     each machine rebuilds every player's roster at the start -- so the
    #     host's list travels over the same bridge as the game's frames: a
    #     datagram of its own type, like the hello, sent every 250 ms while
    #     this instance hosts (lb_rules_outgoing decides) to the siblings and
    #     the remote peer, and taken off the socket before any frame parsing
    #     (lb_rules_incoming keeps it only while this instance is joining).
    wifi_384 = os.path.join(runner_dir, "src", "wifi_net.cpp")
    replace(wifi_384,
            "class LocalMpTransport {\n",
            "/* legobattles-decomp (docs/384): the host's rules. */\n"
            "extern \"C\" int lb_rules_outgoing(unsigned char* buf, int cap);\n"
            "extern \"C\" void lb_rules_incoming(const unsigned char* buf, "
            "int len,\n"
            "                                  unsigned int sender);\n"
            "\n"
            "class LocalMpTransport {\n")
    replace(wifi_384,
            "    static constexpr uint32_t kLbHello = 0x4C42484Fu;  /* LBHO */\n",
            "    static constexpr uint32_t kLbHello = 0x4C42484Fu;  /* LBHO */\n"
            "    /* docs/384: a host's rules (its ban list), never a frame. */\n"
            "    static constexpr uint32_t kLbRules = 0x4C425255u;  /* LBRU */\n"
            "    std::chrono::steady_clock::time_point lb_last_rules_{};\n"
            "    void LbMaybeRules() {\n"
            "        const auto now = std::chrono::steady_clock::now();\n"
            "        if (now - lb_last_rules_ < std::chrono::milliseconds(250))\n"
            "            return;\n"
            "        lb_last_rules_ = now;\n"
            "        uint8_t buf[64];\n"
            "        const int n = lb_rules_outgoing(buf, (int)sizeof buf);\n"
            "        if (n > 0) SendGeneric(kLbRules, buf, n, 0u);\n"
            "    }\n")
    replace(wifi_384,
            "        LbMaybeHello();   /* legobattles-decomp */\n",
            "        LbMaybeHello();   /* legobattles-decomp */\n"
            "        LbMaybeRules();   /* docs/384 */\n")
    replace(wifi_384,
            "            if (ReadLe32(datagram + 8u) == kLbHello) continue;\n",
            "            if (ReadLe32(datagram + 8u) == kLbHello) continue;\n"
            "            /* docs/384: a host's rules go to lb_teams, never to\n"
            "               the guest. */\n"
            "            if (ReadLe32(datagram + 8u) == kLbRules) {\n"
            "                const uint32_t rlen = ReadLe32(datagram + 12u);\n"
            "                if (kHeaderBytes + rlen == static_cast<uint32_t>(got))\n"
            "                    lb_rules_incoming(datagram + kHeaderBytes,\n"
            "                                      static_cast<int>(rlen),\n"
            "                                      ReadLe32(datagram + 4u));\n"
            "                continue;\n"
            "            }\n")
    done.append("wifi_net.cpp: a host's ban list travels to its players "
                "(docs/384)")

    # 0zzzzzzzzzzzzz-400. NAT TRAVERSAL (docs/400). The master server only
    # lists addresses, so a host behind a router was unreachable: the joiner's
    # first datagram met a router that had never seen the host send anywhere
    # (every hosting session of 2026-10-18 logged `recv=0`). Each side now
    # learns its PUBLIC UDP mapping from this socket -- a STUN binding request
    # (RFC 5389) answered with XOR-MAPPED-ADDRESS -- and, when the master
    # server introduces the two, sends hellos to the other's mapping, which
    # opens both routers for the game's frames. lb_net.cpp asks for both from
    # its worker threads (nds_local_wireless_stun / _punch); the transport
    # does the sending on its own thread, from DrainSocket like the hello.
    wifi_400 = os.path.join(runner_dir, "src", "wifi_net.cpp")
    edit(wifi_400, "/* legobattles-decomp (docs/384): the host's rules. */",
         ["/* legobattles-decomp (docs/400): NAT traversal state, shared with",
          "   lb_net.cpp's worker threads through the nds_local_wireless_*",
          "   functions below MP_Begin. */",
          "namespace lb_nat {",
          "std::mutex mu;",
          "uint32_t stun_ip = 0u;      /* network order */",
          "uint16_t stun_port = 0u;",
          "unsigned stun_gen = 0u;     /* bumped by every request */",
          "uint32_t pub_ip = 0u;       /* network order; 0 until answered */",
          "uint16_t pub_port = 0u;",
          "struct Punch {",
          "    uint32_t ip;",
          "    uint16_t port;",
          "    std::chrono::steady_clock::time_point until;",
          "};",
          "std::vector<Punch> punches;",
          "}  // namespace lb_nat",
          ""],
         after=False)
    edit(wifi_400, "    void LbMaybeRules() {",
         ["/* docs/400: STUN and punching (lb_nat above). */",
          "unsigned lb_bind_gen_ = 0u, lb_nat_bind_ = 0u;",
          "unsigned lb_stun_gen_ = 0u;",
          "int lb_stun_tries_ = 0;",
          "uint8_t lb_stun_txid_[12] = {};",
          "std::chrono::steady_clock::time_point lb_last_stun_{}, lb_last_punch_{};",
          "unsigned lb_remote_rx_ = 0u;",
          "void LbHelloTo(uint32_t ip_be, uint16_t port) {",
          "#ifdef _WIN32",
          "    std::vector<uint8_t> datagram;",
          "    WriteLe32(&datagram, kMagic);",
          "    WriteLe32(&datagram, instance_);",
          "    WriteLe32(&datagram, kLbHello);",
          "    WriteLe32(&datagram, 0u);",
          "    WriteLe64(&datagram, 0u);",
          "    sockaddr_in to{};",
          "    to.sin_family = AF_INET;",
          "    to.sin_addr.s_addr = ip_be;",
          "    to.sin_port = htons(port);",
          "    ::sendto(socket_, reinterpret_cast<const char*>(datagram.data()),",
          "             static_cast<int>(datagram.size()), 0,",
          "             reinterpret_cast<sockaddr*>(&to), sizeof(to));",
          "#endif",
          "}",
          "void LbNatTick() {",
          "#ifdef _WIN32",
          "    if (!enabled_ || socket_ == INVALID_SOCKET || !LbRemote()) return;",
          "    const auto now = std::chrono::steady_clock::now();",
          "    uint32_t sip = 0u;",
          "    uint16_t sport = 0u;",
          "    unsigned gen = 0u;",
          "    std::vector<lb_nat::Punch> targets;",
          "    {",
          "        std::lock_guard<std::mutex> lock(lb_nat::mu);",
          "        sip = lb_nat::stun_ip;",
          "        sport = lb_nat::stun_port;",
          "        gen = lb_nat::stun_gen;",
          "        /* A new socket is a new mapping: ask again. */",
          "        if (lb_nat_bind_ != lb_bind_gen_) {",
          "            lb_nat_bind_ = lb_bind_gen_;",
          "            lb_nat::pub_ip = 0u;",
          "            lb_nat::pub_port = 0u;",
          "            lb_stun_gen_ = gen - 1u;",
          "        }",
          "        for (size_t i = 0; i < lb_nat::punches.size();) {",
          "            if (lb_nat::punches[i].until <= now) {",
          "                lb_nat::punches.erase(lb_nat::punches.begin() +",
          "                                      static_cast<std::ptrdiff_t>(i));",
          "                continue;",
          "            }",
          "            targets.push_back(lb_nat::punches[i]);",
          "            ++i;",
          "        }",
          "    }",
          "    if (gen != lb_stun_gen_) {",
          "        lb_stun_gen_ = gen;",
          "        lb_stun_tries_ = 8;",
          "        const uint64_t seed = static_cast<uint64_t>(",
          "            now.time_since_epoch().count()) ^ (uint64_t)instance_ << 56;",
          "        for (int i = 0; i < 12; ++i)",
          "            lb_stun_txid_[i] = static_cast<uint8_t>(seed >> ((i % 8) * 8)) ^",
          "                               static_cast<uint8_t>(i * 37);",
          "    }",
          "    if (sip && sport && lb_stun_tries_ > 0 &&",
          "        now - lb_last_stun_ >= std::chrono::milliseconds(400)) {",
          "        lb_last_stun_ = now;",
          "        --lb_stun_tries_;",
          "        uint8_t req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};",
          "        std::memcpy(req + 8, lb_stun_txid_, 12);",
          "        sockaddr_in to{};",
          "        to.sin_family = AF_INET;",
          "        to.sin_addr.s_addr = sip;",
          "        to.sin_port = htons(sport);",
          "        ::sendto(socket_, reinterpret_cast<const char*>(req), 20, 0,",
          "                 reinterpret_cast<sockaddr*>(&to), sizeof(to));",
          "    }",
          "    if (!targets.empty() &&",
          "        now - lb_last_punch_ >= std::chrono::milliseconds(200)) {",
          "        lb_last_punch_ = now;",
          "        for (const lb_nat::Punch& p : targets) LbHelloTo(p.ip, p.port);",
          "    }",
          "#endif",
          "}",
          "/* A STUN answer to our own request: the public mapping. True for",
          "   anything that is a STUN answer, so it never reaches the game. */",
          "bool LbStunAnswer(const uint8_t* d, int got) {",
          "    if (got < 20 || d[0] != 0x01 || d[1] != 0x01) return false;",
          "    if (d[4] != 0x21 || d[5] != 0x12 || d[6] != 0xA4 || d[7] != 0x42)",
          "        return false;",
          "    if (std::memcmp(d + 8, lb_stun_txid_, 12) != 0) return true;",
          "    const int len = (d[2] << 8) | d[3];",
          "    int at = 20;",
          "    while (at + 4 <= got && at + 4 <= 20 + len) {",
          "        const int type = (d[at] << 8) | d[at + 1];",
          "        const int alen = (d[at + 2] << 8) | d[at + 3];",
          "        const uint8_t* v = d + at + 4;",
          "        if (at + 4 + alen > got) break;",
          "        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {",
          "            uint16_t port = static_cast<uint16_t>((v[2] << 8) | v[3]);",
          "            uint8_t ip[4] = {v[4], v[5], v[6], v[7]};",
          "            if (type == 0x0020) {",
          "                port ^= 0x2112u;",
          "                ip[0] ^= 0x21; ip[1] ^= 0x12; ip[2] ^= 0xA4; ip[3] ^= 0x42;",
          "            }",
          "            uint32_t ip_be = 0u;",
          "            std::memcpy(&ip_be, ip, 4);",
          "            bool changed = false;",
          "            {",
          "                std::lock_guard<std::mutex> lock(lb_nat::mu);",
          "                changed = lb_nat::pub_ip != ip_be || lb_nat::pub_port != port;",
          "                lb_nat::pub_ip = ip_be;",
          "                lb_nat::pub_port = port;",
          "            }",
          "            lb_stun_tries_ = 0;",
          "            if (changed)",
          "                melonDS::Platform::Log(melonDS::Platform::Info,",
          "                    \"[local_mp] public address %u.%u.%u.%u:%u (STUN)\\n\",",
          "                    (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2],",
          "                    (unsigned)ip[3], (unsigned)port);",
          "            if (type == 0x0020) return true;",
          "        }",
          "        at += 4 + ((alen + 3) & ~3);",
          "    }",
          "    return true;",
          "}"],
         after=False)
    replace(wifi_400,
            "        LbMaybeRules();   /* docs/384 */\n",
            "        LbMaybeRules();   /* docs/384 */\n"
            "        LbNatTick();      /* docs/400 */\n")
    replace(wifi_400,
            "            if (got < static_cast<int>(kHeaderBytes)) continue;\n"
            "            if (ReadLe32(datagram) != kMagic) continue;\n"
            "            /* legobattles-decomp: a HOST has no address to answer\n",
            "            if (LbStunAnswer(datagram, got)) continue;   /* docs/400 */\n"
            "            if (got < static_cast<int>(kHeaderBytes)) continue;\n"
            "            if (ReadLe32(datagram) != kMagic) continue;\n"
            "            /* legobattles-decomp: a HOST has no address to answer\n")
    edit(wifi_400, "            if (lb_listen_ && lb_peer_ip_ == 0u && !sibling) {",
         ["if (!sibling) ++lb_remote_rx_;   /* docs/400 */"], after=False)
    replace(wifi_400,
            "        melonDS::Platform::Log(melonDS::Platform::Info,\n"
            "            \"[local_mp] enabled instance=%u port=%u\\n\",",
            "        ++lb_bind_gen_;   /* docs/400: a new socket, a new mapping */\n"
            "        melonDS::Platform::Log(melonDS::Platform::Info,\n"
            "            \"[local_mp] enabled instance=%u port=%u\\n\",")
    edit(wifi_400, "    bool LbEnabled() const { return enabled_; }",
         ["void LbStats(unsigned* s, unsigned* r) const {",
          "    if (s) *s = lb_sent_;",
          "    if (r) *r = lb_remote_rx_;",
          "}"])
    edit(wifi_400, "extern \"C\" uint16_t nds_local_wireless_port(void) {",
         ["/* docs/400: NAT traversal, for lb_net.cpp. */",
          "extern \"C\" void nds_local_wireless_stun(uint32_t ip_be, uint16_t port) {",
          "    std::lock_guard<std::mutex> lock(lb_nat::mu);",
          "    lb_nat::stun_ip = ip_be;",
          "    lb_nat::stun_port = port;",
          "    ++lb_nat::stun_gen;",
          "}",
          "extern \"C\" int nds_local_wireless_public(uint32_t* ip_be, uint16_t* port) {",
          "    std::lock_guard<std::mutex> lock(lb_nat::mu);",
          "    if (ip_be) *ip_be = lb_nat::pub_ip;",
          "    if (port) *port = lb_nat::pub_port;",
          "    return lb_nat::pub_ip != 0u ? 1 : 0;",
          "}",
          "extern \"C\" void nds_local_wireless_punch(uint32_t ip_be, uint16_t port,",
          "                                         int ms) {",
          "    std::lock_guard<std::mutex> lock(lb_nat::mu);",
          "    const auto until = std::chrono::steady_clock::now() +",
          "                       std::chrono::milliseconds(ms);",
          "    for (lb_nat::Punch& p : lb_nat::punches)",
          "        if (p.ip == ip_be && p.port == port) {",
          "            p.until = until;",
          "            return;",
          "        }",
          "    if (lb_nat::punches.size() < 8u)",
          "        lb_nat::punches.push_back({ip_be, port, until});",
          "}",
          "extern \"C\" void nds_local_wireless_stats(unsigned* sent, unsigned* recvd) {",
          "    if (sent) *sent = 0u;",
          "    if (recvd) *recvd = 0u;",
          "    if (g_bridge) g_bridge->local_mp.LbStats(sent, recvd);",
          "}",
          ""],
         after=False)
    done.append("wifi_net.cpp: STUN and hole punching for the bridge (docs/400)")

    # 0zzzzzzzzzzzzz-401. A SAVE FROM ANY REGION, ANY TOOL, UNDER ANY ROM NAME
    # (docs/401; the functions are in lb_setup.cpp). The loader takes another
    # region's tag over to this build's, and a padded or DeSmuME file's first
    # 512 bytes, instead of ignoring the file and later writing a blank save
    # over it. main.cpp gives the retail cartridge one fixed save name.
    io_401 = os.path.join(runner_dir, "src", "io.cpp")
    edit(io_401, "std::string g_cart_save_path;",
         ["/* legobattles-decomp docs/401 (lb_setup.cpp). */",
          "extern \"C\" int lb_save_adopt(unsigned char* sram, size_t size,",
          "                              const char* path);",
          "extern \"C\" int lb_save_lenient_load(const char* path,",
          "                                     unsigned char* sram, size_t size);"],
         after=False)
    replace(io_401,
            "                case NdsBatterySaveLoadResult::Loaded:\n"
            "                    std::fprintf(stderr,\n"
            "                                 \"[save] loaded %zu bytes from %s\\n\",\n"
            "                                 g_cart_backup.sram.size(),\n"
            "                                 g_cart_save_path.c_str());\n"
            "                    break;\n",
            "                case NdsBatterySaveLoadResult::Loaded:\n"
            "                    std::fprintf(stderr,\n"
            "                                 \"[save] loaded %zu bytes from %s\\n\",\n"
            "                                 g_cart_backup.sram.size(),\n"
            "                                 g_cart_save_path.c_str());\n"
            "                    if (lb_save_adopt(g_cart_backup.sram.data(),\n"
            "                                      g_cart_backup.sram.size(),\n"
            "                                      g_cart_save_path.c_str()))\n"
            "                        g_cart_backup.dirty = true;   /* docs/401 */\n"
            "                    break;\n")
    replace(io_401,
            "                case NdsBatterySaveLoadResult::InvalidSize:\n"
            "                case NdsBatterySaveLoadResult::Error:\n"
            "                    std::fprintf(stderr,\n",
            "                case NdsBatterySaveLoadResult::InvalidSize:\n"
            "                    /* docs/401: a padded or DeSmuME file. */\n"
            "                    if (lb_save_lenient_load(g_cart_save_path.c_str(),\n"
            "                                             g_cart_backup.sram.data(),\n"
            "                                             g_cart_backup.sram.size())) {\n"
            "                        lb_save_adopt(g_cart_backup.sram.data(),\n"
            "                                      g_cart_backup.sram.size(),\n"
            "                                      g_cart_save_path.c_str());\n"
            "                        g_cart_backup.dirty = true;\n"
            "                        break;\n"
            "                    }\n"
            "                    [[fallthrough]];\n"
            "                case NdsBatterySaveLoadResult::Error:\n"
            "                    std::fprintf(stderr,\n")
    main_401 = os.path.join(runner_dir, "src", "main.cpp")
    replace(main_401,
            'extern "C" void lb_rom_attach(const unsigned char*, size_t);'
            '  /* docs/384 */\n',
            'extern "C" void lb_rom_attach(const unsigned char*, size_t);'
            '  /* docs/384 */\n'
            'extern "C" int lb_retail_save_path(const char* derived,'
            ' const char* rom_path, const char* rom_sha1, char* out, int cap);'
            '  /* docs/401 */\n')
    replace(main_401,
            "        } else if (interactive) {\n"
            "            std::filesystem::path derived(rom_path);\n"
            "            derived.replace_extension(\".sav\");\n"
            "            save_path = derived.string();\n"
            "        }\n"
            "    }\n",
            "        } else if (interactive) {\n"
            "            std::filesystem::path derived(rom_path);\n"
            "            derived.replace_extension(\".sav\");\n"
            "            save_path = derived.string();\n"
            "        }\n"
            "        /* legobattles-decomp docs/401: the retail cartridge's save\n"
            "           does not depend on what its ROM file is called. */\n"
            "        char lb_fixed[1024];\n"
            "        if (!save_path.empty() &&\n"
            "            lb_retail_save_path(save_path.c_str(), rom_path.c_str(),\n"
            "                                rom_sha1.c_str(), lb_fixed,\n"
            "                                (int)sizeof lb_fixed))\n"
            "            save_path = lb_fixed;\n"
            "    }\n")
    done.append("io.cpp/main.cpp: saves from any region, tool or ROM name "
                "(docs/401)")

    # 0zzzzzzzzzzzzz. A DEBUG MOUSE WHEEL (docs/385). Zooming in/out while a
    # menu is open could offset what clicks what in it. The wheel
    # zooms (lb_overlay), and a harness could not turn it: frontend_input had
    # clicks, motion and buttons but no wheel. `action=wheel` puts the cursor
    # at x,y (window pixels, as `click`) and pushes one SDL_MOUSEWHEEL of dy
    # notches -- the player's own path through lb_overlay_event.
    front_385 = os.path.join(runner_dir, "src", "frontend.cpp")
    replace(front_385,
            "extern \"C\" bool nds_frontend_debug_move(int x, int y) {\n",
            "/* legobattles-decomp docs/385: one wheel event at a window point. */\n"
            "extern \"C\" bool nds_frontend_debug_wheel(int x, int y, int dy) {\n"
            "#if defined(NDS_HAVE_SDL2)\n"
            "    if (!g_input_debug.active) return false;\n"
            "    SDL_Event motion{};\n"
            "    motion.type = SDL_MOUSEMOTION;\n"
            "    motion.motion.windowID = g_input_debug.bottom_window_id;\n"
            "    motion.motion.x = x;\n"
            "    motion.motion.y = y;\n"
            "    SDL_PushEvent(&motion);\n"
            "    SDL_Event event{};\n"
            "    event.type = SDL_MOUSEWHEEL;\n"
            "    event.wheel.windowID = g_input_debug.bottom_window_id;\n"
            "    event.wheel.y = dy;\n"
            "    event.wheel.preciseY = (float)dy;\n"
            "    event.wheel.mouseX = x;\n"
            "    event.wheel.mouseY = y;\n"
            "    return SDL_PushEvent(&event) == 1;\n"
            "#else\n"
            "    (void)x; (void)y; (void)dy;\n"
            "    return false;\n"
            "#endif\n"
            "}\n"
            "\n"
            "extern \"C\" bool nds_frontend_debug_move(int x, int y) {\n")
    dbgsrv_385 = os.path.join(runner_dir, "src", "debug_server.cpp")
    replace(dbgsrv_385,
            "extern \"C\" bool nds_frontend_debug_move(int x, int y);\n",
            "extern \"C\" bool nds_frontend_debug_move(int x, int y);\n"
            "extern \"C\" bool nds_frontend_debug_wheel(int x, int y, int dy);\n")
    replace(dbgsrv_385,
            "        } else if (action == \"touch\") {",
            "        } else if (action == \"wheel\") {   /* docs/385 */\n"
            "            ok = nds_frontend_debug_wheel(\n"
            "                static_cast<int>(json_i64(line, \"x\", 0)),\n"
            "                static_cast<int>(json_i64(line, \"y\", 0)),\n"
            "                static_cast<int>(json_i64(line, \"dy\", 0)));\n"
            "        } else if (action == \"touch\") {")
    done.append("frontend.cpp/debug_server.cpp: a debug mouse wheel (docs/385)")

    # 0zzzzzzzzzzzzzz. THE GAME KEEPS RUNNING WHILE ITS WINDOW IS MOVED
    # (docs/385). The game froze while its window was being moved, which
    # would be a problem for online matches. Windows runs its own modal
    # loop for a title-bar drag, a border drag or the system menu, inside the
    # dispatch that SDL_PollEvent makes, so the frame loop -- emulation,
    # presentation, the network -- stops until the button is released.
    # src/lb_winloop.cpp runs the pump on a second fiber and switches back to
    # the frame loop on a timer while such a loop runs. Here the frontend's
    # SDL_PollEvent loop becomes that pump plus a drain of the queue; SDL
    # pumps once per SDL_PollEvent cycle as well, so outside a modal loop the
    # events seen per frame are the same.
    front_385b = os.path.join(runner_dir, "src", "frontend.cpp")
    replace(front_385b,
            "#include \"lb_overlay.h\"   /* legobattles-decomp */\n",
            "#include \"lb_overlay.h\"   /* legobattles-decomp */\n"
            "struct SDL_Window;\n"
            "/* legobattles-decomp docs/385: src/lb_winloop.cpp */\n"
            "extern \"C\" void lb_winloop_pump(SDL_Window* a, SDL_Window* b);\n")
    replace(front_385b,
            "        SDL_Event event{};\n"
            "        while (SDL_PollEvent(&event)) {\n",
            "        SDL_Event event{};\n"
            "        /* legobattles-decomp docs/385: pumped where a window's\n"
            "           modal move loop cannot hold the frame loop. */\n"
            "        lb_winloop_pump(presentation.windows[0],\n"
            "                        presentation.windows[1]);\n"
            "        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT,\n"
            "                              SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {\n")
    done.append("frontend.cpp: the event pump survives a window's modal loop "
                "(docs/385)")

    # 0zzzzzzzzzzzzzzz. THE HUD IS SPLIT UNDER EDGE MARKING TOO (docs/388).
    # The HUD elements snapped back to their original-aspect positions and
    # did not return until the game was closed and reopened. Building_Draw3D
    # (0x0203B3AC), which draws Transport and Siege units (category 5 and 6,
    # FUN_02036CD0), does DISP3DCNT &= 0xCFEF and then (x & 0xCFFF) | 0x20:
    # anti-aliasing off, edge marking on. Only G3X_Init (0x0210C184, boot)
    # sets them back; the scene setup FUN_02024758 ORs in 8 and never clears
    # bit 5. docs/375 split no edge-marked frame, so from the first ship,
    # catapult or dragon on screen every row was unsplit, and docs/385 left an
    # unsplit row's HUD where the game drew it.
    #
    # Now such a frame splits as well, and stays the unsplit picture:
    #  * edge marking reads the rows above and below, so the HUD set is also
    #    cleared on the rows next to the HUD's (for the HUD's own pass, whose
    #    pixels lb_hud moves);
    #  * lb_EdgeMerge computes a HUD row, and each row next to one, in the
    #    HUD's columns, as the unsplit raster would: the HUD blended over the
    #    world as the split merges it, then edge marking against the depth and
    #    polygon ID the unsplit buffers would hold (PlotTranslucentPixel keeps
    #    the opaque ID and edge flags of what a translucent pixel covers). The
    #    world-only row stays the world's own, for the spots lb_hud vacates.
    #    With fog or anti-aliasing on as well (never seen: Building_Draw3D
    #    clears AA) the merge is not exact, and the split falls back to the
    #    two passes as they are, logged once.
    #  * lb_SplitRows returns 3 for a frame the renderer looked at and found
    #    no HUD polygon in, so lb_hud carries its capture only there; any other
    #    unsplit row takes the path from before docs/385.
    # LB_EDGECHECK=1 renders each split frame unsplit first, on the render
    # thread, and counts the pixels the split picture differs by.
    soft_h_388 = os.path.join(runner_dir, "vendor", "melonds", "GPU3D_Soft.h")
    soft_cpp_388 = os.path.join(runner_dir, "vendor", "melonds",
                                "GPU3D_Soft.cpp")
    replace(soft_h_388,
            "    u32 lb_HudZ = 0;   /* docs/380: the HUD's own depth threshold */\n",
            "    u32 lb_HudZ = 0;   /* docs/380: the HUD's own depth threshold */\n"
            "    /* docs/388: edge-marked split frames; see apply_hooks.py. */\n"
            "    bool lb_EdgeSplit = false;   /* split and edge-marked */\n"
            "    bool lb_EdgeExact = false;   /* ... with fog and AA off */\n"
            "    bool lb_ForceNoSplit = false;   /* LB_EDGECHECK's first pass */\n"
            "    bool lb_Checking = false;\n"
            "    u8 lb_FrameNoHud[2] = {0, 0};   /* looked, found no HUD */\n"
            "    u32 lb_EdgeLine[ScanlineWidth];\n"
            "    void lb_ClearHudRow(s32 y) noexcept;\n"
            "    void lb_EdgeMerge(const GPU3D& gpu3d, s32 y);\n"
            "    void lb_CheckBefore(GPU& gpu);\n"
            "    void lb_CheckAfter();\n")
    replace(soft_cpp_388,
            "        if (row >= NumScanlines || !lb_FrameSplit[s]) return 0;\n",
            "        if (row >= NumScanlines) return 0;\n"
            "        /* docs/388: 3, the renderer looked and the frame had no\n"
            "           HUD polygon; 0, it did not look. */\n"
            "        if (!lb_FrameSplit[s]) return lb_FrameNoHud[s] ? 3 : 0;\n")
    replace(soft_cpp_388,
            "    if (RenderWidth > 256 && !(gpu.GPU3D.RenderDispCnt & (1<<5)) &&\n"
            "        lb_hud_split_wanted())\n"
            "    {\n",
            "    /* docs/388: edge-marked frames split too (lb_EdgeMerge). */\n"
            "    bool lb_looked = false;\n"
            "    if (RenderWidth > 256 && !lb_ForceNoSplit && lb_hud_split_wanted())\n"
            "    {\n"
            "        lb_looked = true;\n")
    replace(soft_cpp_388,
            "    lb_FrameSplit[lb_Slot] = lb_SplitFrame ? 1 : 0;\n",
            "    lb_FrameSplit[lb_Slot] = lb_SplitFrame ? 1 : 0;\n"
            "    lb_FrameNoHud[lb_Slot] = (lb_looked && lb_HudCount == 0) ? 1 : 0;\n"
            "    lb_EdgeSplit = lb_SplitFrame && (gpu.GPU3D.RenderDispCnt & (1<<5));\n"
            "    /* LB_EDGEMERGE=0 keeps the two passes as they are: the\n"
            "       control that shows LB_EDGECHECK can see a difference. */\n"
            "    static const bool lb_merge_on = [] {\n"
            "        const char* v = std::getenv(\"LB_EDGEMERGE\");\n"
            "        return !(v && v[0] == '0');\n"
            "    }();\n"
            "    lb_EdgeExact = lb_EdgeSplit && lb_merge_on &&\n"
            "                   !(gpu.GPU3D.RenderDispCnt & ((1<<4) | (1<<7)));\n"
            "    if (lb_EdgeSplit)\n"
            "    {\n"
            "        static bool lb_said = false, lb_said_inexact = false;\n"
            "        if (!lb_said || (!lb_EdgeExact && !lb_said_inexact))\n"
            "        {\n"
            "            std::fprintf(stderr, \"[lb_hudsplit] edge-marked frame \"\n"
            "                         \"(DISP3DCNT %04X) split: %s\\n\",\n"
            "                         (unsigned)gpu.GPU3D.RenderDispCnt,\n"
            "                         lb_EdgeExact ? \"edges marked on the merged \"\n"
            "                                        \"picture\"\n"
            "                                      : \"not merged (fog or anti-\"\n"
            "                                        \"aliasing on, or LB_EDGEMERGE=0),\"\n"
            "                                        \" HUD borders approximate\");\n"
            "            lb_said = true;\n"
            "            if (!lb_EdgeExact) lb_said_inexact = true;\n"
            "        }\n"
            "    }\n")
    replace(soft_cpp_388,
            "    if (lb_any) lb_UseHudBuffers(false);\n"
            "    lb_RowSplit[lb_Slot][y] = lb_any ? 1 : 0;\n",
            "    if (lb_any) lb_UseHudBuffers(false);\n"
            "    /* docs/388: edge marking reads the rows above and below, so\n"
            "       under it the HUD set is cleared on the rows next to the\n"
            "       HUD's as well. */\n"
            "    if (lb_EdgeSplit)\n"
            "    {\n"
            "        if (lb_any && (y == 0 || !lb_RowSplit[lb_Slot][y - 1]))\n"
            "            lb_ClearHudRow(y - 1);\n"
            "        if (!lb_any && y > 0 && lb_RowSplit[lb_Slot][y - 1])\n"
            "            lb_ClearHudRow(y);\n"
            "    }\n"
            "    lb_RowSplit[lb_Slot][y] = lb_any ? 1 : 0;\n")
    replace(soft_cpp_388,
            "    lb_FinishRow(gpu.GPU3D, RenderHeight - 1);   /* docs/375 */\n",
            "    if (lb_EdgeSplit && lb_RowSplit[lb_Slot][RenderHeight - 1])\n"
            "        lb_ClearHudRow(RenderHeight);   /* docs/388: the border */\n"
            "    lb_FinishRow(gpu.GPU3D, RenderHeight - 1);   /* docs/375 */\n")
    replace(soft_cpp_388,
            "void SoftRenderer::lb_FinishRow(const GPU3D& gpu3d, s32 y)\n"
            "{\n"
            "    ScanlineFinalPass(gpu3d, y);\n"
            "    if (!lb_SplitFrame || y < 0 || y >= NumScanlines ||\n"
            "        !lb_RowSplit[lb_Slot][y])\n"
            "        return;\n",
            "void SoftRenderer::lb_FinishRow(const GPU3D& gpu3d, s32 y)\n"
            "{\n"
            "    /* docs/388: on an edge-marked split frame a HUD row, and a row\n"
            "       next to one, takes its HUD columns from lb_EdgeMerge. */\n"
            "    const bool lb_in = lb_SplitFrame && y >= 0 && y < NumScanlines;\n"
            "    const bool lb_split = lb_in && lb_RowSplit[lb_Slot][y];\n"
            "    const bool lb_merge = lb_in && lb_EdgeExact &&\n"
            "        (lb_split || (y > 0 && lb_RowSplit[lb_Slot][y - 1]) ||\n"
            "         (y + 1 < NumScanlines && lb_RowSplit[lb_Slot][y + 1]));\n"
            "    if (lb_merge) lb_EdgeMerge(gpu3d, y);\n"
            "    ScanlineFinalPass(gpu3d, y);\n"
            "    if (!lb_split)\n"
            "    {\n"
            "        if (lb_merge)\n"
            "        {\n"
            "            const int lb_base = FirstPixelOffset + y * ScanlineWidth;\n"
            "            for (int x = lb_HudX0[lb_Slot]; x < lb_HudX1[lb_Slot]; x++)\n"
            "                ColorBuffer[lb_base + x] = lb_EdgeLine[x - lb_HudX0[lb_Slot]];\n"
            "        }\n"
            "        return;\n"
            "    }\n")
    replace(soft_cpp_388,
            "        if (ha)\n"
            "        {\n"
            "            ColorBuffer[a] = AlphaBlend(gpu3d, h, w, ha);\n",
            "        if (lb_merge) ColorBuffer[a] = lb_EdgeLine[x - lb_HudX0[lb_Slot]];\n"
            "        if (ha)\n"
            "        {\n"
            "            if (!lb_merge) ColorBuffer[a] = AlphaBlend(gpu3d, h, w, ha);\n")
    replace(soft_cpp_388,
            "int SoftRenderer::lb_SplitRows(const u32* line, const u32** world,\n",
            "/* docs/388: the HUD set's row y, in the frame's HUD columns, as\n"
            "   ClearBuffers leaves the main set. */\n"
            "void SoftRenderer::lb_ClearHudRow(s32 y) noexcept\n"
            "{\n"
            "    u32* c = lb_HudColor[lb_Slot];\n"
            "    const int base = FirstPixelOffset + y * ScanlineWidth;\n"
            "    for (int x = lb_HudX0[lb_Slot]; x < lb_HudX1[lb_Slot]; x++)\n"
            "    {\n"
            "        const int a = base + x;\n"
            "        c[a] = 0;\n"
            "        c[a + BufferSize] = 0;\n"
            "        lb_HudDepth[a] = lb_ClearZ;\n"
            "        lb_HudDepth[a + BufferSize] = lb_ClearZ;\n"
            "        lb_HudAttr[a] = lb_ClearAttr;\n"
            "        lb_HudAttr[a + BufferSize] = lb_ClearAttr;\n"
            "    }\n"
            "}\n"
            "\n"
            "/* docs/388: row y of the UNSPLIT picture in the HUD's columns,\n"
            "   before the world's final pass: the HUD blended over the world\n"
            "   as lb_FinishRow merges them, then edge marking exactly as\n"
            "   ScanlineFinalPass does it, against the depth and polygon ID the\n"
            "   unsplit buffers would hold. That is the HUD's where a HUD pixel\n"
            "   wrote depth in front of the world, else the world's; a\n"
            "   translucent pixel keeps the opaque ID and edge flags of what it\n"
            "   covers (PlotTranslucentPixel), which is the world's when no\n"
            "   opaque HUD pixel is under it. Only HUD rows are read from the\n"
            "   HUD set; no other row has a HUD pixel. Fog and AA off. */\n"
            "void SoftRenderer::lb_EdgeMerge(const GPU3D& gpu3d, s32 y)\n"
            "{\n"
            "    const int x0 = lb_HudX0[lb_Slot], x1 = lb_HudX1[lb_Slot];\n"
            "    const u32* hc = lb_HudColor[lb_Slot];\n"
            "    const u8* rows = lb_RowSplit[lb_Slot];\n"
            "    const auto merged = [&](int x, int yy, u32& z, u32& at) {\n"
            "        const int a = FirstPixelOffset + yy * ScanlineWidth + x;\n"
            "        z = lb_DepthStore[a];\n"
            "        at = lb_AttrStore[a];\n"
            "        if (x < x0 || x >= x1 || yy < 0 || yy >= NumScanlines ||\n"
            "            !rows[yy])\n"
            "            return;\n"
            "        const u32 hz = lb_HudDepth[a], ha = lb_HudAttr[a];\n"
            "        if (hz == lb_ClearZ || hz >= z) return;\n"
            "        z = hz;\n"
            "        if (!(ha & (1u << 22)) || ((ha ^ lb_ClearAttr) & 0xFF00000Fu))\n"
            "            at = ha;\n"
            "    };\n"
            "    const int base = FirstPixelOffset + y * ScanlineWidth;\n"
            "    for (int x = x0; x < x1; x++)\n"
            "    {\n"
            "        const int a = base + x;\n"
            "        const u32 w = ColorBuffer[a];\n"
            "        const u32 h = rows[y] ? hc[a] : 0u;\n"
            "        const u32 hal = (h >> 24) & 0x1F;\n"
            "        u32 c = hal ? AlphaBlend(gpu3d, h, w, hal) : w;\n"
            "        u32 z, at;\n"
            "        merged(x, y, z, at);\n"
            "        if (at & 0xF)\n"
            "        {\n"
            "            const u32 polyid = at >> 24;\n"
            "            u32 nz, na;\n"
            "            bool edge = false;\n"
            "            merged(x - 1, y, nz, na);\n"
            "            edge = edge || (polyid != (na >> 24) && z < nz);\n"
            "            merged(x + 1, y, nz, na);\n"
            "            edge = edge || (polyid != (na >> 24) && z < nz);\n"
            "            merged(x, y - 1, nz, na);\n"
            "            edge = edge || (polyid != (na >> 24) && z < nz);\n"
            "            merged(x, y + 1, nz, na);\n"
            "            edge = edge || (polyid != (na >> 24) && z < nz);\n"
            "            if (edge)\n"
            "            {\n"
            "                u16 edgecolor = gpu3d.RenderEdgeTable[polyid >> 3];\n"
            "                u32 edgeR = (edgecolor << 1) & 0x3E; if (edgeR) edgeR++;\n"
            "                u32 edgeG = (edgecolor >> 4) & 0x3E; if (edgeG) edgeG++;\n"
            "                u32 edgeB = (edgecolor >> 9) & 0x3E; if (edgeB) edgeB++;\n"
            "                c = edgeR | (edgeG << 8) | (edgeB << 16) | (c & 0xFF000000);\n"
            "            }\n"
            "        }\n"
            "        lb_EdgeLine[x - x0] = c;\n"
            "    }\n"
            "}\n"
            "\n"
            "/* docs/388: LB_EDGECHECK=1. The frame rendered unsplit first,\n"
            "   into the same buffers, and kept; after the real render, the\n"
            "   pixels that differ are counted. Diagnostics only: it renders\n"
            "   every widened frame twice. */\n"
            "namespace { u32* lb_check_store = nullptr; }\n"
            "void SoftRenderer::lb_CheckBefore(GPU& gpu)\n"
            "{\n"
            "    static const bool on = std::getenv(\"LB_EDGECHECK\") != nullptr;\n"
            "    lb_Checking = false;\n"
            "    if (!on || RenderWidth <= 256) return;\n"
            "    lb_ForceNoSplit = true;\n"
            "    ClearBuffers(gpu);\n"
            "    RenderPolygons(gpu, false, &gpu.GPU3D.RenderPolygonRAM[0],\n"
            "                   gpu.GPU3D.RenderNumPolygons);\n"
            "    lb_ForceNoSplit = false;\n"
            "    if (!lb_check_store) lb_check_store = new u32[BufferSize];\n"
            "    memcpy(lb_check_store, ColorBuffer, BufferSize * sizeof(u32));\n"
            "    lb_Checking = true;\n"
            "}\n"
            "\n"
            "void SoftRenderer::lb_CheckAfter()\n"
            "{\n"
            "    if (!lb_Checking) return;\n"
            "    lb_Checking = false;\n"
            "    if (!lb_SplitFrame) return;   /* nothing split: the same passes */\n"
            "    static unsigned frames[2], rows[2], worst[2];\n"
            "    static unsigned long long px[2], other[2];\n"
            "    const int e = lb_EdgeSplit ? 1 : 0;\n"
            "    unsigned n = 0, r = 0, o = 0;\n"
            "    for (int y = 0; y < RenderHeight; y++)\n"
            "    {\n"
            "        const int base = FirstPixelOffset + y * ScanlineWidth;\n"
            "        unsigned d = 0;\n"
            "        for (int x = 0; x < RenderWidth; x++)\n"
            "            if (ColorBuffer[base + x] != lb_check_store[base + x]) d++;\n"
            "        if (!d) continue;\n"
            "        if (lb_RowSplit[lb_Slot][y]) { n += d; r++; }\n"
            "        else o += d;\n"
            "    }\n"
            "    frames[e]++;\n"
            "    rows[e] += r;\n"
            "    px[e] += n;\n"
            "    other[e] += o;\n"
            "    if (n + o > worst[e]) worst[e] = n + o;\n"
            "    if (frames[e] % 60u == 1u || (n + o && worst[e] == n + o))\n"
            "        std::fprintf(stderr, \"[lb_edgecheck] %s split frames %u: \"\n"
            "                     \"%llu px differ on HUD rows (%u rows), %llu on \"\n"
            "                     \"other rows; worst frame %u px\\n\",\n"
            "                     e ? \"edge-marked\" : \"unmarked\", frames[e],\n"
            "                     px[e], rows[e], other[e], worst[e]);\n"
            "}\n"
            "\n"
            "int SoftRenderer::lb_SplitRows(const u32* line, const u32** world,\n")
    #     LB_TEST_POLYID=1, a test aid for LB_EDGECHECK: every polygon gets
    #     an opaque ID from its index, so edge marking marks every boundary
    #     between two polygons. The game's own frames give nearly every
    #     polygon ID 0, and edge marking then draws nothing the check could
    #     disagree about. Both passes of a checked frame see the same IDs.
    replace(soft_cpp_388,
            "void SoftRenderer::RenderPolygons(const GPU& gpu, bool threaded, Polygon** polygons, int npolys)\n"
            "{\n"
            "    int j = 0;\n",
            "void SoftRenderer::RenderPolygons(const GPU& gpu, bool threaded, Polygon** polygons, int npolys)\n"
            "{\n"
            "    {   /* docs/388: LB_TEST_POLYID=1 */\n"
            "        static const bool lb_ids = std::getenv(\"LB_TEST_POLYID\") != nullptr;\n"
            "        if (lb_ids)\n"
            "            for (int i = 0; i < npolys; i++)\n"
            "                polygons[i]->Attr = (polygons[i]->Attr & ~0x3F000000u) |\n"
            "                                    ((u32)((i * 7 + 3) & 0x3F) << 24);\n"
            "    }\n"
            "    int j = 0;\n")
    replace(soft_cpp_388,
            "            lb_NextColorBuffer();   /* docs/359 */\n"
            "            ClearBuffers(gpu);\n"
            "            RenderPolygons(gpu, true, &gpu.GPU3D.RenderPolygonRAM[0], gpu.GPU3D.RenderNumPolygons);\n",
            "            lb_NextColorBuffer();   /* docs/359 */\n"
            "            lb_CheckBefore(gpu);   /* docs/388: LB_EDGECHECK=1 */\n"
            "            ClearBuffers(gpu);\n"
            "            RenderPolygons(gpu, true, &gpu.GPU3D.RenderPolygonRAM[0], gpu.GPU3D.RenderNumPolygons);\n"
            "            lb_CheckAfter();\n")
    done.append("melonds: edge-marked frames split, edges marked on the merged "
                "picture; split code 3 (docs/388)")

    # 0zzzzzzzzzzzzzzzz. A 2D-ONLY SCENE IS CENTRED (docs/389). The menus,
    # including the boot startup logos and the save select, must look
    # consistent across window scales and stretches and be properly
    # centred in all layers. On engine A this branch drew
    # the native picture at the BAND -- where the guest's 256 columns sit in the
    # widened 3D raster -- and the band is set only when the game submits 3D
    # geometry (lb_band_for_geometry). The boot logos, the legal screen and
    # the title are drawn before any geometry is latched, so they were drawn at
    # 0,0: the top-left corner of the window (LB_FILLTRACE=1: engine 0 at 0,0,
    # dispcnt 00010D0B), and every resize put the picture at the previous
    # size's band for a frame. This branch shows the NATIVE picture, so the
    # band only matters where a battle's camera places it and the frames
    # either side are widened with it (lb_widemap_band_live). Anywhere else --
    # a 2D-only scene, the logos, the title, a menu -- the picture is centred,
    # and the offsets the frontend maps touches with (lb_shown_band_x/y) are
    # the ones it was drawn at.
    gpu_390 = os.path.join(runner_dir, "src", "gpu2d.cpp")
    replace(gpu_390,
            "static uint8_t g_lb_adaptive_screens = 0xFFu;   /* legobattles-decomp */\n",
            "static uint8_t g_lb_adaptive_screens = 0xFFu;   /* legobattles-decomp */\n"
            "extern \"C\" int lb_widemap_band_live(void);   /* docs/389 */\n")
    replace(gpu_390,
            "        if (lb_wide2d_supported(0, u, palette, oam, vram)) {\n"
            "            for (int y = 0; y < 192; ++y)\n"
            "                lb_wide2d_line(0, y, u, palette, oam, *vram,\n"
            "                               adaptive.data() +\n"
            "                                   (y + extra_y) * output_width,\n"
            "                               output_width, extra);\n"
            "            lb_fill_rows_outward(adaptive.data(), output_width,\n"
            "                                 output_height, extra_y);\n"
            "            return adaptive.data();\n"
            "        }\n"
            "        // legobattles-decomp: menus cannot be widened truthfully;\n"
            "        // fill rather than letterbox. See lb_menu_fill.\n"
            "        lb_menu_fill(native, adaptive.data(), output_width,\n"
            "                     output_height, extra, extra_y);\n"
            "        return adaptive.data();\n",
            "        /* legobattles-decomp docs/389: centred unless a battle's\n"
            "           camera places the band; see apply_hooks.py. */\n"
            "        const bool lb_banded = bg0_3d && lb_widemap_band_live();\n"
            "        const int lb_cx = lb_banded ? extra : (output_width - 256) / 2;\n"
            "        const int lb_cy = lb_banded ? extra_y : (output_height - 192) / 2;\n"
            "        g_lb_used_extra = lb_cx;\n"
            "        g_lb_used_extra_y = lb_cy;\n"
            "        /* docs/392: and never decoded past the hardware window.\n"
            "           Engine A's tilemaps hold only what the window shows\n"
            "           (lb_engineA_widening_allowed); the loading screen's\n"
            "           512-wide BG2 showed the last battle's tiles out there. */\n"
            "        if (lb_wide2d_supported(0, u, palette, oam, vram) &&\n"
            "            lb_engineA_widening_allowed) {\n"
            "            for (int y = 0; y < 192; ++y)\n"
            "                lb_wide2d_line(0, y, u, palette, oam, *vram,\n"
            "                               adaptive.data() +\n"
            "                                   (y + lb_cy) * output_width,\n"
            "                               output_width, lb_cx);\n"
            "            lb_fill_rows_outward(adaptive.data(), output_width,\n"
            "                                 output_height, lb_cy);\n"
            "            return adaptive.data();\n"
            "        }\n"
            "        // legobattles-decomp: menus cannot be widened truthfully;\n"
            "        // fill rather than letterbox. See lb_menu_fill.\n"
            "        lb_menu_fill(native, adaptive.data(), output_width,\n"
            "                     output_height, lb_cx, lb_cy);\n"
            "        return adaptive.data();\n")
    done.append("gpu2d: a 2D-only engine-A scene is centred (docs/389)")

    # 0zzzzzzzzzzzzzzzzz. THE MODS FOLDER'S MAPS JOIN THE CARTRIDGE (docs/391).
    # lb_mods.cpp adds each mod map to the image's file system (three new
    # directories, files appended; no stock file id moves) and its name to
    # every language file, before the image reaches the emulator. After the
    # SHA-1 check, which is the identity of the player's own ROM, and
    # lb_teams is re-attached in case the image grew. Without a mods folder
    # the image is untouched.
    main_391 = os.path.join(runner_dir, "src", "main.cpp")
    replace(main_391,
            'extern "C" void lb_rom_attach(const unsigned char*, size_t);'
            '  /* docs/384 */\n',
            'extern "C" void lb_rom_attach(const unsigned char*, size_t);'
            '  /* docs/384 */\n'
            'extern "C" int lb_mods_build(std::vector<uint8_t>* rom);'
            '  /* docs/391 */\n'
            'void lb_mods_keep_rom(std::vector<uint8_t>* rom);'
            '  /* docs/432 */\n')
    replace(main_391,
            "                         rom_sha1.c_str());\n"
            "            return 1;\n"
            "        }\n"
            "    }\n"
            "    std::string save_path;\n",
            "                         rom_sha1.c_str());\n"
            "            return 1;\n"
            "        }\n"
            "    }\n"
            "    /* legobattles-decomp docs/391: the mods folder's maps. docs/432:\n"
            "       the image as the ROM gave it is kept, so a Reset can rebuild\n"
            "       it with a map that arrived since (boot() loads `rom`). */\n"
            "    if (!rom.empty()) lb_mods_keep_rom(&rom);\n"
            "    if (!rom.empty() && lb_mods_build(&rom) > 0)\n"
            "        lb_rom_attach(rom.data(), rom.size());\n"
            "    std::string save_path;\n")
    done.append("main.cpp: the mods folder's maps join the cartridge (docs/391)")

    # 0zzzzzzzzzzzzzzzzzz. THE FIRST-RUN SETUP (docs/394). A double-clicked
    # LegoBattles.exe with no ROM in rom\ printed an explanation to stderr and
    # exited, and a double-clicked program has no console: a new player saw
    # nothing. lb_setup.cpp asks for the ROM in a window of its own (Europe,
    # or USA converted to the European image) and hands the image back. A ROM
    # already in rom\ whose header is not the European one (C5SP at 0x0C) is
    # handed to it too. And the no-argument start gets the local-wireless
    # bridge play.bat always passed, which multiplayer needs.
    main_394 = os.path.join(runner_dir, "src", "main.cpp")
    replace(main_394,
            'extern "C" void lb_rom_attach(const unsigned char*, size_t);'
            '  /* docs/384 */\n',
            'extern "C" void lb_rom_attach(const unsigned char*, size_t);'
            '  /* docs/384 */\n'
            'extern "C" int lb_first_run_setup(const char* root, const char* found,'
            ' char* out, int cap);  /* docs/394 */\n')
    replace(main_394,
            "        if (rom.empty()) {\n"
            "            std::fprintf(stderr,\n"
            "                \"No ROM found in %s\\n\\n\"\n"
            "                \"Put your own LEGO Battles cartridge dump there and run this\\n\"\n"
            "                \"again. It is the only thing this port needs. mods/ is for\\n\"\n"
            "                \"custom map data, not for ROMs to run.\\n\",\n"
            "                (root / \"rom\").string().c_str());\n"
            "            return 1;\n"
            "        }\n",
            "        /* legobattles-decomp docs/394: the first-run setup. */\n"
            "        bool lb_rom_ok = false;\n"
            "        if (!rom.empty()) {\n"
            "            char code[4] = {0, 0, 0, 0};\n"
            "            std::ifstream lb_head(rom, std::ios::binary);\n"
            "            lb_head.seekg(0x0C);\n"
            "            lb_head.read(code, 4);\n"
            "            lb_rom_ok = (bool)lb_head && std::memcmp(code, \"C5SP\", 4) == 0;\n"
            "        }\n"
            "        if (!lb_rom_ok) {\n"
            "            static char lb_picked[2048];\n"
            "            const std::string lb_found = rom.empty() ? std::string() : rom.string();\n"
            "            if (!lb_first_run_setup(root.string().c_str(), lb_found.c_str(),\n"
            "                                    lb_picked, (int)sizeof lb_picked)) {\n"
            "                std::fprintf(stderr,\n"
            "                    \"No ROM set up in %s; closed.\\n\",\n"
            "                    (root / \"rom\").string().c_str());\n"
            "                return 1;\n"
            "            }\n"
            "            rom = fs::path(lb_picked);\n"
            "        }\n")
    replace(main_394,
            "        synthesized.push_back(\"--interactive\");\n",
            "        synthesized.push_back(\"--interactive\");\n"
            "        /* docs/394: what play.bat passes, so multiplayer works from a\n"
            "           double-click: no slirp, and the DS-wireless bridge. */\n"
            "        synthesized.push_back(\"--network\");\n"
            "        synthesized.push_back(\"off\");\n"
            "        synthesized.push_back(\"--local-wireless\");\n"
            "        synthesized.push_back(\"on\");\n")
    done.append("main.cpp: the first-run setup and local wireless (docs/394)")

    # docs/396: THE PC'S DATE. The cartridge reads the DS clock in exactly one
    # place, User::MainMenu (0x020BF0A0): RTC_GetDate (FUN_02115F90), and on
    # 12/25 it unlocks record 0x1B8, Santa. The runner's clock starts at the
    # fixed 2024-01-01 unless --rtc-host, so a player could never get it. A
    # player's start now passes --rtc-host; tests keep the fixed clock (they
    # pass their own arguments), and a recording carries the clock it ran on
    # so a replay sets the same one (lb_demo.cpp, rtc=).
    io_396 = os.path.join(runner_dir, "src", "io.cpp")
    edit(io_396, "bool g_nds_rtc_host = false;",
         ["/* legobattles-decomp docs/396: the guest clock's seven BCD registers",
          "   (year, month, day, weekday, hour, minute, second) for lb_demo.cpp,",
          "   which records them and restores them on a replay. */",
          "extern \"C\" void lb_rtc_get(unsigned char out[7]) {",
          "    std::memcpy(out, g_rtc_datetime, 7);",
          "}",
          "extern \"C\" void lb_rtc_set(const unsigned char in[7]) {",
          "    std::memcpy(g_rtc_datetime, in, 7);",
          "}",
          "extern \"C\" int lb_rtc_is_host(void) { return g_nds_rtc_host ? 1 : 0; }"])
    replace(main_394,
            "        synthesized.push_back(\"--local-wireless\");\n"
            "        synthesized.push_back(\"on\");\n",
            "        synthesized.push_back(\"--local-wireless\");\n"
            "        synthesized.push_back(\"on\");\n"
            "        /* docs/396: the PC's date and time, for the 12/25 unlock. */\n"
            "        synthesized.push_back(\"--rtc-host\");\n")
    done.append("io.cpp/main.cpp: a player's start runs on the PC's clock (docs/396)")

    # docs/396: THE MENU'S RESET. The interactive start already hands its
    # power-on boot() to the debug server (debug_set_reset_fn), which runs it
    # for a `reset` command between frames. The menu's Reset (and Ctrl+R) uses
    # the same boot at the same safe point: just after debug_pump(), on the
    # frontend thread, between frames. lb_port_on_guest_reset then re-arms the
    # port's once-only state (lb_patches.cpp).
    dbg_396 = os.path.join(runner_dir, "src", "debug_server.cpp")
    edit(dbg_396, "void debug_set_reset_fn(std::function<void()> fn) { g_reset_fn = std::move(fn); }",
         ["/* legobattles-decomp docs/396: the same boot, for the menu's Reset. */",
          "extern \"C\" int lb_debug_reset(void) {",
          "    if (!g_reset_fn) return 0;",
          "    g_reset_fn();",
          "    return 1;",
          "}"])
    front_396 = os.path.join(runner_dir, "src", "frontend.cpp")
    edit(front_396, "#include \"debug_server.h\"",
         ["extern \"C\" int lb_take_game_reset(void);      /* legobattles-decomp docs/396 */",
          "extern \"C\" int lb_debug_reset(void);",
          "extern \"C\" void lb_port_on_guest_reset(void);",
          "extern \"C\" void lb_mods_rebuild_if_asked(void);   /* docs/432 */"])
    edit(front_396, "        debug_pump();",
         ["/* legobattles-decomp docs/396: the menu's Reset, at the same point. */",
          "if (lb_take_game_reset()) {",
          "    lb_mods_rebuild_if_asked();   /* docs/432: a map that arrived */",
          "    if (lb_debug_reset()) lb_port_on_guest_reset();",
          "}"])
    done.append("debug_server.cpp/frontend.cpp: the menu's Reset reboots the game (docs/396)")

    # docs/396: THE MULTIPLAYER STALL. Two instances in a match ran at 31 fps,
    # each spending 29.3 ms a frame in emulation against ~10 ms alone (run
    # t396mp, frontend_stats over 40 s). The difference is the wireless
    # bridge's two blocking receives -- the host waiting for its children's
    # replies, a child waiting for the host's packet -- which poll their queue
    # and SLEEP 1 ms between polls. A sleep is at least one timer period
    # whether the datagram arrived after 30 us or not, and the game exchanges
    # several per frame, so every exchange cost a millisecond or more. The wait
    # now blocks on the socket itself (select, 1 ms ceiling, so the deadline
    # is still checked), which returns the moment a datagram lands.
    wifi_396 = os.path.join(runner_dir, "src", "wifi_net.cpp")
    edit(wifi_396, "    static constexpr uint32_t kMagic = 0x4946494Eu;",
         ["// legobattles-decomp docs/396: wait for the socket, not the clock.",
          "void WaitReadable() {",
          "#ifdef _WIN32",
          "    if (socket_ != INVALID_SOCKET) {",
          "        fd_set lb_rd;",
          "        FD_ZERO(&lb_rd);",
          "        FD_SET(socket_, &lb_rd);",
          "        timeval lb_tv{0, 1000};",
          "        ::select(0, &lb_rd, nullptr, nullptr, &lb_tv);",
          "        return;",
          "    }",
          "#endif",
          "    std::this_thread::sleep_for(std::chrono::milliseconds(1));",
          "}",
          ""], after=False)
    replace(wifi_396,
            "                if (std::chrono::steady_clock::now() >= deadline) break;\n"
            "                std::this_thread::sleep_for(std::chrono::milliseconds(1));\n",
            "                if (std::chrono::steady_clock::now() >= deadline) break;\n"
            "                WaitReadable();   /* legobattles-decomp docs/396 */\n")
    replace(wifi_396,
            "                    return 0;\n"
            "                std::this_thread::sleep_for(std::chrono::milliseconds(1));\n",
            "                    return 0;\n"
            "                WaitReadable();   /* legobattles-decomp docs/396 */\n")
    done.append("wifi_net.cpp: the wireless bridge waits on its socket, not 1 ms sleeps (docs/396)")

    # 0zzzzzzzzzzzzz-409. A REPLAY SIZES THE FIELD AS THE RECORDING DID. The
    # field (the guest surface) follows the window and the zoom, and the
    # simulation sees it: camera bounds, where a moved HUD button is touched.
    # A session that zoomed could not be replayed (docs/408). lb_demo_field
    # records each size the frontend picks and, replaying a recording that
    # has them, puts the recorded size back before the surface is sized.
    front_409 = os.path.join(runner_dir, "src", "frontend.cpp")
    replace(front_409,
            "                if (std::getenv(\"LB_NOVFOV\")) lb_wanth = 192;\n",
            "                if (std::getenv(\"LB_NOVFOV\")) lb_wanth = 192;\n"
            "                lb_demo_field(&lb_want, &lb_wanth);   /* docs/409 */\n")
    done.append("frontend.cpp: the field's size is recorded and replayed (docs/409)")

    # 0zzzzzzzzzzzzz-411. SMOOTH 3D MODELS, AT THE RENDERER'S OWN COPY.
    # Building_Draw3D (0x0203B3AC) turns anti-aliasing off and edge marking on
    # for every 3D-model unit it draws (DISP3DCNT &= 0xCFEF, |= 0x20), so a
    # session's first ship or siege leaves the models outlined and jagged
    # (docs/388). "Smooth 3D Models" renders with anti-aliasing on and edge
    # marking off instead. It is applied where melonDS latches the register
    # for the frame (RenderDispCnt), not in the guest: patching the guest
    # would run that per-model function interpreted, and the register is
    # never read by the simulation. The identical-frame test compares the
    # same masked value, so an unchanged frame is still skipped.
    gpu3d_411 = os.path.join(runner_dir, "vendor", "melonds", "GPU3D.cpp")
    edit(gpu3d_411, "extern \"C\" int lb_band_for_geometry(int render_width);",
         ["extern \"C\" int lb_setting_smooth_models(void);   /* docs/411 */",
          "static inline unsigned lb_render_dispcnt(unsigned d) {",
          "    return lb_setting_smooth_models() ? ((d & ~0x20u) | 0x10u) : d;",
          "}"], after=True)
    replace(gpu3d_411,
            "                RenderFrameIdentical = RenderDispCnt == DispCnt\n",
            "                RenderFrameIdentical = RenderDispCnt == lb_render_dispcnt(DispCnt)\n")
    replace(gpu3d_411,
            "            RenderDispCnt = DispCnt;\n",
            "            RenderDispCnt = lb_render_dispcnt(DispCnt);   /* docs/411 */\n")
    done.append("GPU3D.cpp: Smooth 3D Models renders without edge marking, with AA (docs/411)")

    # 0zzzzzzzzzzzzz-413. THE HOST'S MOD MAPS RIDE ON ITS RULES. A lobby names
    # its map by a key that, for a mod map, is the map's place in each
    # machine's own mods folder (lb_mods_name_id), so the host lists every
    # mod map it holds -- key, content hash, name -- after its bans and flags,
    # and a joiner translates the key to its own copy of that map, or refuses
    # it (lb_patches.cpp, docs/413). Thirty maps with names do not fit the 64
    # bytes the bans were sized for; the bridge's frames take 0x948.
    wifi_413 = os.path.join(runner_dir, "src", "wifi_net.cpp")
    replace(wifi_413,
            "        uint8_t buf[64];\n"
            "        const int n = lb_rules_outgoing(buf, (int)sizeof buf);\n",
            "        uint8_t buf[2048];   /* docs/413: the host's mod maps */\n"
            "        const int n = lb_rules_outgoing(buf, (int)sizeof buf);\n")
    done.append("wifi_net.cpp: a host's rules carry its mod maps (docs/413)")

    # 0zzzzzzzzzzzzz-415. ONLINE CHAT. A message is a datagram of its own type,
    # like the rules: lb_chat queues what to send and the bridge's drain sends
    # it to the siblings and the remote peer; a received one goes to lb_chat
    # (which a host also passes on), never to the guest.
    wifi_415 = os.path.join(runner_dir, "src", "wifi_net.cpp")
    replace(wifi_415,
            "/* legobattles-decomp (docs/384): the host's rules. */\n",
            "/* legobattles-decomp (docs/415): online chat. */\n"
            "extern \"C\" int lb_chat_outgoing(unsigned char* buf, int cap);\n"
            "extern \"C\" void lb_chat_incoming(const unsigned char* buf, int len,\n"
            "                                 unsigned int sender);\n"
            "/* legobattles-decomp (docs/384): the host's rules. */\n")
    replace(wifi_415,
            "    static constexpr uint32_t kLbRules = 0x4C425255u;  /* LBRU */\n",
            "    static constexpr uint32_t kLbRules = 0x4C425255u;  /* LBRU */\n"
            "    /* docs/415: a chat message, never a frame. */\n"
            "    static constexpr uint32_t kLbChat = 0x4C424348u;   /* LBCH */\n"
            "    void LbSendChat() {\n"
            "        uint8_t buf[512];\n"
            "        for (int k = 0; k < 8; ++k) {\n"
            "            const int n = lb_chat_outgoing(buf, (int)sizeof buf);\n"
            "            if (n <= 0) break;\n"
            "            SendGeneric(kLbChat, buf, n, 0u);\n"
            "        }\n"
            "    }\n")
    replace(wifi_415,
            "        LbMaybeRules();   /* docs/384 */\n",
            "        LbMaybeRules();   /* docs/384 */\n"
            "        LbSendChat();     /* docs/415 */\n")
    replace(wifi_415,
            "            if (ReadLe32(datagram + 8u) == kLbRules) {\n",
            "            /* docs/415: chat goes to lb_chat, never to the guest. */\n"
            "            if (ReadLe32(datagram + 8u) == kLbChat) {\n"
            "                const uint32_t clen = ReadLe32(datagram + 12u);\n"
            "                if (kHeaderBytes + clen == static_cast<uint32_t>(got))\n"
            "                    lb_chat_incoming(datagram + kHeaderBytes,\n"
            "                                     static_cast<int>(clen),\n"
            "                                     ReadLe32(datagram + 4u));\n"
            "                continue;\n"
            "            }\n"
            "            if (ReadLe32(datagram + 8u) == kLbRules) {\n")
    done.append("wifi_net.cpp: online chat travels between players (docs/415)")

    # 0zzzzzzzzzzzzz-423. THE HOST-LEVEL WH. Host code answers the game's
    # wireless helper (lb_vwh.cpp), and its datagrams ride this socket as a
    # type of their own, like chat: lb_vwh queues them and the drain sends
    # them; a received one goes to lb_vwh, never to the guest. The radio is
    # never powered on then, so nothing calls MP_Begin: the pump opens the
    # socket itself, with whatever peer lb_net asked for.
    wifi_423 = os.path.join(runner_dir, "src", "wifi_net.cpp")
    replace(wifi_423,
            "/* legobattles-decomp (docs/415): online chat. */\n",
            "/* legobattles-decomp (docs/423): the host-level WH. */\n"
            "extern \"C\" int lb_vwh_outgoing(unsigned char* buf, int cap);\n"
            "extern \"C\" void lb_vwh_incoming(const unsigned char* buf, int len,\n"
            "                                unsigned int sender);\n"
            "/* legobattles-decomp (docs/415): online chat. */\n")
    replace(wifi_423,
            "    static constexpr uint32_t kLbChat = 0x4C424348u;   /* LBCH */\n",
            "    static constexpr uint32_t kLbChat = 0x4C424348u;   /* LBCH */\n"
            "    /* docs/423: the host-level WH's datagrams. */\n"
            "    static constexpr uint32_t kLbVwh = 0x4C425657u;    /* LBVW */\n"
            "    void LbSendVwh() {\n"
            "        uint8_t buf[1400];\n"
            "        for (int k = 0; k < 16; ++k) {\n"
            "            const int n = lb_vwh_outgoing(buf, (int)sizeof buf);\n"
            "            if (n <= 0) break;\n"
            "            SendGeneric(kLbVwh, buf, n, 0u);\n"
            "        }\n"
            "    }\n")
    replace(wifi_423,
            "        LbSendChat();     /* docs/415 */\n",
            "        LbSendChat();     /* docs/415 */\n"
            "        LbSendVwh();      /* docs/423 */\n")
    replace(wifi_423,
            "            if (ReadLe32(datagram + 8u) == kLbChat) {\n",
            "            /* docs/423: the host-level WH's own, never the guest's. */\n"
            "            if (ReadLe32(datagram + 8u) == kLbVwh) {\n"
            "                const uint32_t vlen = ReadLe32(datagram + 12u);\n"
            "                if (kHeaderBytes + vlen == static_cast<uint32_t>(got))\n"
            "                    lb_vwh_incoming(datagram + kHeaderBytes,\n"
            "                                    static_cast<int>(vlen),\n"
            "                                    ReadLe32(datagram + 4u));\n"
            "                continue;\n"
            "            }\n"
            "            if (ReadLe32(datagram + 8u) == kLbChat) {\n")
    replace(wifi_423,
            "    bool LbEnabled() const { return enabled_; }\n",
            "    bool LbEnabled() const { return enabled_; }\n"
            "    /* docs/423: the host-level WH drives the socket itself. */\n"
            "    bool LbOpen() const {\n"
            "#ifdef _WIN32\n"
            "        return socket_ != INVALID_SOCKET;\n"
            "#else\n"
            "        return false;\n"
            "#endif\n"
            "    }\n"
            "    void LbPump() {\n"
            "        if (!enabled_) return;\n"
            "        if (!LbOpen()) Begin();\n"
            "        DrainSocket();\n"
            "    }\n")
    replace(wifi_423,
            "extern \"C\" int nds_local_wireless_peer(uint32_t* ip_be,\n",
            "/* docs/423: send and receive now, for the host-level WH; opens the\n"
            "   socket (with any peer asked for before the bridge existed) when\n"
            "   the radio, never powered, has not. */\n"
            "extern \"C\" void nds_local_wireless_pump(void) {\n"
            "    if (!g_bridge) return;\n"
            "    if (!g_bridge->local_mp.LbOpen() && g_lb_pending_valid) {\n"
            "        g_bridge->local_mp.LbSetListen(g_lb_pending_listen);\n"
            "        if (g_lb_pending_peer_ip)\n"
            "            g_bridge->local_mp.LbSetPeer(g_lb_pending_peer_ip,\n"
            "                                         g_lb_pending_peer_port);\n"
            "    }\n"
            "    g_bridge->local_mp.LbPump();\n"
            "}\n"
            "\n"
            "extern \"C\" int nds_local_wireless_peer(uint32_t* ip_be,\n")
    done.append("wifi_net.cpp: the host-level WH's datagrams and pump (docs/423)")

    # 0zzzzzzzzzzzzz-424. THE RELAY. For players the direct UDP route cannot
    # connect (a VPN, symmetric or carrier-grade NAT), the master server's
    # relay carries the same datagrams (lb_relay.cpp): every datagram this
    # transport sends also goes to lb_relay_send while the relay is in use, a
    # relayed one is read before the socket and handled as if it had arrived,
    # and a joiner on the relay leaves the direct route alone, so nothing
    # arrives twice. A relayed datagram never teaches the host a peer address
    # and never counts as direct traffic (lb_net watches that count to decide
    # when to try the relay).
    wifi_424 = os.path.join(runner_dir, "src", "wifi_net.cpp")
    replace(wifi_424,
            "/* legobattles-decomp (docs/423): the host-level WH. */\n",
            "/* legobattles-decomp (docs/424): the master server's relay. */\n"
            "extern \"C\" int lb_relay_active(void);\n"
            "extern \"C\" int lb_relay_direct_off(void);\n"
            "extern \"C\" void lb_relay_send(const unsigned char* d, int n);\n"
            "extern \"C\" void lb_relay_flush(void);\n"
            "extern \"C\" int lb_relay_recv(unsigned char* buf, int cap);\n"
            "/* legobattles-decomp (docs/423): the host-level WH. */\n")
    replace(wifi_424,
            "        if (lb_peer_ip_ != 0u && lb_peer_port_ != 0u &&\n"
            "            !(lb_peer_ip_ == htonl(INADDR_LOOPBACK) &&\n",
            "        if (lb_peer_ip_ != 0u && lb_peer_port_ != 0u &&\n"
            "            !lb_relay_direct_off() &&   /* docs/424 */\n"
            "            !(lb_peer_ip_ == htonl(INADDR_LOOPBACK) &&\n")
    replace(wifi_424,
            "        LbCount(sent, 0);   /* legobattles-decomp */\n"
            "        return sent;\n",
            "        /* docs/424: and to the relay, while it carries someone. */\n"
            "        if (lb_relay_active()) {\n"
            "            lb_relay_send(datagram.data(), static_cast<int>(datagram.size()));\n"
            "            sent = len;\n"
            "        }\n"
            "        LbCount(sent, 0);   /* legobattles-decomp */\n"
            "        return sent;\n")
    replace(wifi_424,
            "        LbNatTick();      /* docs/400 */\n",
            "        LbNatTick();      /* docs/400 */\n"
            "        lb_relay_flush(); /* docs/424: what the sends above gathered */\n")
    replace(wifi_424,
            "            const int got = ::recvfrom(\n"
            "                socket_, reinterpret_cast<char*>(datagram), sizeof(datagram),\n"
            "                0, reinterpret_cast<sockaddr*>(&from), &from_len);\n"
            "            if (got == SOCKET_ERROR) {\n"
            "                const int err = WSAGetLastError();\n"
            "                if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {\n"
            "                    melonDS::Platform::Log(melonDS::Platform::Warn,\n"
            "                        \"[local_mp] recvfrom failed: %d\\n\", err);\n"
            "                }\n"
            "                return;\n"
            "            }\n"
            "            if (LbStunAnswer(datagram, got)) continue;   /* docs/400 */\n",
            "            /* docs/424: the relay's datagrams first, as if received. */\n"
            "            int got = lb_relay_recv(datagram, static_cast<int>(sizeof(datagram)));\n"
            "            const bool lb_relayed = got > 0;\n"
            "            if (!lb_relayed) {\n"
            "            got = ::recvfrom(\n"
            "                socket_, reinterpret_cast<char*>(datagram), sizeof(datagram),\n"
            "                0, reinterpret_cast<sockaddr*>(&from), &from_len);\n"
            "            if (got == SOCKET_ERROR) {\n"
            "                const int err = WSAGetLastError();\n"
            "                if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {\n"
            "                    melonDS::Platform::Log(melonDS::Platform::Warn,\n"
            "                        \"[local_mp] recvfrom failed: %d\\n\", err);\n"
            "                }\n"
            "                return;\n"
            "            }\n"
            "            }\n"
            "            if (!lb_relayed && LbStunAnswer(datagram, got)) continue;   /* docs/400 */\n")
    replace(wifi_424,
            "                const bool sibling =\n"
            "                    from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&\n"
            "                    src_port >= base_port_ &&\n"
            "                    src_port < base_port_ + kMaxInstances;\n"
            "            if (!sibling) ++lb_remote_rx_;   /* docs/400 */\n"
            "            if (lb_listen_ && lb_peer_ip_ == 0u && !sibling) {\n",
            "                const bool sibling =\n"
            "                    !lb_relayed &&\n"
            "                    from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&\n"
            "                    src_port >= base_port_ &&\n"
            "                    src_port < base_port_ + kMaxInstances;\n"
            "            /* docs/424: the direct route left alone while on the relay. */\n"
            "            if (!sibling && !lb_relayed && lb_relay_direct_off()) continue;\n"
            "            if (!sibling && !lb_relayed) ++lb_remote_rx_;   /* docs/400 */\n"
            "            if (lb_listen_ && lb_peer_ip_ == 0u && !sibling && !lb_relayed) {\n")
    replace(wifi_424,
            "    void LbHelloTo(uint32_t ip_be, uint16_t port) {\n"
            "    #ifdef _WIN32\n",
            "    void LbHelloTo(uint32_t ip_be, uint16_t port) {\n"
            "        if (lb_relay_direct_off()) return;   /* docs/424 */\n"
            "    #ifdef _WIN32\n")
    replace(wifi_424,
            "    void LbMaybeHello() {\n"
            "#ifdef _WIN32\n"
            "        if (!enabled_ || socket_ == INVALID_SOCKET) return;\n",
            "    void LbMaybeHello() {\n"
            "#ifdef _WIN32\n"
            "        if (!enabled_ || socket_ == INVALID_SOCKET) return;\n"
            "        if (lb_relay_direct_off()) return;   /* docs/424 */\n")
    done.append("wifi_net.cpp: the master server's relay carries the datagrams (docs/424)")

    # 0zzzzzzzzzzzzz-432. A HOST SENDS A JOINER ITS MOD MAP. Its own datagram
    # type, like chat: lb_maptx queues requests (a joiner) and chunks (the
    # host) and the drain sends them; a received one goes to lb_maptx, never
    # to the guest. The relay carries it like every other (docs/424).
    wifi_432 = os.path.join(runner_dir, "src", "wifi_net.cpp")
    replace(wifi_432,
            "/* legobattles-decomp (docs/423): the host-level WH. */\n",
            "/* legobattles-decomp (docs/432): a host's mod map for a joiner. */\n"
            "extern \"C\" int lb_maptx_outgoing(unsigned char* buf, int cap);\n"
            "extern \"C\" void lb_maptx_incoming(const unsigned char* buf, int len,\n"
            "                                  unsigned int sender);\n"
            "/* legobattles-decomp (docs/423): the host-level WH. */\n")
    replace(wifi_432,
            "    static constexpr uint32_t kLbVwh = 0x4C425657u;    /* LBVW */\n",
            "    static constexpr uint32_t kLbVwh = 0x4C425657u;    /* LBVW */\n"
            "    /* docs/432: a mod map's request and chunks. */\n"
            "    static constexpr uint32_t kLbMapx = 0x4C424D58u;   /* LBMX */\n"
            "    void LbSendMapx() {\n"
            "        uint8_t buf[1100];\n"
            "        for (int k = 0; k < 24; ++k) {\n"
            "            const int n = lb_maptx_outgoing(buf, (int)sizeof buf);\n"
            "            if (n <= 0) break;\n"
            "            SendGeneric(kLbMapx, buf, n, 0u);\n"
            "        }\n"
            "    }\n")
    replace(wifi_432,
            "        LbSendVwh();      /* docs/423 */\n",
            "        LbSendVwh();      /* docs/423 */\n"
            "        LbSendMapx();     /* docs/432 */\n")
    replace(wifi_432,
            "            if (ReadLe32(datagram + 8u) == kLbVwh) {\n",
            "            /* docs/432: a mod map in transit, never the guest's. */\n"
            "            if (ReadLe32(datagram + 8u) == kLbMapx) {\n"
            "                const uint32_t mlen = ReadLe32(datagram + 12u);\n"
            "                if (kHeaderBytes + mlen == static_cast<uint32_t>(got))\n"
            "                    lb_maptx_incoming(datagram + kHeaderBytes,\n"
            "                                      static_cast<int>(mlen),\n"
            "                                      ReadLe32(datagram + 4u));\n"
            "                continue;\n"
            "            }\n"
            "            if (ReadLe32(datagram + 8u) == kLbVwh) {\n")
    done.append("wifi_net.cpp: a host sends a joiner its mod map (docs/432)")

    # 0zzzzzzzzzzzzz-433. 3D RESOLUTION: the 3D layer drawn N times as wide and
    # tall (BR-0003). The native renderer is left exactly as it is, so the
    # picture the simulation, the HUD moves and the field locator read does not
    # change. A second SoftRenderer, owned by the native one, draws the same
    # polygon list into buffers N times the size from vertex positions the
    # geometry stage computes at N times the resolution; the compositor then
    # inserts those samples into the 2D stack at their own priority (it already
    # keeps the stack without the 3D layer for the GPU presenter) and the
    # frontend shows that picture instead of the surface. The code is in
    # src/lb_hires_soft.inc, lb_hires_comp.inc and lb_hires_front.inc; here are
    # only the hooks. Default Native: lb_HiN stays 1, nothing below runs.
    soft_h_433 = os.path.join(runner_dir, "vendor", "melonds", "GPU3D_Soft.h")
    soft_c_433 = os.path.join(runner_dir, "vendor", "melonds", "GPU3D_Soft.cpp")
    g3h_433 = os.path.join(runner_dir, "vendor", "melonds", "GPU3D.h")
    g3c_433 = os.path.join(runner_dir, "vendor", "melonds", "GPU3D.cpp")
    # -- the renderer's buffers become per-instance, sized when allocated.
    # Pointers keep every use of them (ColorStore[s][i], lb_HudColor[s], ...)
    # compiling unchanged; the native instance allocates what it always held.
    replace(soft_h_433,
            "#include <thread>\n#include <atomic>\n",
            "#include <thread>\n#include <atomic>\n#include <vector>\n"
            "#include <memory>\n#include <cstdlib>\n")
    replace(soft_h_433,
            "    void SetRenderHeight(u32 height) override;\n",
            "    void SetRenderHeight(u32 height) override;\n"
            "    /* docs/434: the hi-res partner. */\n"
            "    explicit SoftRenderer(bool lb_hi) noexcept;\n"
            "    void lb_HiSetScale(int n, GPU& gpu);\n"
            "    int lb_HiScale() const noexcept { return lb_Hi ? lb_HiN : 1; }\n"
            "    void lb_HiWaitDone(GPU& gpu);\n"
            "    const u32* lb_HiRows(const u32* line, int* stride, int* width,\n"
            "                         int* n, const u8** mask) const noexcept;\n")
    replace(soft_h_433,
            "    static constexpr int MaxRenderWidth = 1728;"
            "   /* legobattles-decomp: was 448, then 512 */\n"
            "    static constexpr int ScanlineWidth = MaxRenderWidth + 2;\n"
            "    static constexpr int MaxRenderHeight = 768;"
            "  /* legobattles-decomp: was 256 */\n"
            "    static constexpr int NumScanlines = MaxRenderHeight + 2;\n"
            "    static constexpr int BufferSize = ScanlineWidth * NumScanlines;\n"
            "    static constexpr int FirstPixelOffset = ScanlineWidth + 1;\n",
            "    /* docs/434: per instance now (lb_Alloc); the native one is\n"
            "       1728 x 768 as it was. */\n"
            "    int MaxRenderWidth = 1728;\n"
            "    int ScanlineWidth = 1730;\n"
            "    int MaxRenderHeight = 768;\n"
            "    int NumScanlines = 770;\n"
            "    int BufferSize = 1730 * 770;\n"
            "    int FirstPixelOffset = 1731;\n")
    replace(soft_h_433,
            "    u32 ColorStore[2][BufferSize * 2];\n"
            "    u32* ColorBuffer = ColorStore[0];\n"
            "    u32* DisplayedBuffer = ColorStore[0];\n"
            "    std::atomic<u32*> ShownColor {ColorStore[0]};\n",
            "    u32* ColorStore[2] = {nullptr, nullptr};   /* 2 x BufferSize each */\n"
            "    u32* ColorBuffer = nullptr;\n"
            "    u32* DisplayedBuffer = nullptr;\n"
            "    std::atomic<u32*> ShownColor {nullptr};\n")
    replace(soft_h_433,
            "    u32 lb_DepthStore[BufferSize * 2];\n"
            "    u32 lb_AttrStore[BufferSize * 2];\n"
            "    u32* DepthBuffer = lb_DepthStore;\n"
            "    u32* AttrBuffer = lb_AttrStore;\n"
            "    u32 lb_HudColor[2][BufferSize * 2];\n"
            "    u32 lb_HudDepth[BufferSize * 2];\n"
            "    u32 lb_HudAttr[BufferSize * 2];\n"
            "    u32 lb_WorldOut[2][BufferSize];\n"
            "    u8 lb_RowSplit[2][NumScanlines];\n",
            "    u32* lb_DepthStore = nullptr;\n"
            "    u32* lb_AttrStore = nullptr;\n"
            "    u32* DepthBuffer = nullptr;\n"
            "    u32* AttrBuffer = nullptr;\n"
            "    u32* lb_HudColor[2] = {nullptr, nullptr};\n"
            "    u32* lb_HudDepth = nullptr;\n"
            "    u32* lb_HudAttr = nullptr;\n"
            "    u32* lb_WorldOut[2] = {nullptr, nullptr};\n"
            "    u8* lb_RowSplit[2] = {nullptr, nullptr};\n")
    replace(soft_h_433,
            "    u32 lb_EdgeLine[ScanlineWidth];\n",
            "    u32* lb_EdgeLine = nullptr;\n"
            "    /* docs/434 */\n"
            "    struct LbFree {\n"
            "        void operator()(u32* p) const noexcept { std::free(p); }\n"
            "    };\n"
            "    struct LbFree8 {\n"
            "        void operator()(u8* p) const noexcept { std::free(p); }\n"
            "    };\n"
            "    std::unique_ptr<u32[], LbFree> lb_Mem;\n"
            "    std::unique_ptr<u8[], LbFree8> lb_Mem8;\n"
            "    bool lb_Alloc(int maxW, int maxH, bool hud);\n"
            "    bool lb_IsHi = false;\n"
            "    int lb_HiN = 1;\n"
            "    std::unique_ptr<SoftRenderer> lb_Hi;\n"
            "    int lb_PendingSlot = 0;\n"
            "    bool lb_HiOk[2] = {false, false};\n"
            "    bool lb_WantOk = false;\n"
            "    bool lb_HiStarted = false;\n"
            "    bool lb_HiReady = false;\n"
            "    std::vector<Polygon> lb_HiPoly;\n"
            "    std::vector<Vertex> lb_HiVert;\n"
            "    std::vector<Polygon*> lb_HiPolyPtr;\n"
            "    int lb_HiNumPolys = 0;\n"
            "    void lb_HiFrame(GPU& gpu, int slot);\n"
            "    bool lb_BuildPolys(GPU& gpu);\n"
            "    void lb_PolyLog(const GPU& gpu, Polygon** polys, int n);\n"
            "    u8* lb_Spec[2] = {nullptr, nullptr};   /* the specials' masks */\n"
            "    void lb_SpecRow(s32 y);\n"
            "    bool lb_SpecAny[2] = {false, false};   /* a special in the slot */\n"
            "public:\n"
            "    /* The scale to composite at: lb_HiN when a hi frame holds a\n"
            "       special, else 1 (nothing to sharpen: Native, exactly). */\n"
            "    int lb_HiActive() const noexcept {\n"
            "        const SoftRenderer* h = lb_Hi.get();\n"
            "        if (!h) return 1;\n"
            "        for (int s = 0; s < 2; s++)\n"
            "            if (h->lb_HiOk[s] && h->lb_SpecAny[s]) return lb_HiN;\n"
            "        return 1;\n"
            "    }\n"
            "private:\n")
    replace(soft_c_433,
            "    const bool lb_in = lb_SplitFrame && y >= 0 && y < NumScanlines;\n",
            "    if (lb_IsHi) lb_SpecRow(y);   /* docs/434 */\n"
            "    const bool lb_in = lb_SplitFrame && y >= 0 && y < NumScanlines;\n")
    replace(soft_h_433,
            "    u8 StencilBuffer[MaxRenderWidth*2];\n",
            "    u8* StencilBuffer = nullptr;\n")
    # -- the renderer's own code
    replace(soft_c_433,
            "    RenderThreadRunning = false;\n"
            "    RenderThreadRendering = false;\n"
            "    RenderThread = nullptr;\n"
            "}\n"
            "\n"
            "SoftRenderer::~SoftRenderer()\n",
            "    RenderThreadRunning = false;\n"
            "    RenderThreadRendering = false;\n"
            "    RenderThread = nullptr;\n"
            "    lb_Alloc(1728, 768, true);   /* docs/434 */\n"
            "}\n"
            "\n"
            "SoftRenderer::~SoftRenderer()\n")
    replace(soft_c_433,
            "                lb_note_render_thread();   /* docs/358 */\n",
            "                if (!lb_IsHi) lb_note_render_thread();   /* docs/358 */\n")
    replace(soft_c_433,
            "void SoftRenderer::lb_NextColorBuffer() noexcept\n"
            "{\n",
            "void SoftRenderer::lb_NextColorBuffer() noexcept\n"
            "{\n"
            "    if (lb_IsHi)   /* docs/434: the slot the native frame took */\n"
            "    {\n"
            "        lb_Slot = lb_PendingSlot;\n"
            "        ColorBuffer = ColorStore[lb_Slot];\n"
            "        ShownColor.store(ColorBuffer, std::memory_order_release);\n"
            "        return;\n"
            "    }\n")
    replace(soft_c_433,
            "    memset(lb_RowSplit, 0, sizeof(lb_RowSplit));\n"
            "    memset(ColorStore, 0, sizeof(ColorStore));   /* docs/359 */\n",
            "    memset(lb_RowSplit[0], 0, NumScanlines);\n"
            "    memset(lb_RowSplit[1], 0, NumScanlines);\n"
            "    memset(ColorStore[0], 0, (size_t)BufferSize * 2 * sizeof(u32));   /* docs/359 */\n"
            "    memset(ColorStore[1], 0, (size_t)BufferSize * 2 * sizeof(u32));\n"
            "    lb_Hi.reset();   /* docs/434: drawn again when asked for */\n"
            "    lb_HiN = 1;\n")
    replace(soft_c_433,
            "    int j = 0;\n"
            "    for (int i = 0; i < npolys; i++)\n"
            "    {\n"
            "        if (polygons[i]->Degenerate) continue;\n",
            "    if (!lb_IsHi) lb_PolyLog(gpu, polygons, npolys);   /* docs/434 */\n"
            "    int j = 0;\n"
            "    for (int i = 0; i < npolys; i++)\n"
            "    {\n"
            "        if (polygons[i]->Degenerate) continue;\n")
    replace(soft_c_433,
            "    memset(lb_RowSplit[lb_Slot], 0, sizeof(lb_RowSplit[lb_Slot]));\n",
            "    memset(lb_RowSplit[lb_Slot], 0, NumScanlines);\n")
    replace(soft_c_433,
            "            ClearBuffers(gpu);\n"
            "            RenderPolygons(gpu, true, &gpu.GPU3D.RenderPolygonRAM[0], gpu.GPU3D.RenderNumPolygons);\n"
            "            lb_CheckAfter();\n",
            "            ClearBuffers(gpu);\n"
            "            if (lb_IsHi)   /* docs/434 */\n"
            "                RenderPolygons(gpu, true, lb_HiPolyPtr.data(), lb_HiNumPolys);\n"
            "            else\n"
            "                RenderPolygons(gpu, true, &gpu.GPU3D.RenderPolygonRAM[0], gpu.GPU3D.RenderNumPolygons);\n"
            "            if (lb_IsHi && lb_WantOk) lb_HiOk[lb_Slot] = true;\n"
            "            lb_CheckAfter();\n")
    replace(soft_c_433,
            "    FrameIdentical = !(textureChanged || texPalChanged) && gpu.GPU3D.RenderFrameIdentical;\n",
            "    FrameIdentical = !(textureChanged || texPalChanged) && gpu.GPU3D.RenderFrameIdentical;\n"
            "    /* docs/434: the hi-res partner draws this frame too, into the\n"
            "       colour slot this one is about to take. */\n"
            "    if (lb_Hi)\n"
            "        lb_HiFrame(gpu, (DisplayedBuffer == ColorStore[0]) ? 1 : 0);\n")
    replace(soft_c_433,
            "void SoftRenderer::RestartFrame(GPU& gpu)\n"
            "{\n"
            "    SetupRenderThread(gpu);\n"
            "    EnableRenderThread();\n"
            "}\n",
            "void SoftRenderer::RestartFrame(GPU& gpu)\n"
            "{\n"
            "    SetupRenderThread(gpu);\n"
            "    EnableRenderThread();\n"
            "    if (lb_Hi)   /* docs/434 */\n"
            "    {\n"
            "        lb_Hi->SetupRenderThread(gpu);\n"
            "        if (lb_Hi->lb_HiReady)\n"
            "        {\n"
            "            lb_Hi->lb_HiStarted = true;\n"
            "            lb_Hi->EnableRenderThread();\n"
            "        }\n"
            "    }\n"
            "}\n")
    replace(soft_c_433,
            "    if (lb_3dwait_on()) lb_3dwait_frame(lb_t0, RenderWidth, RenderHeight);\n",
            "    if (lb_3dwait_on()) lb_3dwait_frame(lb_t0, RenderWidth, RenderHeight);\n"
            "    lb_HiWaitDone(gpu);   /* docs/434 */\n")
    replace(soft_c_433,
            "    if (Threaded != threaded)\n"
            "    {\n"
            "        Threaded = threaded;\n",
            "    if (Threaded != threaded)\n"
            "    {\n"
            "        Threaded = threaded;\n"
            "        lb_Hi.reset();   /* docs/434 */\n")
    with open(soft_c_433, "a", encoding="utf-8", newline="") as fh:
        fh.write("\n/* docs/434: the hi-res partner. */\n"
                 "namespace melonDS\n{\n"
                 "#include \"lb_hires_soft.inc\"\n"
                 "}\n")
    _written.add(os.path.abspath(soft_c_433))
    # -- the geometry stage: each vertex's position at N times the surface
    replace(g3h_433,
            "    s32 HiresPosition[2];\n",
            "    s32 HiresPosition[2];\n"
            "\n"
            "    // legobattles-decomp docs/434: the position at lb_HiN times the\n"
            "    // surface, in those pixels; lb_HiScale is the N it was made for\n"
            "    // (0: none).\n"
            "    s32 lb_HiPos[2];\n"
            "    s32 lb_HiScale;\n")
    replace(g3h_433,
            "    u32 ScrolledAttrLine[1728]; // host presentation metadata\n",
            "    u32 ScrolledAttrLine[1728]; // host presentation metadata\n"
            "    int lb_HiN = 1;   // docs/434: the 3D resolution multiple; 1 is off\n")
    replace(g3c_433,
            "        vtx->FinalPosition[1] = posY & 0x3FF;"
            "  /* legobattles-decomp: was 0xFF, then 0x1FF */\n",
            "        vtx->FinalPosition[1] = posY & 0x3FF;"
            "  /* legobattles-decomp: was 0xFF, then 0x1FF */\n"
            "\n"
            "        /* docs/434: the same vertex at lb_HiN times the surface. It\n"
            "           keeps the native pixel (FinalPosition, whatever tweaks\n"
            "           went into it) and adds the sub-pixel the continuous\n"
            "           position has, so the two pictures agree to a pixel. */\n"
            "        vtx->lb_HiScale = 0;\n"
            "        if (lb_HiN > 1 && w != 0)\n"
            "        {\n"
            "            const s64 lb_n = lb_HiN;\n"
            "            const s64 lb_den = (s64)w << 1;\n"
            "            const s64 lb_sx = (s64)(u32)(vtx->Position[0] + w);\n"
            "            const s64 lb_sy = (s64)(u32)(-vtx->Position[1] + w);\n"
            "            const s64 lb_fx = (lb_sx * (s64)viewport_width) / lb_den;\n"
            "            const s64 lb_fy = (lb_sy * (s64)viewport_height) / lb_den;\n"
            "            s64 lb_hx = (s64)(posX & 0x7FF) * lb_n +\n"
            "                ((lb_sx * (s64)viewport_width * lb_n) / lb_den - lb_fx * lb_n);\n"
            "            s64 lb_hy = (s64)(posY & 0x3FF) * lb_n +\n"
            "                ((lb_sy * (s64)viewport_height * lb_n) / lb_den - lb_fy * lb_n);\n"
            "            if (lb_hx < 0) lb_hx = 0;\n"
            "            if (lb_hy < 0) lb_hy = 0;\n"
            "            if (lb_hx > (s64)RenderWidth * lb_n) lb_hx = (s64)RenderWidth * lb_n;\n"
            "            if (lb_hy > (s64)RenderHeight * lb_n) lb_hy = (s64)RenderHeight * lb_n;\n"
            "            vtx->lb_HiPos[0] = (s32)lb_hx;\n"
            "            vtx->lb_HiPos[1] = (s32)lb_hy;\n"
            "            vtx->lb_HiScale = lb_HiN;\n"
            "        }\n")
    # -- the runner's bridge
    replace(gpu3d,
            "const uint32_t* nds_gpu3d_wide_attr_line(int line) {\n",
            "/* legobattles-decomp docs/434: 3D resolution. */\n"
            "static melonDS::SoftRenderer* lb_hi_soft() {\n"
            "    if (g_nds.GPU.GPU3D.IsRendererAccelerated()) return nullptr;\n"
            "    return dynamic_cast<melonDS::SoftRenderer*>(\n"
            "        &g_nds.GPU.GPU3D.GetCurrentRenderer());\n"
            "}\n"
            "extern \"C\" void nds_gpu3d_set_hi_scale(int n) {\n"
            "    if (n < 1) n = 1;\n"
            "    if (n > 4) n = 4;\n"
            "    melonDS::SoftRenderer* r = lb_hi_soft();\n"
            "    if (!r) { g_nds.GPU.GPU3D.lb_HiN = 1; return; }\n"
            "    r->lb_HiSetScale(n, g_nds.GPU);\n"
            "    g_nds.GPU.GPU3D.lb_HiN = r->lb_HiScale();\n"
            "}\n"
            "extern \"C\" int nds_gpu3d_hi_active(void) {\n"
            "    melonDS::SoftRenderer* r = lb_hi_soft();\n"
            "    return r ? r->lb_HiActive() : 1;\n"
            "}\n"
            "extern \"C\" int nds_gpu3d_hi_scale(void) {\n"
            "    melonDS::SoftRenderer* r = lb_hi_soft();\n"
            "    return r ? r->lb_HiScale() : 1;\n"
            "}\n"
            "extern \"C\" void nds_gpu3d_hi_wait(void) {\n"
            "    melonDS::SoftRenderer* r = lb_hi_soft();\n"
            "    if (r) r->lb_HiWaitDone(g_nds.GPU);\n"
            "}\n"
            "extern \"C\" const uint32_t* nds_gpu3d_hi_rows(const uint32_t* line,\n"
            "                                             int* stride, int* width,\n"
            "                                             int* n,\n"
            "                                             const uint8_t** mask) {\n"
            "    melonDS::SoftRenderer* r = lb_hi_soft();\n"
            "    return r ? r->lb_HiRows(line, stride, width, n, mask) : nullptr;\n"
            "}\n"
            "\n"
            "const uint32_t* nds_gpu3d_wide_attr_line(int line) {\n")
    # -- the compositor
    edit(gpu,
         "// Composite one scanline into the internal 6-bit format (channels at bits",
         ["#include \"lb_hires_comp.inc\"  /* legobattles-decomp docs/434 */",
          ""], after=False)
    replace(gpu,
            "    if (engine == 0) g_hd_frame_valid = false;\n",
            "    if (engine == 0) g_hd_frame_valid = false;\n"
            "    if (engine == 0) g_hi_valid = false;   /* docs/434 */\n")
    replace(gpu,
            "    if (!snapshot_matches && output_height > 0)\n"
            "        (void)nds_gpu3d_wide_line(output_height - 1);\n",
            "    if (!snapshot_matches && output_height > 0)\n"
            "        (void)nds_gpu3d_wide_line(output_height - 1);\n"
            "    /* docs/434: the hi-res partner's frame is the live raster's twin. */\n"
            "    if (!snapshot_matches) nds_gpu3d_hi_wait();\n")
    replace(gpu,
            "    const bool emit_hd =\n"
            "        g_hd_emit_enabled && !g_adaptive_skybox_fill && screen == 0;\n",
            "    /* docs/434: 3D resolution wants the stack without the 3D layer too,\n"
            "       whichever screen the field is on. */\n"
            "    const int lb_hi_want =\n"
            "        g_adaptive_skybox_fill ? 1 : nds_gpu3d_hi_active();\n"
            "    const bool emit_hd =\n"
            "        (g_hd_emit_enabled && !g_adaptive_skybox_fill && screen == 0) ||\n"
            "        lb_hi_want > 1;\n")
    replace(gpu,
            "    const bool lb_obj_on = (u.dispcnt & 0x1000u) != 0;\n",
            "    const int lb_hi_n =\n"
            "        lb_hi_want > 1 ? lb_hi_alloc(output_width, output_height, lb_hi_want)\n"
            "                       : 1;   /* docs/434 */\n"
            "    const bool lb_obj_on = (u.dispcnt & 0x1000u) != 0;\n")
    replace(gpu,
            "        if (lb_fast_timed)\n"
            "            lb_fast_check_row(y, dst, lb_fast_dst, output_width,\n"
            "                              lb_fast_t0, lb_fast_t1,\n"
            "                              std::chrono::steady_clock::now());\n",
            "        if (lb_fast_timed)\n"
            "            lb_fast_check_row(y, dst, lb_fast_dst, output_width,\n"
            "                              lb_fast_t0, lb_fast_t1,\n"
            "                              std::chrono::steady_clock::now());\n"
            "        if (lb_hi_n > 1)   /* docs/434 */\n"
            "            lb_hi_row(u, y, output_width, lb_hi_n, dst, line3d,\n"
            "                      line3d_raw, prio3d, mbmode, mb,\n"
            "                      g_hd_top_pixels.data() +\n"
            "                          static_cast<size_t>(y) * output_width * 2u,\n"
            "                      g_hd_below_pixels.data() +\n"
            "                          static_cast<size_t>(y) * output_width * 2u,\n"
            "                      g_hi_out.data());\n")
    replace(gpu,
            "    if (emit_hd) {\n"
            "        g_hd_frame.top_pixels = g_hd_top_pixels.data();\n",
            "    if (lb_hi_n > 1)   /* docs/434 */\n"
            "        lb_hi_frame_done(adaptive.data(), output_width, output_height,\n"
            "                         lb_hi_n);\n"
            "    if (emit_hd) {\n"
            "        g_hd_frame.top_pixels = g_hd_top_pixels.data();\n")
    replace(gpu,
            "void nds_gpu2d_invalidate_hd_frame() {\n",
            "/* docs/434: the compositor's N-times picture for this frame, if it\n"
            "   made one. */\n"
            "extern \"C\" bool nds_gpu2d_hi_frame(const uint32_t** px, int* w,\n"
            "                                    int* h, int* n) {\n"
            "    if (!g_hi_valid || g_hi_out.empty()) return false;\n"
            "    if (px) *px = g_hi_out.data();\n"
            "    if (w) *w = g_hi_w;\n"
            "    if (h) *h = g_hi_h;\n"
            "    if (n) *n = g_hi_n;\n"
            "    return true;\n"
            "}\n"
            "\n"
            "void nds_gpu2d_invalidate_hd_frame() {\n")
    # -- the frontend
    replace(front,
            "int lb_present_src_w[2] = {0, 0};   /* legobattles-decomp */\n",
            "#include \"lb_hires_front.inc\"  /* legobattles-decomp docs/434 */\n"
            "int lb_present_src_w[2] = {0, 0};   /* legobattles-decomp */\n")
    replace(front,
            "            lb_render_field(presentation, lb_only, lb_s, lb_gw, lb_ghh);   /* docs/382 */\n",
            "            if (!lb_hires_present_field(presentation, lb_only, lb_s))   /* docs/434 */\n"
            "                lb_render_field(presentation, lb_only, lb_s, lb_gw, lb_ghh);   /* docs/382 */\n")
    replace(front,
            "        nds_gpu2d_invalidate_hd_frame();\n",
            "        nds_gpu2d_invalidate_hd_frame();\n"
            "        lb_hires_prepare();   /* docs/434 */\n")
    done.append("3D resolution: a hi-res partner renderer, composited and presented (docs/434)")

    # Last, once every patch has been written: hold the mtime of any file whose
    # patched CONTENT is unchanged since the previous build, so make does not
    # invalidate all 302 generated banks over a timestamp. See stamp_written().
    done.append(stamp_written())

    return done


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    try:
        for step in apply(sys.argv[1], sys.argv[2]):
            print("  hooked  %s" % step)
    except HookError as exc:
        print("HOOK FAILED: %s" % exc, file=sys.stderr)
        print("\nUpstream frontend.cpp or CMakeLists.txt has changed. Re-locate "
              "the anchor rather than loosening the check.", file=sys.stderr)
        return 1
    print("  all hooks applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
