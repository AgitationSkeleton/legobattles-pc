# 340 — The ground and the sprites are drawn against different cameras

Status: **measured**. This is the mechanism behind *"while in motion sprites
become jittery and disseheveled"*, and it is much larger than anything measured
before it.

## The number

Over a two-way driven pan, comparing the camera the **terrain** is drawn with
(`chosen`, from `choose_camera`) against the camera the **geometry** was
transformed with (`rcam`, from `lb_band_render_cam`):

    frame pairs where ground or geometry moved:  432
    the two stepped together on:                  22  --  5.1%

    relative slip per frame:  median 16 px   p90 16 px

**They agree on one moving frame in twenty.** The dominant pattern is the
ground stepping while the geometry camera does not move at all:

| d_ground | d_geometry | n |
|---:|---:|---:|
| +16 | 0 | 124 |
| −16 | 0 | 81 |
| −8 | 0 | 75 |
| +8 | 0 | 40 |

On 322 such frame pairs, 311 had nothing submitted and the render list empty.
**That is not a counter-example.** An empty render list means the 3D layer was
not re-rendered, so the compositor re-presents the retained snapshot — the
sprites are on screen and static while the ground moves under them by a mean of
**14.1 px**.

Expressed as the residual a player sees, sprites against ground:

    median 4.0 px   mean 5.40   p90 16.0   within 2px 41.3%

## Why this was invisible until now

I graded `LB_RESHIFT` against `rcam`. That is the **frozen** camera — the one
the geometry pipeline itself used — so any correction that moves sprites toward
the *ground* necessarily scores worse against it. The test was rigged by
construction, in the same family of error as grading a composite-time fix with
a polygon-list tool (docs/338).

Re-graded against the ground camera, `LB_RESHIFT` is **still worse** —
mean 5.40 → 7.59, within-2px 41.3% → 36.1% — so the withdrawal stands. But it
stands for the right reason now, and the number it failed to improve is 4 px
and 16 px, not 2 px and 8 px.

## A correction published here earlier, withdrawn

docs/338 and docs/339 said `cam` and `rcam` *"differ by a constant 48 px, which
is the two camera objects docs/33x describes"*. **That was read off a single
trace line.** Over 6,433 frames the difference is 0 on 73%, 161 on 12%, and
ranges far wider. There is no constant 48. Corrected in both documents.

## What the fix is not

The obvious move — shift the 3D layer by `chosen − rcam` — is **unsafe as
measured**. That quantity sits at −4 px on 58% of frames (the half-tile bias the
cartridge's own `4 - camx mod 8` formula predicts, so that part is correct and
expected), but it exceeds ±64 px on **21.8%** of frames and reaches 161 px.
Applying it directly would slam a 161 px shift onto a fifth of all frames. A
correction has to distinguish "the snapshot is one guest-update stale" from
"the geometry pipeline has been idle for a long stretch", and `chosen − rcam`
alone cannot.

## What the fix probably is, and the decomp argument for it

Hold the **ground** back rather than pushing the **sprites** forward.

On the cartridge, the scroll callback and the geometry submission are part of
the same per-frame update: `func_020396E0` writes the BG offsets (docs/195,
docs/339) in the same pass that builds the display list. The ground and the
sprites therefore advance **together**, at whatever rate the game updates. The
original cannot produce this defect, because it has no way to move one without
the other.

This port breaks that coupling: the band synthesis decodes the terrain live from
VRAM every compositor frame against the current camera, while the 3D layer only
changes when the guest submits. Re-coupling them means drawing the terrain with
the camera the currently-displayed snapshot was built with — which is exactly
`lb_band_render_cam`, already promoted in lockstep with that snapshot by
`lb_band_promote()`.

That is structurally safer than shifting rows: no sample-index sliding, no edge
strip to fill, no HUD exclusion needed. The terrain is synthesised from a camera
either way; this only changes *which* camera.

> **SUPERSEDED by docs/341.** The guest swaps buffers 0.625 times per
> frame (first published as 0.905, inflated by a boot-time counter jump --
> corrected in 341) -- five frames in eight, so not a clean 30 Hz updater --
> and holding the ground back to the geometry's rate would quantise the
> ground to a rate the cartridge's own scroll does not follow. The fix
> proposed below is withdrawn before being built. Dropped submissions are
> also ruled out: on frames where the ground moved, the guest swapped on
> 87.6%.

**Not implemented yet, and deliberately so.** The open question is whether the
guest genuinely updates at ~30 Hz — in which case re-coupling reproduces the
cartridge — or whether this port is *losing* geometry submissions, in which case
holding the ground back would lock the whole picture to a defective rate and
hide the real bug. The scroll-phase invariant holding at 94.5% against the live
camera (docs/339) says the guest's scroll callback runs often; whether its
geometry submission runs as often is the next thing to measure, and it decides
between the two fixes.

## Gate

`tools/lb_spriteworld.py` grades this, but it must be pointed at the **ground**
camera. Against `rcam` it measures whether sprites agree with the pipeline that
placed them, which they trivially do.
