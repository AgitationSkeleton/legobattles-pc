# 361 — Engine A's native picture, drawn only when something shows it

Written for: whoever works on the 2D renderer or the frame's timing next.

Status: **built, verified, on by default** (`LB_NATSKIP=0` turns it off);
deployed 2026-09-24 06:05 (`7e57ab74`, runner built 05:51,
`build/LegoBattles.exe` sha256 `aa34c31f...`, settings kept).

## 1. The cost

In a widened battle frame the emulation thread still composes engine A's
own 256x192 picture, one scanline at a time: BG decode, OBJ, the compose,
master brightness and the row write. The widened composite then replaces it
on screen. That is ~3.9% of the emulation thread at the widest surface,
about 0.64 ms a frame (docs/358 s2). It is the same work at every widened
size, and all of it is on the critical path.

## 2. When the native picture is shown

The composite for frame N runs at the frame boundary. By then the guest's
VBlank handler has written frame N+1's registers, so the composite decides
from the NEXT frame's configuration. When it refuses the scene, it shows
frame N's native picture (through `lb_menu_fill`). A skip decided at
scanline time must therefore survive exactly one frame: the one where the
game leaves the battle configuration.

**The decomp says how it leaves.** Master brightness (0x0400006C and
0x0400106C for the two engines) is written through a fade object:

- `FUN_0200d9bc(fade, duration)` starts a fade toward -16 (full black) on
  either engine;
- `FUN_0200da2c` starts a fade back;
- `FUN_0200dad8` steps it (interpolated over the duration, written through
  `FUN_0210ad98`) and posts a completion message when it ends.

Scene changes call these in pairs (`FUN_020a2264` fades both screens out
over 8 units, `FUN_020a1a6c` fades them back in). A line drawn at full fade
is exactly `0xFF000000`, whatever its layers: darken by 16 zeroes every
channel. Brighten by 16 gives `0xFFFFFFFF`.

**Measured** (`LB_NATSKIP=probe`, nothing skipped). Per scanline, the probe
records whether the composite would take the line's frame, using its own
conditions as they stood then. When such a frame is then shown natively,
it compares the rendered rows with the fill colour, pixel for pixel.

| route | widened battle frames, all 192 lines skippable | shown natively |
|---|---:|---|
| the reference replay | 8,583 | none |
| a battle paused and unpaused (`--pause-cycle`) | 2,832 | **one** frame: 192 lines at full fade, all 192 rows exactly the fill colour |

The pause switches engine A from the battle configuration (DISPCNT
`00010F08`) to the pause menu's (`00010D0B`), behind the fade. The unpause
goes the other way; lines drawn in the menu configuration are never skipped.

## 3. The skip

- **Per scanline**, engine A checks what the composite checks:
  - display mode 1;
  - BG0 is the 3D layer and BG1-3 are text layers;
  - no display capture this frame, and not a direct-present frame;
  - Enhanced is on, the title widens that screen, and the surface is wider
    or taller than the DS.

  A line that passes composes nothing and writes nothing. The rest of the
  scanline (the 3D snapshot, docs/359; the widemap scan) still runs. The
  line's master brightness is recorded.
- **Completion.** `nds_gpu2d_framebuffer()` now fills any skipped line in
  before it returns. A line at full fade gets the exact fill colour, the
  case measured above. Any other line is composed late, with the registers
  as they stand then (the next frame's), which is inexact. That path is
  logged and has never been seen. The composite and the frontend's
  per-frame fetch use `nds_gpu2d_framebuffer_raw()` and complete the
  picture only when they really show it.
- **Readers that need the true picture** call `nds_gpu2d_want_native()`,
  which stops the skipping for 120 frames:
  - the debug server's native `framebuffer` command;
  - the frame dumps;
  - the battlefield panel and wide-field view (`lb_field.cpp`), which are
    opt-in.

  In normal play none of them runs.
- `LB_NATSKIP=0` turns it off, `=probe` measures, `=force` skips even for
  readers (a test), and `=ab` alternates every 300 frames and times engine
  A's scanlines.

## 4. Verified

With the skip on (the default):

- **The exactness gate:** identical at all six frames. Nothing the guest can
  see changes: capture frames are never skipped, and a skippable line has
  no affine layer whose reference points the skip could leave unadvanced.
- **Layer maps and composites** inside the replay: 8 of 8 byte-identical at
  1473x830 and 8 of 8 at 1280x720. In that run every line of 7,549
  widened battle frames was skipped, and no native picture was needed.
- **Vanilla:** 240 of 240. Nothing is skipped when widening is off.
- **What is presented**, the skip forced even for readers (`=force`)
  against the skip off, 20 consecutive frames of the replay: presented
  frames 40 of 40 identical. The native dumps of the bottom screen differ
  in 9 of 20, as they should. That test makes a dump read skipped lines
  that were not at full fade, so they are composed late. In normal
  operation such a reader asks for the true picture first.
- **The pause exit, skip on:** the frame that shows the native picture had
  192 skipped lines, and all 192 were completed exactly, none late. The
  probe had shown that fill is pixel for pixel what the native path draws.

## 5. Measured

`LB_NATSKIP=ab` skips in every other block of 300 frames and times engine
A's scanlines per frame. The reference replay at the widest surface
(1720x720 at pixel scale 1 = 3440x1440):

| blocks | drawn | skipping |
|---|---:|---:|
| before the battle (nothing skippable) | 0.67-0.75 ms | 0.63-0.75 ms |
| in battle, 10 pairs | 0.74-0.83 ms | 0.135-0.145 ms |

**About 0.65 ms a frame** off the emulation thread in battle, the same at
every widened window size. What remains of engine A's scanlines (~0.14 ms)
is the 3D snapshot's row pointers, the widemap scan and the per-line check.

**The widest window, before and after today** (docs/358-361). The same
replay and surface, the machine quiet both times (2-3% load before the
run), the frontend's last 6 s of the battle:

| | this morning (`43483414`) | now (`7e57ab74`) |
|---|---:|---:|
| emulation | 12.02 ms | 10.60 ms |
| present (of which the composite) | 3.88 (2.72) ms | 3.62 (2.56) ms |
| idle, pacing | 0.61 ms | 2.42 ms |
| frames with more than 16.7 ms of work | 197 of 363 | 42 of 360 |
| more than 20 ms | 4 | 0 |
| present cadence, standard deviation | 2.13 ms | 0.44 ms |
| audio underruns, whole run | 11 | 0 |

One run each, so the digits are weather. The direction is not: the frame
now fits with about 2.4 ms to spare, and its cadence is as even as the
narrow windows' (0.5-0.8 ms). Under a loaded desktop the margin is
what absorbs the slowdown.

## 6. Not covered

- The end of a battle, a victory or a defeat, was not exercised: the
  harness has no route that finishes one. **The decomp covers it**
  (2026-09-24, with the RTTI-named corpus: docs/200, "Naming the
  virtuals"). The battle view,
  `User::WorldViewScreen`, leaves through `FUN_020dacb4`, which calls
  `FUN_020a2334`. That routine:
  - subscribes to game events 0xf and 0x1f (0x1f is the fade-complete
    message `FUN_0200dad8` posts);
  - starts an 8-unit fade to black on both screens (`FUN_0200d9bc` twice);
  - stops the music.

  The scene changes on the fade-complete event, so the frame that leaves
  the battle configuration is at full fade: exact, like the measured pause.
  The pause menu's own actions (`020d65c0`-`020d6d18`) use the same
  routine and its sibling `FUN_020a2264`. An exit that did not fade would
  show its one frame composed late, which is inexact but whole, and logged.
- In a harness run the frame dumps ask for the true picture every
  `LB_DUMP_EVERY` frames (default 30), so the skip is mostly off there.
  Timing runs set `LB_DUMP_EVERY=100000`, as they already do.
