# 320 — The placement footprint draws in the bands

docs/318 measured the placement-footprint bug end to end and docs/319 recorded two dead
implementations and why each died. This is the one that works, and the picture
that proves it.

## The shape that works

Two overrides, and **no nested guest call anywhere** — which is what both
earlier attempts got wrong.

| when | `func_020370B4` (painter), before `original()` | `func_02036FBC` (post), before `original()` |
|---|---|---|
| ghost inside the guest's window | nothing at all | nothing |
| ghost at or past an edge | camera ±band, in whole metatiles | harvest that band; rebuild the native window from it; restore the camera |

`func_02036FBC` is the post hook because it is the right place rather than a
convenient one: it has exactly one caller, `func_02037EC0`; it is the very next
call after the painter (`0x02037F60` against `0x02037F44`); every path through
that function reaches it; and it **uses the camera itself** —
`func_0203CA10(&out, view, &in)` — so the restore has to land before it runs.

The guest calls its own painter, on its own schedule, so the painter is free to
spin on the hardware divider for as long as it likes (docs/319 — that spin is
what killed the synchronous call). Nothing here calls guest code out of turn.

## The native window is reconstructed, not restored

The first working version kept a copy of the native buffer on one frame and
wrote it back on the next. It harvested correctly and it was wrong twice over:

* **stale.** Two frames in three showed a native buffer up to two frames old.
* **starved.** `saves=7 restores=1` over a whole placement, because
  `func_02037EC0` calls the painter only when the placement state *changes*,
  not every frame. Alternating native/band frames when there are seven painter
  calls in total misses a band that is needed briefly — which is exactly why
  one run printed `needed=1` and never harvested the right band once.

No copy is needed. The shift is a whole number of **cells** — 240 px / 8 = 30 —
so a cell at shifted column `x` sits at native column `x + 30`, and the cells
whose native column lands back inside 0..31 are precisely the sliver the guest
would have drawn itself. `lb_fp_native_rebuild` fills rows 0..23 with the inert
tile and writes that overlap back.

Exact, no lag, and **every cell value still came from the cartridge's own
painter**. The arithmetic is a shift, not a predicate; `func_0205C034` is never
called from this port and its eleven-caller placement rule is never replicated.

## Measured

```
[lb_fp] band LEFT  harvest: 24 footprint cell(s) at camera shift -240 px  extends3=1 served=71296
[lb_fp] band RIGHT harvest: 24 footprint cell(s) at camera shift +240 px  extends3=1 served=162304
saves=3 restores=3 abandoned=0        57.5 fps
```

Four separate things, each of which could have been the gap:

1. the guest paints the band under the shifted camera — **24 cells harvested**;
2. `lb_widemap_extends(3)` turns on while a harvest is live — **extends3=1**;
3. the compositor asks for those columns and gets them — **served** counts the
   cells actually handed over, and 71,296 over ~300 placement frames is about
   237 a frame, against 24 cells × 8 scanlines = 192, so it is the right order
   rather than a stray;
4. the camera always comes back — **abandoned=0**, and `saves == restores` at
   every sample.

And the drag table says the reconstruction is right: ghost fully outside leaves
the native window empty (`32,26..-1,-1`), ghost partly outside leaves exactly
the clipped sliver (`0,12..1,15` on the left, `26,12..31,15` on the right).

## The picture

`screenshots/exp-band33/route/bf-4-LEFT-BAND.png` and `bf-4-RIGHT-BAND.png`,
1600x900 on a 786x426 guest surface, so native columns are guest x 265..521.

* **Left band**: a 6x4 **green** footprint at window x 330..430 — guest x
  165..215, outside the native window on the left.
* **Right band**: a 6x4 **red** footprint at window x 1100..1200 — guest x
  550..600, outside it on the right.

Green is `0x4F`, placeable; red is `0x3F`, blocked. The right-hand site sits on
the impassable rock border, and docs/318 measured the guest returning
`verdict=0 blocked=18` there. **So the band is not showing a substitute marker;
it is showing the cartridge's own per-cell verdict, and the two bands disagree
because the ground does.**

## One defect the picture found

The right-band screenshot still showed the LEFT band's green footprint. The
left table had been harvested a moment earlier and nothing ever retired it —
two ghosts, one of them a memory.

`lb_widemap_scan` now latches **which** band may be drawn, from
`lb_fp_band_needed()`, and `lb_fp_band_tile` serves only that one. The moment
the ghost moves — to the other band, or back inside the guest's window — the
old table stops being drawn. Latched once a frame for the same reason the
terrain band is: a value that changes mid-render draws one frame with two
geometries.

## Cost

Nothing at all while the ghost is inside the guest's own window, which is most
of a placement: `lb_fp_band_needed()` returns -1 and the override is a tail
call. While it is outside, one extra painter run per painter call — and the
painter runs on state change, not per frame — plus 832 halfword reads for the
harvest and 768 writes for the rebuild.

Measured across the runs that exercised it: **57.5, 57.7, 56.7 fps**, against
57.2 on the same route with the feature off. The `[lb_fp]` logging is behind
`LB_FPLOG` and off by default; the counters that stay are increments on a path
that only runs during placement.

## The stale band, fixed and re-measured

The re-run after the latch shows the right band alone: the red footprint at
window x 1100..1200 and **nothing at x 330..430**, where the previous run left
a green ghost behind. `screenshots/exp-band34/route/bf-4-RIGHT-BAND.png`.
58.2 fps.

## Status: FIXED

`LB_FPBANDS` now defaults to **on**, and `LB_FPBANDS=0` turns it off. It was
off while the fix did not work; it does now, and a fix nobody can see is not a
fix. Every claim above is a measurement rather than an argument:

| claim | measurement |
|---|---|
| the guest paints the band | `band LEFT/RIGHT harvest: 24 footprint cell(s)` |
| BG3 is extended while live | `extends3=1` |
| the compositor draws those cells | `served=71296`, ~237/frame against 24 cells x 8 scanlines |
| the camera always comes back | `abandoned=0`, `saves == restores` at every sample |
| the verdict is the cartridge's | green left, red right, on ground docs/318 measured as `blocked=18` |
| it costs nothing | 56.7-58.2 fps, against 57.2 with the feature off |
| one ghost, not two | the right-band shot after the latch is empty at x 330..430 |

**Not verified:** a play session. The route this was driven through is the
harness's, and it reaches placement roughly one attempt in two. Nothing here
has been seen by a person dragging a building with a mouse.
