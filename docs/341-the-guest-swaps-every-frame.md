# 341 — The guest swaps buffers on five frames in eight

> Title corrected: first published as "The guest swaps buffers every frame" (0.905/frame, inflated by a boot-time counter jump). See below.

Status: **measured**. This answers the question docs/340 left open and rules out
the fix docs/340 proposed.

## The question docs/340 left open

docs/340 found the ground and the geometry stepping together on 5.1% of moving
frames, and offered two readings:

* the guest updates at ~30 Hz and the port's error is moving the **ground** at
  60 Hz, in which case re-coupling them reproduces the cartridge;
* the port is **losing** geometry submissions, in which case holding the ground
  back would lock the picture to a defective rate and hide the real bug.

`lb_band_promote()` runs only in the `FlushRequest` branch of `GPU3D::VBlank` —
the branch where the guest hands new geometry to the renderer — so counting it
is counting the guest's SwapBuffers.

## The answer

    swaps per compositor frame:  0 on 37%,  1 on 63%   ->  0.625 per frame

> **CORRECTED IN PLACE.** This line first read *"6,020 over 6,655 frames ->
> 0.905 per frame"* and the heading claimed the guest swaps on nearly every
> frame. The 6,020 included a single jump of **1,858 on frame 1** -- swaps
> accumulated during boot before the trace began. The documentation audit
> caught that the distribution printed on the very next line contradicts it.
> Excluding that one boot jump the rate is **0.625**.

**0.625 is between 30 Hz (0.5) and 60 Hz (1.0), not near 60.** The guest
submits on five frames in eight. So the argument below is weaker than first
written: it still rules out a clean 30 Hz updater, and "hold the ground back to
the geometry's rate" would still quantise the ground to a rate the cartridge's
own scroll does not follow (docs/339's invariant tracks the live camera), but it
does NOT show the guest updating every frame. Why a DS game would swap on 5/8
frames -- a real variable frame cost, or an artefact of this port's scheduling
-- is open, and it bears directly on the jitter.

On the 388 frames where the ground moved, the guest swapped on **87.6%**. The
geometry is being handed over; it is not being dropped.

## A number I published in this session and had to correct

I first measured "8 swaps in 6,655 frames — 0.001 per frame", which would have
meant a frozen 3D layer. **That was a capture-group error**: the trace header
had grown to `... rsdx %d hofs %u vofs %u prom %u` and I read group 12, which
is `vofs`, not group 13. `vofs` is nearly constant, so it produced a beautifully
consistent and completely wrong answer.

What caught it was the arithmetic disagreeing with itself — a second script
counted 4,163 frames where the counter advanced, against the first script's 1.
Two derived numbers that cannot both be true is the cheapest defect detector in
this project and it has now caught three errors in two days.

## choose_camera, confirmed a second time

docs/338 exonerated `choose_camera` by showing the ground is never more than
7 px out of place at any pick age. A stronger test is available and it passes:

    frames where the ground moved:                     387
    ...while the live camera did NOT move:               0  (0.0%)

The terrain never moves on its own. Had `choose_camera` been flapping between
adjacent candidates it would show up here as ground motion with a stationary
camera, and it does not happen once.

## The matching gate is not binding

docs/338 warned that `lb_spriteworld.py`'s 24 px gate discards genuinely large
drifts and so reports a **floor**. Tested by widening it:

    gate  24 px ->  n=230  median 4.0  p90 16.0  within2px 41.3%
    gate  64 px ->  n=230  median 4.0  p90 16.0  within2px 41.3%
    gate 128 px ->  n=230  median 4.0  p90 16.0  within2px 41.3%
    gate 256 px ->  n=230  median 4.0  p90 16.0  within2px 41.3%

Byte-identical. No candidate lies between 24 and 256 px of its prediction, so
in this recording the caveat costs no coverage and **median 4 px / p90 16 px is
the real figure**, not a floor. (Identical output across a changed parameter is
usually the signature of a parameter that is not reaching the code; here the
plumbing was checked first and the gate genuinely is not binding.)

## Stop using the live pixel camera as a reference

`cam - rcam` on moving frames reads median 96 px, p90 640 px, max 957 px. Taken
at face value that would mean sprites drawn a hundred pixels off the map, which
no one has ever reported and which the per-frame residual (median 4 px)
contradicts.

The resolution is that `lb_widemap_cam_px` is **unreliable during motion** —
this is the same accessor that returns 0 when `g_cam_px_valid` is false and that
produced the phantom 161 px and 1,252 px offsets earlier in this session. Its
*differences* over a still stretch are meaningful; its absolute value during a
pan is not.

The trustworthy pair is `chosen` (validated against the guest's own BG1 tilemap)
and the polygon list's screen positions. Everything quantitative from here uses
those.

## Where that leaves the defect

Unchanged and still open: sprites drift against the ground by a median of 4 px
and a p90 of 16 px while the camera moves, with only 41.3% landing within 2 px.
Ruled out so far: the reshift premise (docs/338), `choose_camera` (docs/338 and
here, twice), the sub-tile scroll phase (docs/339), dropped submissions and a
a clean 30 Hz guest (here; the real rate is 5 frames in 8).

What survives: the geometry is stamped with a camera that is not the one the
ground is drawn with on the same frame. The ground's camera updates whenever
the guest scrolls; the geometry's only on the 5 frames in 8 that swap. That points at *when* `lb_band_for_geometry` samples the camera
relative to the widemap's own per-frame refresh — an ordering question inside
one frame, not a rate question between frames.
