# 339 — The sub-tile scroll phase matches the cartridge

Status: **measured against the decompiled original**, and the hypothesis it was
built to test is **dead**. The port's terrain is not misplaced within the tile.

## What was suspected, and why it was a good suspect

docs/338 measured the surviving sprite-vs-ground drift at a median of 2 px with
a p90 of 8 px. Two things made a sub-tile scroll error the obvious candidate:

* the ground's own error against the pixel camera runs **0..7 px** — a tile
  modulus, not an arbitrary spread;
* the band synthesis works entirely in **tiles**. Its "phase" means position
  within a 3-tile metatile, not a pixel offset, and it contains no reference to
  a scroll register at all. It relies on the BG renderer to apply the scroll
  bias to the widened columns along with the native ones — an assumption that
  had never been checked while the camera was moving.

## The cartridge's own rule

docs/195 read this out of the guest's code. `func_0202D1B4`, the BGxOFS setter,
writes `-param_2 & mask`; `func_020396E0` calls it with
`param_2 = 4 - (camx mod 8)` horizontally. Inverting gives an invariant the
**original** renderer satisfies by construction:

    camx & 7 == (BG1HOFS + 4) & 7

That is a ground truth from the decomp, not a guess, and it is exactly the sort
of thing a change in this project has to be based on.

## The first measurement was of a constant

Tested against `nds_io_debug_read(9, 0x04000014, 16)`, the invariant "held on
58.7% of 6,822 frames" — a tidy, plausible, **entirely false** number.

**BGxOFS is write-only on the DS.** The read returns 0 on every frame forever,
so that 58.7% is just the fraction of frames where `camx & 7` happened to equal
4. The giveaway was in the data and I walked past it: *all four* BG scroll
registers read exactly 0 on all 6,822 frames.

`gpu2d.cpp` already carried the warning, a few hundred lines from where I was
working — *"those registers are write-only, and reading them back returned 0
while the picture plainly showed a non-zero offset."* The codebase knew. I did
not read it before measuring.

`lb_bg_scroll(engine, bg, vert)` now exports `g_unit[engine].bgx[bg]`, the bias
the renderer actually scrolls with, which is the only copy the invariant can
honestly be tested against.

## The real result

Against the renderer's own copy, over 6,433 frames with a valid pixel camera:

    renderer BG1 scroll x:  0 (3780)  508 (1827)  3 (270)  2 (160)
                            510 (147)  511 (107)  1 (82)  509 (60)

Those are `-4 .. +3` modulo 512 — precisely the range
`-(4 - camx mod 8) & 0x1FF` produces. The shape of the cartridge's formula is
reproduced by the port.

    invariant holds on 6,081 of 6,433 frames — 94.5%

and **every one of the 351 failures is explained by the trace, not the
renderer**:

| where | n | fails | rate |
|---|---:|---:|---:|
| moving frames | 507 | 181 | 35.7% |
| still frames | 5,925 | 170 | 2.9% |

* All **181** moving-frame failures satisfy the invariant against the
  **previous** frame's camera. The trace prints the live pixel camera at dump
  time, which by then may already hold the next frame's value, while the scroll
  bias is the one the finished frame was composited with. docs/195 warns about
  exactly this: *"Sampling `view+0x5E` at composite time cannot promise that,
  because the guest may already have written the next frame's camera."*
* All **170** still-frame failures fall within 3 frames of camera motion
  (median 1, max 2) — the same skew settling.

**Zero frames show a genuine sub-tile misplacement.** The port's terrain sits
where the cartridge's formula says it should.

## What this closes, and what it leaves

Closed: the sub-tile scroll phase is not the source of the drift measured in
docs/338, and the band synthesis's reliance on the BG renderer to scroll the
widened columns is **correct**, not an oversight.

Still open: the median 2 px / p90 8 px residual from docs/338 survives correct
camera pairing and now survives this too. It is not the camera's travel since
the last submission (docs/338), not `choose_camera` (docs/338), and not the
sub-tile scroll phase (here). The remaining candidate is the billboard
transform itself — LEGO Battles draws units and buildings as textured
billboards (docs/195). docs/340 takes that further and finds the ground and
the geometry are drawn against cameras that step together on only 5.1% of
moving frames.
