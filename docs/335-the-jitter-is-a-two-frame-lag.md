# 335 — The jitter is a two-frame lag, measured

The defect: while the camera is in motion, sprites become jittery and
dishevelled (reported 2026-09-13 and again 2026-09-20).

Two earlier attempts at this are retracted. This one has a mechanism and a
number, and it uses **no pixel correlation and no camera proxy**, which is what
sank both of the others.

## What the earlier attempts got wrong

* **`chosen - rcam`** (`37ad4aa3`) reported the 2D and the 3D drawn from
  cameras up to 136 px apart in 86% of moving frames. **Retracted**: `rcam` is
  a VBLANK camera sampled inside the vertex transform, not the camera the game
  used for those vertices. Stale by construction.
* **Terrain-vs-building by cross-correlating BG1** found 41 of 376 pairs where
  a stationary building did not track the ground, by up to 17 px. Its own
  caveat: this game's ground is repetitive grass, and a correlation mis-peak
  looks exactly like the finding.

## The measurement: `tools/lb_jitter.py`

Two statistics from the `LB_POLYLIST=1` dump alone.

**1. Is the 3D internally rigid?** Within one frame's polygon list, every piece
of world geometry must shift between frames by the same amount, whatever the
camera did. Polygons are matched across frames by nearest centre within a
(tex_param, tex_palette, vertex_count) group, and **polygons below z = 5,000,000
are excluded** — docs/212 puts the in-field HUD at 4,259,328/4,267,520 and the
health bars at 4,718,080, and a screen-locked quad would report its own
stillness as the world's jitter. That is exactly how `lb_drift3d` once produced
a 92.5% residual that was entirely HUD.

    30,269 polygons matched over 830 moving frame pairs
    RIGID: 2,474 of 2,490 shifts within +/-1 px of the frame's mode  (99.36%)

**The 3D is rigid.** Sprites do not jitter against each other.

**2. Does the ground track the geometry?** `mode + d(cam)` should be zero: a
camera moving right by N puts the world N to the left. It is not zero, and the
residual is the same whether the pixel camera (`cam`) or the tile camera
(`chosen`, which is `ctx*8`) is used — so it is **not** tile quantisation,
which was the first thing worth ruling out.

## The raw trace, which is what actually settles it

Aggregates could not distinguish a lag from noise. Thirty consecutive frames
across one pan:

    frame   cam   dcam  mode  resid
      998   166    +2    +0     +2
      999   166    +0    +0     +0
     1000   170    +4    -2     +2
     1001   170    +0    +0     +0
     1002   178    +8    -4     +4
     1003   178    +0    +0     +0
     1004   186    +8    -6     +2
     1005   186    +0    +0     +0
     1006   190    +4    -8     -4
     1007   190    +0    +0     +0
     1008   192    +2    -4     -2
     1009   192    +0    +0     +0
     1010   192    +0    -2     -2

Two facts, both plain:

**The camera updates on alternate frames only.** `dcam` is non-zero on every
even frame and exactly 0 on every odd one. The guest advances its camera at
30 Hz while the compositor produces 60 frames.

**The geometry lags the ground by two frames.** Align `mode(N)` against
`dcam(N-2)`:

| geometry | camera two frames earlier |
|---|---|
| `mode(1000) = -2` | `dcam(998)  = +2` |
| `mode(1002) = -4` | `dcam(1000) = +4` |
| `mode(1004) = -6` | `dcam(1002) = +8` |
| `mode(1006) = -8` | `dcam(1004) = +8` |
| `mode(1008) = -4` | `dcam(1006) = +4` |
| `mode(1010) = -2` | `dcam(1008) = +2` |

Five of six match exactly and negated. The sixth (-6 against +8) is one
rounding step inside an accelerating pan.

**Frame 1010 is the clearest single line in the whole trace:** the camera has
stopped (`dcam = 0`) and the geometry is *still moving* by -2. It is catching
up. That is not noise; noise does not keep going after the input stops.

## What the player sees

During a pan the sprites sit behind where the ground says they should be, by
roughly the distance the camera moved in the preceding two frames — 2 px on a
slow pan, 8 px at the speed above, and more at higher speeds. When the pan
stops they snap forward into place.

That is "jittery and dishevelled" exactly: the units appear to swim against the
ground, and the faster the pan the further they swim. It also explains why the
09-13 correlation found up to 17 px: that run panned faster.

And it explains the shape of the whole problem — **the 3D is 99.36% rigid**, so
nothing is wrong with how any individual sprite is placed. They all lag
*together*, against a ground that does not.

## Why it happens

`lb_patches.cpp`'s own note beside the dump has carried the mechanism since
09-13 without it being proven:

> the 3D comes from a snapshot of the frame the renderer just finished while
> the 2D is decoded live from VRAM, so during motion they can be a frame apart
> -- which is a unit sliding against the ground under it, and is what "sprites
> become jittery" would be.

The trace turns "can be a frame apart" into "is two frames apart, and here is
the arithmetic". The doubling is the 30 Hz camera: one frame of snapshot
latency lands two compositor frames behind a camera that only steps on
alternate frames.

## The fix, and how it will be checked

The 2D and the 3D must be composited from the **same** camera instant. The
compositor currently draws the 2D from the live camera and the 3D from a
snapshot two frames old. Aligning them removes the lag by construction —
either by holding the 2D back to the snapshot's camera, or by not compositing a
geometry snapshot older than the 2D it is being drawn over.

**The acceptance test already exists and is this document's first statistic.**
After the change, `mode + d(cam)` must collapse to 0 for moving pairs, and the
`RIGID` figure must not fall below the 99.36% it is now. Both come from one
`LB_POLYLIST=1` run and `tools/lb_jitter.py`, so the fix is falsifiable before
it ships.

## The first attempted fix, and why it was withdrawn

`LB_LAGFIX` applied the camera's two-frame travel to the 3D row's sample index
at composite time. It was built, gated off, and measured. **It is withdrawn,
for two reasons found by measuring rather than by reasoning ahead.**

**1. The acceptance test in this document could not grade it.** `lb_jitter.py`
reads the polygon list, which records where geometry sits *in the 3D raster*;
the fix changed where the compositor *samples* from. The A/B was therefore
blind by construction:

    LB_LAGFIX      TRACKING          RIGID     JITTER
    off        91/830 (10.96%)      99.36%     0.64%
    on         92/830 (11.08%)      99.36%     0.64%

The camera object was located in both runs (`camera object at 0224B448`), so
the correction was live; the instrument simply could not see it. An acceptance
criterion has to be written against the same quantity the fix changes, and this
one was not.

**2. The fix was the wrong shape anyway.** Two defects, either of which is
disqualifying:

* **It moves the HUD.** The in-field HUD is 3D geometry in the *same polygon
  list as the units* (lb_hud.cpp). Sliding the whole row shifts the HUD with
  the sprites — and the HUD is screen-locked, so it would gain precisely the
  jitter the units lose.
* **It misfires on camera jumps.** Tracing `lagdx` per frame: **518 of 519
  samples read 0, and the one that did not read 32** — the clamp, not a pan.
  A level load or a minimap click makes the two-frame delta enormous, and the
  correction would smear the entire 3D layer for two frames.

### What a correct fix has to do

* shift **only world geometry**, which means doing it where the z that
  separates world from HUD is available — not on a flat composited row;
* **reset across a camera discontinuity**, so a jump cannot be mistaken for a
  pan;
* and be graded by a test that measures **where the pixels end up**, not where
  the polygons were before compositing.

The diagnosis in this document stands unchanged — frame 1010 still shows the
camera stopped and the geometry still moving. Only the remedy was wrong.

## CONFIRMED: the ground is current, so the slide is real

The obvious objection to everything above is the one that sank `37ad4aa3`: the
geometry was compared against a **camera reading**, and if that reading is not
the one the picture was drawn from, the difference means nothing.

`choose_camera` answers it directly, and had been computing the answer every
frame without printing it. It tests each of the last four camera readings
against the guest's **own BG1 tilemap** and accepts only a candidate with
**zero mismatches across 48 sampled cells**; `g_cam_pick_age` is how far back
it had to reach. That is not a proxy — it is the age, in frames, of the camera
the terrain on screen was actually drawn with.

Measured over 3,374 traced frames:

    campick 0   3007   89.1%
    campick 1    317    9.4%
    campick 2     48    1.4%
    campick 3      2    0.06%

**The ground is drawn from the newest camera reading 89% of the time.** The
geometry lags by two. So the two are not lagging together, and the relative
slide that was reported is real.

### Conditioned on the camera actually moving, it is 100%

An aggregate over all frames mixes in thousands of still ones, where an older
camera trivially also matches because nothing moved — the shape of error this
project has recorded as *sampling selected by the property*. Splitting the same
13,434 frames on whether `cam` changed:

    moving frames   758
      campick 0     758   100.0%

    still frames  12675
      campick 0   12053    95.1%
      campick 1     565     4.5%
      campick 2      51     0.4%
      campick 3       6     0.0%

**In 758 of 758 moving frames the ground is drawn from the newest camera
reading.** Every non-zero age is on a still frame, where it cannot matter.

So during a pan the ground has no lag at all, and the relative slide is
*exactly* the geometry's two frames. There is nothing left for the ground to
explain.

It had been printed only from a band-log line that does not fire in the current
configuration — measured and thrown away, which is the failure this project
keeps rediscovering. It now rides in the `LB_POLYLIST` header as `campick`.

## The correct fix, and the one thing that makes it hard

The shift must apply to **world geometry only**, and the place where world can
be told from HUD is `SubmitVertex` — the vertex carries its z, and docs/212's
threshold already separates the in-field HUD (4,259,328 / 4,267,520) and the
health bars (4,718,080) from world geometry (6,987,264+). The port already
shifts vertices there for the band, so the hook exists.

**The hard part is that it must extrapolate.** At `SubmitVertex` time the
geometry has not been composited yet; it will be, about two frames later. The
correction is therefore the camera's *expected* travel over the next two
frames, and the only estimate available is its travel over the last two — which
is right during a steady pan and overshoots at a direction change.

That is a real risk of trading a lag for an overshoot, so it needs the pixel
test, not this document's polygon test: **record frames, locate a stationary
feature in the 2D ground and a stationary building in the 3D, and measure their
separation across a pan.** Until that test exists, no fix ships.

## The pixel test finally ran, and it does NOT confirm the lag

`tools/lb_groundshift.py` on 120 consecutive gameplay frames (frames
5441..5560, 512x384 so the ground has structure), pairing each recorded frame
to its polygon list through the runner's own `[lb_dumpidx] slot N frame M`:

    frames 120   paired with geometry 107   ambiguous 12

    ground - geometry
      -1 px      6
      +0 px     95
      +1 px      6

    TOGETHER: 107 of 107 pairs within +/-1 px  (100.0%)

**The ground and the geometry agree within one pixel on every pair.**

### Why that does not settle it either

Every listed pair reads `ground +0`. **The camera was static for the whole
window.** The two-frame lag only produces a divergence while the view is
moving, so this run did not exercise the claim at all. The prediction in this
document — 2 px on a slow pan, 8 px at the speed traced above — remains
**untested at the pixel level**, and the polygon-level evidence for it stands
unchanged.

A window containing a real pan is what would confirm or kill it. This one is
not that window.

### What it did find: a 1 px oscillation against still ground

    frame  ground  geometry  diff
     5449      +0        -1     +1
     5451      +0        +1     -1
     5460      +0        -1     +1
     5462      +0        +1     -1
     5484      +0        -1     +1
     5486      +0        +1     -1

The ground does not move and the geometry alternates **-1, +1, -1, +1** on
roughly a two-frame period. Twelve such pairs in 107.

**WITHDRAWN WITHIN THE HOUR, by the obvious follow-up.** I wrote that this was
"a one-pixel shimmer of every sprite". It is not. Running `lb_jitter.py` over
the same window gives the per-polygon spread *within* each frame:

    -1 px    147
    +0 px   1890
    +1 px    157

**304 of 2,214 polygon-shifts deviate from their own frame's modal shift by
exactly one pixel — 13.7%.** So the ±1 is *scattered across polygons*, not
applied to all of them together. A coherent layer shimmer would have put every
polygon on the same value and left this spread at zero.

Scattered ±1 is what sub-pixel rounding looks like, with walking units on top,
and neither needs explaining. The header fields rule out the port's own state
as a cause in any case: on frames 5448..5462 `cam`, `rcam`, `chosen`, `rband`
and `extra` are **byte-identical**, so nothing this port controls is
alternating.

What survives is only that the modal shift is an unstable statistic on a
21-polygon frame: when 14% of polygons sit one pixel either side, the mode can
flip between 0 and ±1 without anything moving. That is a caveat on the
measurement, not a finding about the picture.

**Not yet attributed.** A +/-1 alternation could equally be rounding in the
vertex transform, and this tool cannot tell those apart. What it can say is
that it is in the GEOMETRY and not in the ground, because the ground's measured
shift is exactly 0 across all twelve.

## Why six attempts to see it in pixels failed: THREE frames

The pixel test was run on a window chosen for camera motion — field frames
2683..2802, 77 moving frames out of 120. It refused: 90 of 119 pairs ambiguous,
and **zero** paired with a geometry shift. The reason is not the tool.

    polys per frame, field frames 2683..2802:   123 frames at 0 polys

Every frame in that window draws **no 3D geometry at all**. It is a pre-battle
camera sweep.

Asking the whole run for frames that have *both* camera motion and geometry:

    frames with camera motion AND >= 10 polygons, in 18,733 frames:  3

**Three.** In `demos/user-combat-2026-09-14`, when the camera moves there are
essentially no units on screen, and when units are on screen the camera is
still. That single number explains every failed measurement in this document's
history, including the two static windows and the ambiguous pan.

### What it means for the claim

**The two-frame lag cannot be observed at pixel level on this route**, and no
amount of re-recording will change that. The polygon-level evidence stands on
its own; the pixel-level confirmation needs different input.

It also puts a bound on the lag's *practical* significance that is worth
stating plainly: if a recorded session pans while units are visible only three
times, then either this demo is unrepresentative of real play — the jitter
is reported "in motion", so a player evidently pans while looking at their
units — or the lag is not what the report describes.

### The next input, and why it is different

`--pan` drives the camera with key presses rather than waiting for the replay
to do it. Driven during a battle with units on screen, it produces the one
combination this demo never contains. That is the next recording, and until it
exists the pixel column of this document stays empty rather than being filled
with a number from a window that could not show the effect.
