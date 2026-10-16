# 173 — Tracking the camera by terrain, and why it can only half work

Status: **superseded by docs/174 as the camera source.** The camera is read from
the game's own state now; terrain matching is kept only as a fallback. The limit
described here is real and is why that replacement was needed. Recorded because four rounds of "fix the
tracker" went into finding that out.

## The problem

`lb_field_locate()` (docs/172) reports where the camera is, but it scans main RAM
and cannot run per frame. Its answer goes stale the moment the player scrolls, so
a camera box drawn from it alone is frozen where it was found.
`lb_field_track_camera()` was meant to close that: re-match the visible window
against the already-located grid, cheaply, every frame.

## Four rounds, each with the same symptom and a different cause

Each round the tracker reported the same cell forever. The causes were not the
same:

**1. Too expensive to let anything happen.** Scoring every candidate position
over all ~120 visible cells came to roughly 1.5M bus reads per frame. That did
not merely run slowly -- it starved the guest, so the camera never moved, and a
tracker that was correctly reporting "unchanged" looked broken. *A performance
bug and a correctness bug present identically here.*

**2. Sampling, then accepting the previous position.** Cutting to ~30 sampled
cells and taking the old position when it still scored well made it sit on a
stale cell. On a map that is mostly grass, a wrong position still matches most
cells -- grass matches grass everywhere and carries no positional information.
Fixed by scoring only cells whose block is not the modal one, the same reason
`find_grid()` probes with the most varied row.

**3. Accepting on a net score.** `hit - 3*miss >= 4` accepts a position that
matches a lot and contradicts a little. But a wrong position is not merely worse,
it is *inconsistent*: the right one has **no** mismatching distinctive cell.
Fixed by requiring `miss == 0`.

**4. The real limit, which is not a bug.** With all of the above fixed the
tracker still lost the camera, and the reason showed up only when the true
position was computed independently:

    TRUE position (clean match): none anywhere on the map
    score at the tracker's cell: hit = 0, miss = 120

Not a near miss -- a total mismatch, everywhere. **The view had scrolled over
unexplored ground, where the shroud replaces the level's own tiles.** There is
nothing for terrain matching to match. The tracker was reporting failure
correctly and the test was wrong.

Enabling the cartridge's **Reveal Map** (docs/171, `flags[0]`) does not rescue
it: that clears the minimap but the field still draws its shroud layer.

## Where that leaves it

Terrain matching can follow the camera **only over explored ground**, which makes
it unfit as the primary camera source. `draw_battlefield()` therefore keeps the
last known box and flags it stale rather than falling back to a full re-locate,
because `lb_field_locate()` scans main RAM and running it per failed frame would
be ruinous.

The honest fix is to read the camera's position from the game rather than infer
it from pixels. Two attempts have not found it:

* scanning for a 20.12 pair near the expected value gave 49 candidates, all
  noise in one table;
* diffing RAM across a known scroll gave candidates whose deltas matched, none
  of which survived a second scroll.

`docs/163` found a view object at `0x0224B42C` whose `+0x16C`/`+0x170` are a
per-tile bitmask sized to the viewport, and established that the values are
copied from a further object upstream. That upstream object is the next place to
look, and it is a code-reading job rather than a memory-scanning one.

## Consequence for the entity offset

docs/172's `kEntityToMapX/Y = (33, 4)` was measured at one camera position, and
confirming it at a second position needs a reliable camera at that second
position -- which is exactly what is missing. So it stays flagged as calibrated
and unconfirmed, and nothing positional is built on it.

---

# Finding the camera in memory: a negative result

The plan was to stop inferring the camera from pixels and read it from the game.
Memory scanning did not find it, and the reason is worth recording because the
method itself is unsound here.

## What was searched, and how

Ground truth was the terrain-derived camera (the clean-match search above),
sampled at five or six camera positions driven by the d-pad. For every field, the
test was whether `value/scale - camera` stays constant across all samples.

* regions: `mainram`, `dtcm`, `itcm`, `wramshared`, `wram7`
* widths: signed 32-bit AND signed 16-bit -- the 16-bit pass matters because if
  x and y are adjacent `u16`, a 32-bit read combines them and destroys any
  linearity, which would hide the field from a word-only scan
* scales: raw pixels, 20.12, 24.8, and tile units
* tolerance: up to 8 px, because the terrain-derived camera is quantised to whole
  tiles by the phase while the real camera has sub-tile precision in BGnHOFS

**No field survives, in any region, at any width or scale.**

## Why the method cannot work here

The ground truth is not trustworthy. Driving the camera one axis at a time and
reading back the derived position gives, for example, an UP press that moves the
derived **x** by -24 px. A purely vertical scroll cannot do that.

The cause is the map itself: **this map is symmetric** (docs/168 -- forest belts
down both edges, mirrored cliff formations), so a clean match can occur at the
mirrored position as well as the true one. Correlating anything against a
reference that occasionally jumps to the mirror image will find nothing, and
would be just as likely to find something wrong.

So the negative result above is evidence about the *method*, not about the game:
it does not show the camera is absent from RAM.

## One false positive, recorded

A single-delta search did return `0x0216BC50`, which changed by exactly the
120 px the camera had moved. Sampling it across five further scrolls shows values
like 36745040 and 37615736 -- pointers into `0x0230xxxx`, not coordinates. It
matched once by chance. **A single delta match on one sample is not evidence**;
this is the same class of error as the correlation peak in docs/169.

## The lead that is left

`0x02152000` -- the tilemap staging buffer in main RAM, whose contents are DMAed
to the BG map -- has exactly two literal-pool references:

    func_02036550   func_020366d8

Both key off a global at `0x02151F40`. Whatever decides which map cells get
written into that buffer must read the camera, so that is where the camera
lives, and it is a code-reading job. That is the next step, and this time it
should be done before any more scanning.

---

# Resolved by docs/174

The lead at the end of this doc was right, and following it worked. The camera is
two signed 16-bit pixel values in the view object owned by `func_020396E0`, the
scroll callback -- found through `xref 0x04000014` in three steps, without any
scanning.

Everything above stands as the reason terrain matching cannot be the camera
source: it needs explored ground to match against, and a symmetric map defeats
its ground truth. It is now the fallback rather than the mechanism.

