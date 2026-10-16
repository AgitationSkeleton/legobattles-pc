# 337 — Geometry submission drops while the camera moves

> ## TITLE AND HEADLINE WITHDRAWN, 2026-09-21
>
> This document was published as *"Geometry vanishes while the camera moves"*
> and claimed units blink out during a pan. **Two checks the same day refute
> the claim as written**, and both were available before it was published.
>
> **1. Nothing is being clipped.** Publishing the geometry engine's SUBMITTED
> count beside the rendered one, on a driven pan:
>
>     moving  n=130    submitted==0:  114 (88%)   rendered==0:  98 (75%)   submitted-but-clipped: 0
>     still   n=6527   submitted==0: 2755 (42%)   rendered==0: 914 (14%)   submitted-but-clipped: 2
>
> (Those are the COMPLETE run. I first quoted `still n=3095, 52%, 29%` from a
> log sampled while the run was still going; the still-frame rates fell to 42%
> and 14% as the rest arrived. The moving arm and the zero-clipped result did
> not move. Sampling a running log and publishing the number is the same error
> as grading a partial recording.)
>
> **Zero moving frames have geometry submitted and then clipped away.** So this
> port's band shift and frustum are exonerated, and the whole "docs/268 again"
> line of inquiry below is void. The polygons were never submitted.
>
> **2. And 42% of STILL frames submit nothing either.** That is the number that
> reframes it: the guest builds its 3D display list at roughly 30 Hz, the same
> cadence as the camera (docs/335). A frame with `NumPolygons == 0` is the
> normal off-beat, not a fault — and because the rendered raster persists
> between updates, `rendered == 0` does **not** mean a blank screen.
>
> **3. A frame recorded during motion has everything in it.** The one capture
> gated on camera motion shows grass, trees, a building, two units and the HUD.
> Nothing is missing. I inferred a visible effect from a counter without
> looking at the picture, which is the error this project has written down as
> *read the picture, not the symbol*.
>
> **What survives, as a hypothesis:** submission is measurably rarer while
> panning (88% empty against 42%), so sprites may *update* less often during a
> pan while the ground scrolls smoothly — which would read as stuttering rather
> than vanishing, and would compound the two-frame lag. `n=129` for the moving
> arm is thin, and this has not been confirmed in pixels.
>
> The measurements below are correct. The conclusion drawn from them was not.
>
> **Follow-up, docs/338.** The surviving hypothesis -- that sprites update
> less often during a pan and so slide against the ground -- is now measured
> in pixels: 37% of matched sprites are within 2 px of correct while the
> camera moves, and the worst tenth are 13 px or more out. The correction
> built from it, LB_RESHIFT, made every statistic worse and is withdrawn.
> The 88%/42% submission rates below were also taken from a recording that
> is 98% stationary -- see docs/338 on why the default `--pan` cannot
> measure motion.

The defect: while the camera is in motion, sprites become jittery and
dishevelled (reported 2026-09-13 and 2026-09-20).

Two days went into measuring how far sprites **slide** during a pan. They do not
mainly slide. **They disappear.**

## The measurement

Split every frame pair by whether the camera moved, and count the polygons the
renderer submitted. Four independent runs, two of them with a deliberately
driven pan:

| run | moving frames | zero polygons | still frames | zero polygons |
|---|---:|---:|---:|---:|
| `drivepan` (driven `--pan`) | 129 | **97 — 75%** | 6,518 | 919 — 14% |
| `jitter1` | 881 | **561 — 64%** | 12,407 | 2,094 — 17% |
| `pix5` | 966 | **635 — 66%** | 13,436 | 1,755 — 13% |
| `camage2` | 881 | **561 — 64%** | 12,552 | 2,094 — 17% |

Mean polygon count while moving: **0.9 to 1.0**. While still: **2.4 to 10.5**.

**Between 64% and 75% of frames in which the camera moves draw no 3D geometry
at all**, against 13–17% when it is still. That is a four- to fivefold increase,
reproduced on four runs and on two different ways of producing motion.

## Why this took so long to see

It was in front of me the whole time, as an obstacle rather than a result. Six
attempts to measure the sprite *slide* in pixels failed, and each failure
report said the same thing without my hearing it:

* the motion-dense window had **123 frames at 0 polys**;
* the whole replay had **3 frames** with both camera motion and ≥ 10 polygons;
* `LB_DUMP_ONMOVE`, which gates on exactly that pair of conditions, captured
  **3 frames in a driven pan**.

I read each of those as "the recording is unlucky" and went looking for a
better window. The scarcity *was* the finding: there are almost no frames with
motion and geometry **because motion removes the geometry**.

## What the player sees

Units and buildings blink out while the view is panning and come back when it
stops. That is a far better fit for "jittery and dishevelled" than a two-pixel
slide, and it explains why the effect is reported as happening *in motion*
specifically.

## Relation to the two-frame lag (docs/335)

The lag is real and separately established — the ground is drawn from the
newest camera reading in **758 of 758** moving frames while the geometry lags
by two. But its visible consequence was never confirmed in pixels, and this
finding suggests why: during a pan there is frequently **no geometry on screen
to slide**.

The two are plausibly the same mechanism seen twice. If the vertices are
transformed against one camera while the clip volume is built from another,
then during motion the geometry does not merely land a few pixels off — it
lands **outside the frustum and is culled**. When the camera is still the two
cameras agree and nothing is lost. That would produce exactly this table.

**Stated as a hypothesis, not a result.** What is measured is the disappearance;
the cause is not yet established.

## Prior art that this resembles

docs/268 recorded the same shape at a world edge: *"the render polygon list
EMPTY on 55 of 56 samples"*, caused by `SubmitVertex` widening the frustum
symmetrically about the camera while `band_compute` clamped the band
asymmetrically. The fix moved the shift into `SubmitVertex`, per vertex, before
the clipper, and took empty samples from 55/56 to 0/56.

`53de0a57` (2026-09-18) then changed where that per-vertex value comes from —
from `lb_band_left(RenderWidth)` to a value latched once per frame — to stop a
smear. Whether that reintroduced an edge case during motion is the **first
thing to check**, and it is a check, not an accusation: the commit's own
measurements were about smearing and never counted polygons.

## What to do next, in order

1. **Establish where they go.** `gx_polygons` reports per-object state after the
   viewport transform, which is how docs/268 distinguished "clipped" from "never
   submitted". That separates a culling bug from a submission bug and nothing
   else does.
2. **A/B `53de0a57`'s latch** against the per-vertex read, counting polygons on
   moving frames rather than looking for smear.
3. Only then consider the lag correction, which is a smaller effect and is
   currently unmeasurable for the reason above.

## Why this outranks everything else open

It is reproducible on four runs, it is large (4–5x), it is squarely the
"sprites or models cull wrong" defect, and unlike the fog wall it
is not a design question — geometry that exists and is not drawn is a defect by
any reading.
