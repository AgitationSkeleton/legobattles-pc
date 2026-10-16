# 268 — The frustum is symmetric about the camera and the band is not

Clicking around, dragging and pressing arrow keys on a test window caused
noticeable sprite deformation and shifting, so the band-bias work was not
finished. docs/265 verified the horizontal band bias against a
static camera and a scripted pan of six steps, and never against sustained
camera motion. The defect lives in motion, and specifically in motion INTO A
WORLD EDGE.

## Reproduced

A longer pan (14 steps, 0.6 s holds) with `LB_LAYERDUMP=30` and
`LB_DRIFTLOG=1`. `lb_comp-1712x730-1530.png` has the player's base drawn as a
complete building; `-1560.png` has the same base as a one-pixel-wide vertical
sliver and the unit beside it as a smear. `px3d` -- the count of pixels the 3D
layer wins -- measures it:

```
 f     px3d   3Dbox x       extra  band
1548   2306   315..373        983   1015
1558   1817   335..375       1063   1095
1560    945   351..375       1079   1111
1562    190   367..374       1095   1127
1564      0    -1..-1        1095   1143
```

## Three instruments that measured themselves

Recorded because each looked like a finding and cost a round trip:

- **`extra != band_left`** on 147 of 1527 frames. By design: the composite pairs
  with the 3D snapshot's band, and `extra == snap` on ALL frames.
- **`fill_cam != cam_tx`** on 122 frames. Also by design: `g_cam_tx` has two
  writers, and `choose_camera` deliberately picks the OLDER camera that explains
  the tilemap on screen. The difference IS the lag being corrected.
- **`cam_tx*8 - 48 != fieldcam`** on 145 frames. A tile index compared against
  pixels -- the kCameraToMap conflation lb_widemap.cpp warns "made this wrong
  four times", made wrong a fifth.

And the one that mattered: `lb_drift3d`'s box is a **union over the whole 3D
layer**, and the in-field HUD is 3D geometry that is screen-fixed. So the box
says "nothing moved" whatever the units do, and a tracking test built on it
returned 92.5% non-zero residual that was entirely the HUD.

## The instrument that could see it

`gx_polygons` is the render list AFTER the viewport transform: one screen rect
per polygon, per object rather than per frame, and it is the thing the clipper
drops. `--polysample` samples it mid-hold at every pan step, which is where the
transient lives.

```
step burst  cam    gx_polygons count
   0     0   755        3       (min_x 312..335)
   0     1  1027        0
 ...                    0    <- all 52 remaining samples
```

**The render polygon list is empty.** Not mispositioned, not partly clipped:
zero polygons survive. So the fault is in the geometry stage, before
rasterisation, which clears the compositor and every placement theory.

## The cause

`SubmitVertex` widens the horizontal frustum by shrinking clip X by
256/RenderWidth -- that is what reveals the extra world the band shows, at
unchanged pixels-per-world. It widens SYMMETRICALLY about the camera.
`band_compute` clamps the band ASYMMETRICALLY at a world edge.

At the map's east edge on a 1712-wide surface: the camera stops at 1240 on a
1536-px map, the clamp puts the whole extension west (band 1370), so the visible
world runs -130..1496 -- while the frustum, half-width RenderWidth/2 = 856,
covers 384..2096. The units at world ~350 are west of 384 and are clipped away.

The shift `lb_xbias` existed to place them, but it ran at the VIEWPORT
transform, which is after clipping: it places what survived the frustum and
cannot recover what the clipper dropped.

The second symptom follows from the first. `extra` comes from
`g_wide_3d_band[g_front]`, updated only when a 3D frame is snapshotted, so once
nothing renders the whole 2D composite freezes at the last band -- 1095 against
a live 1370, a 275 px shift that persists for the rest of the pan.

## The fix, and the one that was tried first and reverted

The shift moves into `SubmitVertex`, per vertex, before the clipper:

```c
Position[0] += (Position[3] * 2 * (band - centre)) / RenderWidth;
```

and `lb_xbias` becomes 0. `LB_FULLVP_X=0` restores the old arrangement.

**The clip matrix was tried first and is wrong.** Putting the same translation
in `UpdateClipMatrix` puts it in force for clipping, which is the property
needed -- but the guest does not rebuild its clip matrix every frame, so the
band baked into it goes stale as the band moves. Measured: the units slid ~27 px
per 32 px of camera travel, and zero-3D frames went from 65.1% to 86.8%. A
clipping fault traded for a tracking fault. `SubmitVertex` runs per vertex, so
the band it reads is always this frame's.

## Verified

```
                                BEFORE          AFTER
gx_polygons samples empty       55 of 56        0 of 56
zero-3D gameplay frames         3620/5559 65.1%  548/5718 9.6%
polygon widths                  23, 56, 25      24, 57, 25
LB_THREADCHECK                  --              21 reports, 7 sizes, 0 differing
fps                             55.98 56.97 56.14   57.57 57.69 57.81
emu_ticks                       12.15 11.94 12.05   11.84 11.83 11.77
```

The widths matter as much as the counts: a translation must not change
pixels-per-world, and the vertical axis's own history (docs/251, "you're just
stretching sprites vertically") is what that check exists to prevent repeating.

## The residual, closed

The zero-3D frames that remain are not a leftover of this defect. They are one
contiguous stretch during the harness's window-resize sweep, with the camera
parked at the map's east edge while the surface shrinks -- and the units are
legitimately off screen there. The visible world span is `cam - band .. cam+256`:

```
  612 wide   band 356   visible  884..1496    base at world ~350 outside   px3d 0
  996 wide   band 740   visible  500..1496    outside                      px3d 0
 1252 wide   band 996   visible  244..1496    inside                       px3d 699
```

The 3D returns at exactly the width where the base re-enters the span. That is a
boundary the arithmetic predicted before the data was read, which is the only
reason to believe it rather than assume it.

For scale, against the same measurement before the fix:

```
  BEFORE   longest zero-3D run  3615 frames (f=1564..5178)  -- mid-pan, never returns
  AFTER    longest zero-3D run   542 frames (f=4510..5051)  -- the resize sweep, explained above
```

The other two runs are four frames at boot and a two-frame transient at a band
change.
