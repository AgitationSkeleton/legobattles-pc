# 352 — A HUD element's fog cut-out outlived it

Written for: whoever audits the widened field next.

Status: **built, measured, deployed** 2026-09-23 21:29 (`1f57ef81`) --
`build/LegoBattles.exe` sha256 `41906c12...`, byte-identical to the build
measured here, the player's settings.json kept.

docs/351 closed with one open item: at 1280x720, a 1-pixel-tall, 16-pixel
line at surface (0..15, 35), over unexplored shroud in the top-left band,
changing colour slowly. It is not a stray tile or a band-fill error. It is
**real terrain showing through the fog**. The fog's cut-out for a relocated HUD
corner element outlived the element, one row per row band, for as long as
no element came back. Fixed by moving two assignments to the top of the
function that owns the mask. What follows is how that was established, and
what an audit of the rest of the compositor's threading found.

## 1. What the line is

`LB_LAYERDUMP=1285` on the reference replay (`build/lb-session-1790122638.lbdemo`,
`LB_WINDOW=1280x720`, surface 640x360, the native field at (192,84)) writes
the winning layer per pixel. At the line, **BG1 (terrain) wins**. Directly
above and below it, and everywhere around it, BG2 (the fog) wins:

    y 34 ................................
    y 35 ,,,,,,,,,,,,,,,,................     , = BG1 (terrain)
    y 36 ................................     . = BG2 (fog)

The colours are the terrain's own: water navy `000038`, grass `51DB41`, dirt
`AA6128`. They change as the camera moves the ground under a hole that
stays fixed on the screen. In frame 3855 the row's pixels are exactly the
ground that frame 5140 shows once that area is explored. The hole is at the
same surface position in frames 1285, 2570, 3855 and 6425. It is 15 or 16 px
wide, and it starts at surface x 0 whatever the scroll bias does to the tile
edges, so it is anchored to the surface, not to a tile.

## 2. Why

The compositor draws the fog IN FRONT of the in-field HUD (BG2 priority 1,
the 3D layer 2). So where this port moves a HUD corner element into a widened
band, it removes BG2 and BG3 under that element's opaque pixels, or the
element would land on cloud and vanish (lb_hud.cpp, the note on `g_cover`).
In gpu2d.cpp's row body:

    if (lb_cover && lb_cover[x] && (line.target & 0x0Cu)) continue;

`lb_cover` comes from `lb_hud_cover_row(y)`, documented as "the mask for the
line lb_hud_shift_line was last called with". That was not true.
`lb_hud_shift_line` reset the mask (`g_cover_y`, `g_cover_any`) only on the
path that goes on to draw. Its two early returns left it as they found it:
no boxes this frame (`g_count <= 0`), and no box on this row. The in-field HUD
exists only while something is selected (docs/194), so every
deselect takes the first early return on every row.

The state is `thread_local`, and `lb_parallel_rows` uses FIXED bands:
`band = ceil(rows / n)`, the calling thread takes band 0, and worker i takes
band i, every frame. So each thread's mask is the one its last covered row
left. That thread is handed the same row on every later frame, and the row
keeps the cut-out until a box returns. A row carries a hole only if that
last covered row was one where a moved element drew, not merely a row of
its source span. At 640x360 on this 12-core machine (ten workers, 36-row
bands):

- band 0 ends at row 35, inside the top-left element's destination rows. It
  is 15-16 px wide, which is why the hole is. Frame 7710 has the element on
  screen, at surface x 0..14.
- band 9 ends at row 359, the last row of the surface, inside the bottom-left
  element's destination. That is a second hole: x 0..22/23. It sits on the
  frame's edge, so it has no fog below it to show against, and nobody had
  seen it.

The other bands' last covered rows are rows where only the element's source
span was vacated, which set no cover. The holes therefore depend on the
window size and the core count, which is why the other three sizes in
docs/351 showed nothing.

It was a map leak as well as a picture fault. The pixels were the true ground
under unexplored cloud.

**Since when:** the mask arrived with the top/bottom anchoring,
`e245e1be` (2026-09-04). It was a plain global then, and the single-threaded
compositor could already return it for the last covered row in a later
frame. `e3d49484` (2026-09-10) made it `thread_local` for the row workers,
which gave every band its own stale row.

## 3. The fix

`lb_hud_shift_line` now sets `g_cover_y = y; g_cover_any = false;` before
either early return, so the mask is always this call's or none. That is the
contract `lb_hud_cover_row` already stated. (lb_hud.cpp, the note at the top
of the function.)

In the same change: `lb_reshift_line`'s function-`static` scratch vector is
now `thread_local`. It is called from every row worker, and a shared buffer
would hand each worker the row another one is writing. It is only reachable
with `LB_RESHIFT` set.

## 4. Verified

**The A/B, same load.** The deployed pre-fix build (b6e68e13, copied into the
harness folder as `nds_runner_b6e68e13.exe` so the pinned settings apply)
against the fix, same replay, same window, both runs 0 underruns:

| composite | layer winners changed | which | where |
|---:|---:|---|---|
| 1285 | 39 | BG1 -> BG2, all | y 35 x 0..15; y 359 x 0..22 |
| 2570 | 39 | BG1 -> BG2, all | y 35 x 0..14; y 359 x 0..23 |
| 3855 | 39 | BG1 -> BG2, all | y 35 x 0..14; y 359 x 0..23 |
| 5140 | 24 | BG1 -> BG2, all | y 359 x 0..23 |
| 6425 | 278 | 40 BG1 -> BG2; 238 BG0 <-> BG1 | the two rows; a unit's 3D pixels |
| 7710 | 0 | -- | (the corner element is on screen) |

Every changed winner outside the 3D layer is fog restored at exactly those
rows. The 238 BG0 <-> BG1 swaps at 6425 are one unit's 3D pixels, drawn
slightly differently. That is not this change, which never touches the 3D
layer. It is not the docs/348 raster pick either, which decides from guest
camera tags alone (`lb_pair_pick`). Composite 6425 is past the end of the
replay, as the next paragraphs show, so the two runs were no longer playing
the same game.

`tools/lb_thinholes.py` (new) reads the maps and reports every 1-2 px run of
terrain, 3D or backdrop with fog on both sides. Pre-fix it finds 4 holes,
the row-35 one in four frames. With the fix it finds 0. It cannot see the
row-359 hole, because an edge row has no outer neighbour. That limit is
stated in the tool.

**The exactness gate** (1473x830, pinned settings, the same replay) is
identical to the instruction at 1500, 3000, 4500, 6000 and 7500. A sixth
point for future runs: frame 6425, insn9 1,451,835,104, insn7 89,797,212.

**Vanilla** (`--adaptive-widescreen none`, LB_CADENCE=20, 240 frames from
5000): 240 of 240 pixel-identical to the 19:28 build.

**Window sizes, with the fix** (the replay, `LB_LAYERDUMP=1285`, six maps
each). Holes are counted by `lb_thinholes.py`. Cuts are read by
`lb_spritecut.py`: unit pixels either side of the native picture's edges,
where a cut would show as a cliff between -1 and 0. The replay plays out
differently at each size, because its touches are recorded in host
coordinates, so each row is a different stretch of battle and the timings
are not a controlled comparison:

| window | surface | thin holes | cut at the native edge | fps | emu / adaptive ms | frames > 16.7 ms of work | underruns |
|---|---|---:|---|---:|---|---:|---:|
| 1280x720 | 640x360 | 0 (4 before) | none | 59.8 | 9.6 / 0.9 | 1 | 0 |
| 1920x1080 | 960x540 | 0 | none | 59.8 | 11.4 / 2.3 | 140 | 2 |
| 2560x1080 | 1280x540 | 0 | (no units at the edges) | 59.7 | 12.6 / 2.9 | 194 | 80 |
| 1080x1350 | 540x674 | 0 | none | 59.7 | 11.2 / 1.5 | 64 | 0 |
| 1024x768 | 512x384 | 0 | none | 59.8 | 11.4 / 0.9 | 33 | 0 |
| 3440x1440 | 1536x642 | 0 | (no units at the edges) | **58.9** | 11.8 / 3.8 | 206 | 44 |

The compositor's cost follows the surface's pixel count: 0.9 ms at 197k
pixels, 2.9 ms at 691k, 3.8 ms at 986k. Emulation follows the scene instead:
9.6 ms at 640x360 and 11.4 ms at the smaller 512x384. At 2560x1080 the
frame's work is 16.5 ms against a 16.7 ms budget, with 0.1 ms idle. At
3440x1440 it is the whole budget, and that is the one size in this sweep
that did not hold 60 fps. Any other load on the machine turns into
underruns. The machine carried other desktop load during the sweep.
**Not yet 60 fps at
the widest windows**, and the emulation is the larger share. Section 6.

**Two differences at composite 6425, explained: that dump is past the end of
the replay.** The pre-fix run that first showed the line (20:13, under another
project's build, 30 audio underruns) has a composite 6425 that shows the whole
scene about 45 px further along than both quiet runs: ground and 3D
together, a best-shift match of 96% against 91% unshifted. And the two quiet
runs differ there by 238 pixels of one unit. Both come from the dump numbers,
not the port:

- The dump counter ticks once per WIDENED engine-A composite. Menus,
  loading and native-size frames return before `lb_layer_begin` and do not
  tick, so the number is not the emulated frame. `[lb_layerdump]` now prints
  the replay's own frame beside it. On this replay, at both 1473x830 and
  1280x720, composite N is replay frame N + 2216: 1285 is 3501, 2570 is 4786,
  3855 is 6071 and 5140 is 7356.
- So composites 6425 and 7710 are replay frames of about 8641 and 9926, past
  the recording's last event at 8188 (the print shows 8188 for both because
  the counter stops with the replay). There the harness probes the camera at
  wall-clock moments, and two runs part: docs/349 found the same for the
  gate. 7710 happened to agree.

The valid comparison frames above are therefore 1285 to 5140, and there the
only change is the fog restored.

**The gate under load.** Settled directly: the exactness gate with a 10-process
CPU burner running on the 12-core machine (41.7 fps, 3,254 underruns, present
sd 6.9 ms) is identical to the quiet run and to the reference at all six
frames. Host load does not reach the guest during a replay.

## 5. The rest of the row workers, audited

A read-only sweep went through everything the row body calls, directly or
transitively, for the two shapes found here. One is shared mutable state
written from the workers. The other is thread-local state stamped with a row
and not reset on every call. Checked and confirmed in the source:

- **No other stale-stamp state.** There are five `thread_local`s in the
  tree: the four in lb_hud.cpp and `lb_reshift_line`'s scratch. The two
  scratch buffers carry no row stamp and are rewritten in full before their
  pointer is returned.
- **One latent pixel race on the default path, not triggered in this game:**
  melonDS's `GPU3D::GetLine` (vendor GPU3D.cpp) returns its one shared
  `ScrolledLine[1728]` member whenever the 3D layer's X scroll
  (`RenderXPos`) is non-zero. With ten workers it would hand each row
  another row's line. The field keeps BG0's scroll at 0 (measured, the note
  at lb_widemap.cpp's clear-tile supplier), so it never fires. It would be
  the first suspect for torn 3D rows in any scene that scrolls BG0.
  `GetAttrLine`'s twin needs the skybox fill as well, and that is off for
  this title.
- **Debug-only:** `LB_MAPAUDIT` lets several workers `assign()` the same
  audit vectors (`g_band_served`, `g_fog_served`, `g_fog_frame`) while
  others index them, and labels records with another worker's band. That can
  corrupt the heap on the first frame after it is armed. Treat a crash under
  `LB_MAPAUDIT` as this before anything else. `LB_HUDSCALE`'s lazy read can
  race so that one band scales its HUD differently for a frame. Every other
  default-path race is on a counter no picture reads.
- **A comment describing wiring that does not exist:** `g_lb_obj_band` and
  `g_lb_obj_band_valid` are set by the composite (item #5's note in
  gpu2d.cpp) and never read. `render_obj_line` takes the sprite band from
  `lb_band_left`. OBJ is off in the field, so nothing shows.

## 6. Open

- `ScrolledLine` (section 5), latent.
- For whoever uses the layer dumps next: `LB_LAYERDUMP=1285` on this replay
  lands two of its six dumps after the recording ends. Use a period that
  keeps the dumps below composite 5972 (replay frame 8188 - 2216), or read
  the replay frame the dump line now prints.
- Frame budget at the widest surfaces (section 4): 2560x1080 leaves 0.1 ms
  a frame, and 3440x1440 ran 58.9 fps. The next step is `LB_SAMPLE` at one
  wide size over the whole replay, to rank what the emulation thread spends
  its 11-12 ms on.
