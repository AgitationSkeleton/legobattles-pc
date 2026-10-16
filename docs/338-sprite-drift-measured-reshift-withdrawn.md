# 338 — The sprite drift, measured; and LB_RESHIFT withdrawn

The defect: while the camera is in motion, sprites become jittery and
dishevelled (reported 2026-09-13 and 2026-09-20).

This is the first document to put a **number in pixels** on that defect, and
the first to grade a proposed fix with a test that can actually see it. The fix
failed. Both halves matter.

## The number

On frames where the camera moved, for geometry that is not walking:

```
  uncorrected  n=249  median 2.0 px  mean 3.25  p90 8.0  max 16
               within 2px 55.0%   (ambiguous, refused: 142)
```

A stationary object's screen position must change by exactly minus the
camera's movement. The residual `dscreen + dcam` is its positional error, and
zero is correct. **Only 55% of matched sprites are within 2 px of correct while
the camera moves, and the worst tenth are 8 px or more out of place.**

### Two corrections to my own first figures

I first published this as *median 4.0 px, p90 13.0, 37% within 2 px*. Both
errors inflated it and both were mine:

1. **I paired the polygon list with the live camera.** `lb_band_render_cam()`
   is documented as *"the camera the geometry now queued for rendering was
   transformed against -- pair this with the render polygon list, never the
   live camera"*, and warns in the same comment that the live one manufactures
   a swing *"shaped exactly like the shear it would be used to detect"*. I did
   exactly that. It costs about 1.5 px of p90; `--livecam` reproduces it.
2. **Repeated art matched to the wrong instance.** Grass and trees share
   (texture, palette, vertex count), so nearest-centre matching happily pairs a
   polygon with a different instance one tile over, and the mismatch enters the
   statistic as drift. Candidates are now required to lie within 24 px of where
   the camera predicts, and ambiguous ones are refused outright -- 142 of them.

**The bias this leaves must be stated:** the gate discards an object that
genuinely drifted more than 24 px rather than counting it, so these figures are
a **floor** on the drift, not a ceiling.

That is the jitter, quantified. It is not sprites vanishing (docs/337,
withdrawn) and it is not the ground (below).

## The ground is not at fault

`choose_camera` picks the camera the terrain is drawn with by validating
candidates against the guest's own BG1 tilemap. It was the obvious suspect:
during a pan the guest's tile streamer rewrites BG1 progressively, so the
tilemap is a mix of two cameras and an *older* reading can score better than
the current one. (The trace appeared to show it reaching back 8, 10, even 40
frames. It does not -- that was the submitted polygon count under the wrong
label; see the correction below.)

Measuring the consequence rather than assuming it, over 6,367 frames with a
valid pixel camera:

| campick age | n | ground error (cam_x − chosen_x) |
|---:|---:|---|
| 0 | 6085 | +0 … +7 px |
| 1 | 346 | +0 … +7 px |
| 2 | 2 | +2 … +2 px |

**Zero frames of 6,367 have the ground more than 32 px out of place**, and the
maximum error at *any* age is 7 px — which is the tile rounding, `chosen` being
reported in tiles×8.

> **This table was published with the wrong column and is corrected above.**
> The first version listed ages of 6, 8, 19 and 40 and reasoned about what a
> "high pick age" means. Those were not ages — they were the **submitted
> polygon count**, read through a tuple off-by-one (`r[8]` is capture group 9,
> not 8). `choose_camera` picks from a history of **four**, so an age above 3
> is impossible, and that impossibility is what exposed it.
>
> The conclusion is unaffected: the ground error is 0–7 px at every age that
> actually occurs, and `choose_camera` is still exonerated. The reasoning about
> high ages was about a quantity that does not exist.

The single 392 px ground jump in the recording matches a 392 px camera jump on
the same frame pair. That is the `--pan-return` leg, not a defect.

**`choose_camera` is exonerated.** The opposite had been written as a
hypothesis earlier the same day; it is withdrawn here rather than left standing.

## LB_RESHIFT — withdrawn

The idea: the guest submits geometry at roughly 30 Hz, so on frames where it
submits nothing the sprites sit where they were when the raster was built while
the ground, decoded live from VRAM, has already scrolled. `lb_reshift_dx()`
returned the camera's travel since the raster was stamped, and the compositor
slid the 3D row by it, skipping HUD rows.

Graded with `tools/lb_spriteworld.py --rsdx` on the same recording:

```
  uncorrected  n=249  median 2.0 px  mean 3.25  p90  8.0  within 2px 55.0%
  reshifted    n=249  median 8.0 px  mean 8.92  p90 20.0  within 2px 28.5%
```

**Worse on every statistic**, and the corrected matcher makes the verdict
sharper rather than softer: the median quadruples and the fraction landing
within 2 px falls by nearly half. It stays off by default and is not deployed. The
premise — that the residual is the camera's travel since the last submission —
predicts the corrected series should collapse toward zero. It does not, so the
premise is wrong, and no amount of tuning the clamp rescues a wrong premise.

Two defects found in it along the way are fixed regardless, because the code
remains in the tree as a default-off experiment and a broken experiment is
worse than none:

1. **The clamp saturated.** At ±24 px it was pegged on 882 of the 923 frames it
   fired — 95.6%. A correction that is constant on 96% of its applications is
   not a correction. The trace says why: the camera moves +16 px/frame and the
   guest declines to resubmit for several frames together. Values beyond 64 px
   are now *abandoned* (return 0) rather than truncated, because a truncated
   correction is wrong by construction while no correction is a known quantity.
2. **The shift read into the next row.** It was `line3d_raw + lb_rs` at the
   call site, so a positive shift read the following row's opening pixels into
   this row's tail, and a negative shift on row 0 read before the buffer. This
   is precisely where clamping was needed that does not break things, and it
   was the thing doing the breaking. `lb_reshift_line()` now
   bounds-checks each sample and writes 0 — alpha 0 in the compositor's ARGB —
   for out-of-range ones, so an uncovered edge strip shows the 2D ground rather
   than a smear of the wrong row.

## The measurement tool, and why the old one could not do this

`tools/lb_groundshift.py` compares ground read from PIXELS against geometry
read from the POLYGON LIST. A correction applied at composite time moves pixels
without touching the polygon list, so that tool is **blind to it by
construction**. `LB_LAGFIX` and now `LB_RESHIFT` were both proposed, built, and
only then found to be ungradable by the tool meant to grade them.

`tools/lb_spriteworld.py` measures the stationary-object invariant instead, in
the same units the player sees, and can grade any correction the runner reports
in its polylist header.

## The recording matters as much as the tool

The standard `--pan` recording is **useless for studying motion** and every
motion figure taken from one should be re-read with that in mind:

* 24 pan steps at the default 0.35 s hold travel ~216 px each; the map runs out
  after about six. The camera then sits jammed at x=1252 for the rest of the
  run.
* Of 7,223 frames, **120 have any camera motion at all — 1.7%**. The recording
  is 98% stationary.

`--pan-steps 12 --pan-hold 0.12 --pan-return` keeps the camera inside the map
and pans both ways: 388 moving frames of 6,439, 6.0%, with steps of ±8 and ±16
in both directions. Every figure in this document comes from that shape of
recording.

## Next

The residual is real, measured, and now gradable. What it is *not* is the
camera's travel since the last submission. The next candidate is that the
vertices are transformed against a different camera than the one the band
synthesis uses. (I first wrote that `cam` and `rcam` "differ by a constant
48 px". **Wrong** -- that was read off a single trace line. Over 6,433 frames
the difference is 0 on 73% and 161 on 12%; see docs/340.) Pairing the list
with `rcam` rather than `cam` accounts for about 1.5 px of the residual, so the
geometry path is reading the camera the band code believes it is -- but a
median 2 px and a p90 of 8 px survive that correction, and remain unexplained.
