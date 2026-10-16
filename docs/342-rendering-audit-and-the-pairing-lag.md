# 342 — Rendering audit, 2026-09-22: what works, what doesn't, and the pairing lag

Written for: whoever picks this up next.

The audit behind this read the git history diff by diff, checked every
rendering doc against the current code, audited the render path, and
reconstructed the original game's renderer from the decompilation. This
document is the synthesis plus the measurements made alongside them.

## 1. Where the rendering actually stands

**Works, verified by picture:**

* Vanilla (`--adaptive-widescreen none`) looks like a DS at every window size
  tried: 256x192, the minimap and resource bar on the top screen.
* Adaptive at 4:3, 16:9 (1024x768, 1280x720, 1920x1080) and portrait
  (768x1024): the widened columns show real game content -- the player's base,
  units, forest -- with jagged, correct shroud edges. 1920x1080 is clean.

**Broken, reproduced during the audit:**

* **Ultra-wide (2560x1080, 1462-px surface): striped shroud and a revealed
  rectangle.** Thin rows of the terrain underneath show through the fog hatch
  every ~16 px across most of the unexplored area, and a ~256-px-wide
  hard-edged rectangle of terrain appears mid-shroud. That is the reported
  broken shroud and square fog. Suspected mechanism, **unverified**: this
  level's map is ~1,540 px wide, the surface covers ~95% of it, the clamp code
  centres the camera when the view is wider than the room to pan, and the fog
  layer's widened columns read past what the game maintains -- the fog, terrain
  and HUD tilemaps are packed end to end (docs/199), so reading past BG2's
  columns reads BG1 terrain entries as fog.
* **Sprites jitter against the ground while panning** -- measured in pixels
  for the first time, section 3.

**Open, from the audits, not yet re-verified by picture:** the command cursor
drawn one band width too far left; Eagle Command base flicker; unconfirmed fog
tiles `0C8..0D0` shipping on by default; the vertical edge-clipping fix never
applied north/south; `apply_camera_clamp` running every frame by default
although its own comment and call site both say it is off.

## 2. Frame rate: the emulator, not the widening

Twelve runs, six window sizes, each in both modes:

| window | adaptive fps | vanilla fps | adaptive surface |
|---|---:|---:|---|
| 512x384 | 59.8 | 58.4 | 292x218 |
| 768x1024 | 56.2 | 59.7 | 438x584 |
| 1024x768 | 42.7 | 42.4 | 584x438 |
| 1280x720 | 34.4 | 47.4 | 730x410 |
| 1920x1080 | 34.6 | 51.7 | 1096x616 |
| 2560x1080 | 42.2 | 55.7 | 1462x616 |

**Vanilla draws an identical 256x192 picture in every one of those runs, yet
its emulator time ranges 13.8-22.4 ms a frame and its frame rate 42-60.** That
is the known interpreter-fallback lottery plus background load, and it is
larger than the entire cost of widening (1.3-4.6 ms of composite work). So:

* **one run per configuration cannot rank configurations** -- the 1462-px
  surface "beating" the 1096-px one is noise until repeated;
* **the obstacle to a steady 60 is the emulator's own cost and its
  instability**, not the renderer. Widening costs frames only once the surface
  is wide, because above 448 px wide (or any width not a multiple of 64) the 3D
  falls back to the software rasteriser -- and raising that cap segfaults
  (`88d62387`).

## 3. The jitter, measured in pixels

`tools/lb_layershift.py` reads nothing but composited pixels. With
`LB_DUMP3D=1` the runner writes the 3D layer the compositor actually used
beside each presented frame (magenta where transparent), so a sprite's
displacement is read from its own pixels and the ground's from the picture with
every 3D pixel masked out. `LB_DUMP_ONMOVE=3` records contiguous frames from the
first camera motion; this matters because the camera steps 16, 0, 16, 0, and a
one-frame lag is visible only between ADJACENT frames.

It replaces `lb_spriteworld.py` and `lb_groundshift.py` as the gate for
anything done at composite time. Both of those read sprite positions from the
polygon list, which a composite-time change never touches -- so they were
blind to `LB_RESHIFT` by construction and its grading was void.

On a two-way pan at 512x384:

```
frame  ground  sprite   drift
 250     -16      0      +16
 251       0    -16      -16
 252     -16    -16        0
 253     -16      0      +16
 254       0    -16      -16
```

**The sprites replay the ground's motion one frame late.** 11 of 22 active
frame pairs drift by a full 16 px, so a stationary building shudders two tiles
back and forth against the ground it stands on. Reproduced on three separate
recordings.

## 4. What the lag is not

* **Not a stale 3D snapshot.** The code audit located the lag in the
  compositor pairing post-VBlank 2D with a snapshot of the previous raster, and
  I built `LB_LIVE3D` to composite the live raster instead. It changed nothing:
  the same one-frame relation, at a different phase. A probe then showed the
  live raster **equals the snapshot on 7,224 of 7,343 frames (98.4%)**, and the
  composite always runs at scanline 0. The snapshot was never stale. The
  audit's diagnosis was wrong, and so was my acceptance of it.
* Not `choose_camera` (docs/338, 341), not the sub-tile scroll phase (docs/339),
  not dropped submissions (docs/341).

## 5. What it probably is: a swap and its scroll commit landing in different frames

From the decompilation (`decomp-renderer.md`): each game update is one task
list, run after a VBlank wait. Task `0x01` (`020396e0`) writes the BG scroll
and DMAs the tilemaps **using the camera committed in the previous run**; later
tasks commit the new camera (`0203942c`), build the 3D list from it
(`02039148`) and swap (`02024d08`). The scroll commit waits one run **on
purpose**, so that it pairs with the previous run's swap. The loop also waits
until 20 ms have passed, so the game updates on about two VBlanks in three --
which is exactly the 0.625 swaps per frame measured in docs/341. The port's
cadence is the cartridge's own.

Scanline-stamped events (`LB_LIVE3D_PROBE=1`, `[lb_evt]`) over a pan:

* every geometry latch happens at scanline **192**;
* the guest's SwapBuffers executes during active display on 4,526 of 6,514
  swaps, but **during VBlank on 1,988 (~30%)** -- those latch one VBlank later
  than the run that issued them;
* BG1HOFS is written at scanline ~197 on two frames in three.

The frames on which a latch arrives without its matching scroll commit, or a
scroll commit without its latch, are where ground and sprites carry different
camera steps. That is **inferred** from the ordering; per-event attribution to
specific runs is fiddly and has not been nailed down, and whether the same
mismatch occurs on real hardware is unknown -- it would, if the swap-to-VBlank
timing matches.

## 6. The fix this points to

Present each 3D frame only with **its own** scroll commit: when a latch has
not yet been followed by the BG write that carries the same camera, keep
presenting the previous raster (and its band) for that frame. This enforces the
pairing the cartridge was designed around, at presentation time, without
touching game logic -- an enhancement for the adaptive mode, with vanilla
untouched. It is designed, not built; `tools/lb_layershift.py` is the gate, and
the target is drift 0 on every active pair.

## 7. What the history says about how to work

The git timeline's verdict: the rendering history **cycles** rather than
converges. Most reported breakages came from changes shipped on by
default after passing only the harness at one window size (1024x384) on one
level, and their fixes caused the next breakage. Three more lessons from this
audit alone:

* a comment is not the code: `apply_camera_clamp` is documented as off in two
  places and runs every frame;
* a hand-derived timing argument was wrong twice today (the stale snapshot,
  then the sign of the lag) and was caught only by measuring;
* a composite-time change needs a composite-level gate.

So nothing from this document ships until it passes `lb_layershift.py` at
several window sizes and is looked at on real frames.

## 8. Everything added by this audit is default OFF

`LB_LIVE3D` (refuted, kept for the record), `LB_DUMP3D`, `LB_LIVE3D_PROBE`,
`LB_DUMP_ONMOVE=3`. Nothing user-visible changed; the deployed build is still
the 17:32 build from 2026-09-21.

## 9. Addendum, same day: the pairing fix failed, and the lag is not a constant offset

**LB_PAIR** (section 6) was built and graded. It holds the previous 3D raster
and band for one composite after a latch that has not yet been followed by a
BG1HOFS write, capped at one frame and gated to the field. The event trace
shows it firing exactly as designed -- HOLD on every frame that latched
without a scroll commit (239, 242, 245, ...). **The drift did not improve**:
exactly-0 fell from 22.7% to 0%, median 16 px. It stays default OFF.

The model behind it predicted the per-frame order *sprite-only then
ground-only*; every recording shows *ground-only then sprite-only*. So
`tools/lb_layershift.py --lagscan` now measures the offset instead of arguing
it: it slides the sprite-shift series against the ground-shift series by k
frames and reports which k zeroes the drift.

| recording | best k | exactly 0 at best k |
|---|---|---|
| l3-snap (control) | -1 (and +2) | 53% |
| l3-live | +1 (and -2) | 50% |
| l3-both | none clear | -- |
| l3-pair | none; median 16 px at every k | 0% |

**No constant offset fixes it.** The best k flips sign between two control-like
runs, and offsets three frames apart score alike -- the signature of the
game's two-updates-in-three cycle (docs/341, and the 20 ms pacing rule from the
decomp). The mismatch is phase-locked to the update cycle, and its phase
varies from run to run. That is why neither holding the 3D nor delaying the 2D
by a fixed frame can work, and why a hold keyed on the wrong event made every
offset worse. Samples are small (9-22 scored pairs per run); this is weak
evidence for any single mechanism.

**Next:** long recordings with events and pixels together, drift tabulated by
the event pattern of each frame (latch with/without a scroll commit, scroll
commit without latch, neither), so the event combination that misaligns the
layers -- and the direction -- is learned from data. Only a fix keyed to that
is worth building.
