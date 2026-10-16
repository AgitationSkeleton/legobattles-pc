# 265 — The horizontal band bias was never applied to the 3D

Sprites still visibly drifted, as early as spawning into Free Play, and the
base initially spawned under the fog to the right instead of in the revealed,
fogless starting circle to its left. Two hypotheses: the fault is in the
ground rather than the units, and it is related to the clamp at world edges.

Both hold. It is the ground, and it is the clamp.

## The two conventions

`band_compute()` in lb_widemap.cpp clamps the band at a world edge so the
widened columns never ask for map that does not exist. That clamp is
deliberate and measured — it is what removed an earlier black slab of 34%
undrawn pixels. The guest's own 256 columns therefore sit at `band`, which is
NOT the centre of the surface whenever the clamp engages.

The 2D composite honours that. `extra` is the band, and every extension column
is indexed relative to it.

The 3D does not. In `vendor/melonds/GPU3D.cpp`, when the guest sets a
full-screen viewport, `enhanced_full_viewport` replaces it with the whole
surface at x=0:

```c
const u32 viewport_width = enhanced_full_viewport ? RenderWidth : Viewport[4];
const int lb_band = lb_band_for_geometry(RenderWidth);
const u32 viewport_x = enhanced_full_viewport ? 0 : Viewport[0] + lb_band;
```

With `viewport_width` at `RenderWidth`, an origin of 0 puts the guest's 256
columns at `(RenderWidth - 256) / 2` — the centre. So the units land at the
centre while the ground they stand on is composited at the band, and the two
are `centre - band` pixels apart.

## Measured

One line, one instant, from inside the compositor (`LB_DRIFTLOG`), surface
1728 with the camera at map x 328:

```
extra = band_left = 468      centre = 736      clamped by 268
3D pixels landed at x 609..652
  predicted from CENTRE: 619, 643      <- matches
  predicted from BAND:   351, 375
```

After the fix, same surface, same camera:

```
3D pixels landed at x 342..398
  predicted from BAND:   351, 375      err 9      <- matches
  predicted from CENTRE: 619, 643      err 277
5 of 5 sampled frames
```

The residual 9-10 px is the documented sprite anchor (`screen = world - camera
+ (19,-1)`) plus sprite extent, and it is the same 9-16 px measured at the
narrow surface before any of this. It is the anchor, not drift.

## Why it scales with the window

The clamp only engages near a world edge, and how near depends on how much
extension the surface asks for against a 1536-px-wide map:

```
surface  946  (1920x1080)   asks 690 px    clamped on SOME frames
surface 1720  (3440x1440)   asks 1464 px   nearly the whole world: clamped almost always
```

Hence the base moves far away at 3440x1440 and only a little at 1920x1080.
**One correction to a claim made while chasing this:**
at 946 the band is NOT usually unclamped. Measured over two runs, `extra` was
132 against a centre of 345 on 1453 and 1418 frames respectively, and 345 on
only 808 and 228. The error was 213 px there too; less of it shows
because magnification 2 makes 213 surface px a smaller fraction of a smaller
window, not because the bug was smaller.

## The clincher

This same defect was found, measured and fixed on the VERTICAL axis, and the
horizontal twin was never done. From the existing comment beside `lb_ybias`:

> WHERE THE GUEST'S OWN 192 ROWS MUST LAND. ... a viewport_y of 0 centres the
> guest's rows on the SURFACE -- at (RenderHeight-192)/2. The 2D layers are
> composited at lb_band_y, which is NOT that number whenever the band is
> clamped at a map edge, and the two then disagree: measured at the map's north
> edge with band_y 0, every polygon sat +12/+84/+162/+252 px low at surfaces
> 216/360/516/696 -- exactly (RenderHeight-192)/2 -- units floating below the
> ground they stand on.

`lb_ybias = lb_band_y - lb_ycentre` existed. `lb_xbias` did not. It does now,
applied identically: signed, clamped at 0 because the position is unsigned, at
the `FinalPosition` site. The same bias also goes on both `HiresPosition`
paths, which only the accelerated renderer reads — that renderer does not
initialise here ("compute render width 512 is unsupported", so the port falls
back to the threaded software renderer) but a fix that silently applies to one
renderer and not the other is the same class of trap as a band that applies to
the 2D and not the 3D.

## Gates

```
LB_THREADCHECK   786x426   2400 frames, 0 differed
                 946x516   2520 frames, 0 differed
                1266x696   2640 frames, 0 differed
                1706x696   2760 frames, 0 differed of 1,187,376 pixels
```

## Three clock bugs paid for on the way

Every one of them produced numbers that looked like findings:

1. Pairing `[lb_ent]` against layer-dump FILES. Dumps land on frames divisible
   by `every`; the entity log fires every 60 frames at its own phase. 0 of 55
   paired at `LB_LAYERDUMP=10`, then 0 of 720 at `=3`.
2. The same again after re-tuning the cadence, because the phase is per-run.
3. A private counter in the drift probe, read BEFORE `lb_layer_begin()` ticks
   the shared one — so the two lines reported different surface widths (1712
   against 1572) for the same frame.

The fix in all three cases is the same and it is now in place: ONE line, one
instant, emitted after the rows with the shared frame counter. A probe that
needs two lines joined is a probe that can lie.

Also paid for: `lb_drift_3d_extent` first tested the layer array for `80u`. The
array holds `order + 1` — BG0/3D is 2 — and the `* 40` is applied only when the
PGM is written. Testing the FILE encoding read `px3d=0` on all 3,564 frames
while dump files from the same build contained 2,361. A uniform zero is a
broken instrument, not a measurement.

## Still open

**Units in the widened columns are often not drawn at all.** Measured: 17-20
entities, none in the guest's native window, predicted columns spread
595..1775, and a single 3D cluster at 592..650 matching the two entities
nearest the native edge. `lb_wide2d.inc` records as a design decision that
"OBJECTS DO NOT EXTEND ... LEGO Battles fills OAM for a 256-wide viewport",
while `[lb_cull]` shows the gather rect IS widened at runtime
(`camx+256 -> camx+948`). That is the other half of "properly expanded to the
extended view space" and it is a separate defect from this one.

**"Occasionally some holes can be seen in the fog."** Not yet investigated.
