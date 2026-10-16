# 334 — The rendering audit, and why the 2026-09-20 build is a wall of fog

Written after a screenshot of the 09:04 deploy: the whole battlefield is the
grey diagonal shroud hatch except one wedge of terrain at the bottom left, at
**10.6 FPS**. The fix chosen was a revert to a build from four or five days
earlier. That revert is being built from `d4ca9431` (2026-09-15 17:48).

This document is the audit the revert should have been preceded by.

## There are TWO terrain renderers and they both run

This is the fact that most of the last week's confusion comes from, and it is
not written down anywhere else.

| | `lb_widemap.cpp` — the band synthesiser | `lb_field.cpp` + `lb_fog.cpp` — the host field |
|---|---|---|
| what it draws | the widened columns either side of the guest's native 256 | the whole map, as a host-side bitmap |
| how | rebuilds BG tilemaps for the bands from the level grid | renders map pixels and shrouds them from the game's own visibility record |
| age | the original approach, months of fixes behind it | docs/325, phases landed 2026-09-19 |
| status | **this is what you actually see** | measured contributing **1 band pixel in 672** |

`[lb_hostterrain] row 247: band 1/672 kept` — of 672 band pixels on the middle
output row, the host field's colour survived to the screen **once**. 86 samples
read 0, 17 read 1, 5 read 4. The band synthesiser wins essentially every pixel,
and the host field is rendered and then overdrawn.

**So the port pays for both and shows one.** Before 2026-09-19 that was free,
because `lb_field_locate()` failed on the screenshot's level and the host field
never ran at all there. Making locate work (`5263b07b`) switched on a
whole-map render and shroud pass that contributes almost nothing.

## What the screenshot is, mechanically

`9af03fa2`, deployed 09:04, changed one declaration:

    constexpr int kShroudW = 64, kShroudH = 63;   ->   sized from the map

The screenshot's level is **96x96**. Under the old constant, cells at `cx >= 64` or
`cy >= 63` — **57% of the map** — had no entry in the mask, so
`lb_field_apply_shroud` skipped them and they drew **unshrouded whatever the
game's record said**. After the fix they are shrouded, and the game's own
record says

    [lb_shroud] record 96x96, mask 96x96: 8856 of 9216 cells hidden

**96.1% of the map is unexplored**, which is an ordinary state early in a
battle. So the wall of fog is the game's own visibility data, drawn for the
first time over the whole map.

That makes the change *more* faithful to the record and *much* worse to look
at, and both of those are true at once. The DS never showed this because the
DS never showed more than 256x192 of the map. The adaptive window does, and a
whole-map fog field is what the record actually contains.

**This is the core unresolved design question for the widened view**, and it
has never been stated: *what should the widened columns show for ground the
player has not explored?* Three answers, none yet chosen:

1. **Shroud, faithfully** — what the 09:04 build does. Correct against the
   record, and it means most of a big window is grey most of the match.
2. **Nothing / backdrop** — treat unexplored band ground as outside the
   playfield, the way a letterbox would.
3. **Terrain without units** — what the pre-09:04 build accidentally did for
   57% of the map: show the ground, hide what is on it.

The reaction to the screenshot is evidence for (2) or (3) over (1), but the
question has not been put directly, and the 09:04 change answered it without
noticing it was answering it.

## The 10.6 FPS

Not measured on this build yet, and I will not guess a single cause. What is
known:

* the host field renders **and** shrouds a 2304x1536 map every frame. Measured
  on this exact route and window: **2.587 ms whole-map, 0.430 ms clipped to
  the visible window** (docs/330). The clip is in the deployed build.
* the shroud pass now covers **9,216 cells instead of 4,032** — 2.3x the work
  it did before 09:04, and it was already the larger half.
* the 3D is software-rasterised because `nds_gpu3d_use_compute_renderer` gates
  on `render_width > 448` and `render_width % 64 != 0`, and a wide window fails
  both (docs/327). That is independent of anything changed this week and is the
  standing reason wide windows cost frame rate.

## The rendering defects, against the builds

| date | defect | status |
|---|---|---|
| 09-13 | sprites become jittery and dishevelled while in motion | **OPEN** — reproduced, not fixed. See below. |
| 09-18 | when panning the camera, sprites or even special models sometimes stretch very wide horizontally | fixed `53de0a57` (per-vertex band shift replaced by a per-frame latch); **never verified by eye** |
| 09-18 | after resizing the window, sprites nudge as the camera moves | fixed `5af03a58`; viewport/vertex disagreement 64–80 px → 0 on 48 of 51 pairs |
| 09-19 | the command cursor is still mispositioned | **OPEN** — the cursor sprite is drawn one band width too far left; only the touch *delivery* was ever verified |
| 09-19 | Eagle Command base sprites flicker/truncate seemingly at random | **OPEN** — two mechanisms closed (docs/267, 310), a third is on building sprites |
| 09-19 | the shroud re-appears glitchily as the camera pans, even in explored areas | the minimap-readback fallback samples the game's own white camera box as terrain (docs/247); the record path avoids it, and the record path is what 09:04 turned on everywhere |
| 09-20 | the screenshot above | caused by `9af03fa2`, mechanism above |

## The jitter: the best evidence, and the test that would settle it

2026-09-13 reproduced it without a camera model, which two earlier attempts had
failed to do. A building does not walk, so a stationary building must move
exactly with the ground under it:

    terrain  -32   base  -33   diff  -1    agree
    terrain  -13   base   +0   diff +13    DISAGREE
    terrain  -48   base  -31   diff +17    DISAGREE

**41 of 376 pairs disagree — about a third of the pairs where anything moved,
by up to 17 px.** That is the report, reproduced.

**The weak link, stated at the time and still true:** the terrain shift comes
from pixel cross-correlation on BG1, and this game's ground is repetitive
grass. A mis-peak looks exactly like "terrain -13 while base +0".

**The decisive test was named and never run:** compare a stationary object
against something that needs no correlation. There are two such references and
both are already instrumented:

* another **stationary object's** own `surf_x` in the same polygon list — two
  buildings must agree with each other whatever the ground does;
* the **guest camera** itself, via `lb_field_camera` (`0x02152034` +0x5E/+0x60),
  which was verified to track exactly — **0 of 683** moving frames disagreed
  after `5263b07b`. A stationary building's `Δsurf_x` must equal `−Δcamera_x`.

The second is the cleaner test: it is one subtraction per frame, it involves no
pixels at all, and it produces a signed error in pixels per frame that is
exactly "how jittery, in what direction". That is the next measurement.

## What the revert restores, and what it does not

`d4ca9431` predates the chain locate, so on the screenshot's level:

* the host field never engages — no whole-map render, no whole-map shroud;
* the band synthesiser draws the widened columns, as it had for weeks;
* the fog-mask question does not arise, because nothing shrouds the bands from
  the record.

It also loses: the visible-window clip, the map-sized mask, the session logger,
and the camera-tracking fix. None of those helps a build whose renderer is the
one being reverted away from.

**What the revert does not fix:** the jitter (09-13, predates all of it), the
command cursor offset, the building-sprite flicker, and the software
rasteriser's cost at wide windows. Those four are the actual rendering backlog
and they are older than this week.
