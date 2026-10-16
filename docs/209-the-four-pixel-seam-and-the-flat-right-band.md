# 209 — The 4-px seam, the flat right band, and units seen at last

Goal: feature item 1, adaptive screen size fully working in gameplay -- unit
rendering, modelled special unit rendering, fog/shroud. This document covers
three defects found and fixed in one pass, and the capture that finally
settles the unit question docs/208 left open.

Evidence: `screenshots/exp-story3` … `exp-story8`.

## Getting into a story mission at all

The test needs story mode missions in which specials exist, plus the shroud
removal cheat. Both halves now exist in `tools/lb_exp.py`:

* `--route story` walks Single Player → **Story** → **Knights** → **Act 3**.
  The Single Player screen's left button is Story; that was read off
  `exp-cull2/route/03` and `04`, where the highlight sits on the left button
  before the Free Play tap and on the right one after it.
* `LB_CHEATS=0` forces cheat 0, Reveal Map. Confirmed working: the port reads
  the guest's own BG2 window as **768/768 clear tiles**.

**Act 3 on purpose.** Act 1 mission 1 is the campaign's tutorial, and a
tutorial mission holds the camera still until its multi-page box is finished.
That is why `exp-story3` and `exp-story4` reported `camx = 256` for every one of
ten pan steps. Panning was not broken; it was being refused. From Act 3 the
camera moves normally — `camx: 336, 368, 400, … 544`.

## Defect 1 — the 4-px stripe was the game's own, and widening exposed it

A column of wrong tiles ran the full height of the screen at surface x 96..99,
i.e. native x 0..3, the first four pixels of the guest's own viewport.

`LB_3DEDGE=1` prints each layer's decoded colour at the seam. It said:

    bg2 le=0 dw=256 prio=1: 0000 ... 0000 AA6B B28F 0000 0000

opaque at native x 0 and 2, transparent from x 4 — while the port read the
guest's BG2 tilemap window as entirely clear. Pixels that cannot have come from
inside the window must come from outside it.

They do. The renderer samples tilemap column `(bgx + hw_x) >> 3`, and the game
biases `BGxOFS` by `4 - (camx % 8)`. Measured live, from the renderer's own copy
rather than a debug read:

    [lb_3dedge] bgx 0 508 508 508  bgy 0 0 0 0

508 is −4 on BG1, BG2 and BG3; BG0, the 3D layer, is 0. So the first four screen
columns sample the tilemap column immediately **left** of the 32 the game
maintains, which holds whatever was last written there.

(The harness's own read of `BG1HOFS` returned 0 for every pan step of every run.
Those registers are write-only; reading them back says nothing. The renderer's
copy is the one that matters, and it disagreed.)

This artefact is **vanilla**. On a 256-px screen it is four pixels in the extreme
left edge. Widening does not create it — it moves the native viewport's left
edge into the middle of the picture and makes the same four pixels a bright
column down the screen.

Fix: key the port's tile substitution on the tilemap column the hardware is
sampling, not on the screen column. They differ by exactly this bias.

## Defect 2 — fixing BG2 handed the stripe to BG3

With the bias corrected, BG2 went transparent at the seam and **BG3 went
opaque** in the same pixels, at priority 0, so the stripe survived a fix that had
genuinely worked:

    bg2 ... 0000 0000 0000 0000      <- fixed
    bg3 ... 9665 9285 0000 0000      <- new

docs/205 already established that this port's model of BG3 paints an opaque
band, which is why `lb_widemap_extends` declines it. The bias correction had
quietly handed it a new place to do so: with a −4 bias the first four screen
columns key to tile column −1, which BG3 was happy to serve. Gated on the
unbiased screen column, so BG3 is substituted only where it is actually being
widened — which today is nowhere.

## Defect 3 — declining to supply is not the same as drawing nothing

Then the stripe came *back* on BG2, in a different scene. `lb_widemap_fog`
requires `g_ext2_valid`, and with the map revealed there is no fog to
reconstruct, so it declines — and the stale column is drawn anyway.

"I have nothing truthful for this column" has to mean "draw nothing", not "fall
through to whatever is in memory". `lb_widemap_clear_tile` now supplies each
overlay layer's own clear entry whenever the hardware would sample outside the
32 maintained columns. Both values are measured, not chosen: with the map
revealed the guest's BG2 window is `0x0064` in all 768 entries and its BG3
window is `0x001F` in all 768, and both decode fully transparent.

After this, surface columns 96 and 100 disappear from the frame's
worst-discontinuity ranking entirely.

## Defect 4 — the flat green right band

The right widened column showed **flat green** where the left showed correct,
detailed, seamless terrain. Entities still drew over it, which is what made it
read as "the extension works but the ground is missing".

`[lb_seen]` named the cause in one line:

    [lb_seen] row 116: ................|................................|################

Left extension and native columns clear; **right sixteen shrouded**. That record
is the map-space visibility grid docs/206 parked as unreliable, and
`lb_widemap_tile` blanks to tile 0 wherever it says "unexplored" and the fog
tiles are not yet learned — which, with the shroud removed, they never are.

The contradiction is direct: `clear_frames=1493` and `nonzero=0 of 768` say
there was nothing shrouded anywhere in the guest's window for fifteen hundred
consecutive frames. `g_clear_frames` had been demoted to a diagnostic because
near the player's base it sits at 0 — every frame has fog somewhere — so it
could never open a gate. That is an argument against trusting it when it is
**small**, not when it is large. The blank-out branch now stands down after one
full second with zero shrouded tiles on screen.

## What the capture shows

`screenshots/exp-story8/full.png`, the whole 448-wide surface:

* terrain continuous across **both** boundaries, no seam at either;
* **a unit — the grey and red vehicle — standing at surface x ≈ 360**, and a
  blue pickup at ≈ 407. Both are past 351, the last native column. They are in
  the right widened column, drawn correctly, over correct ground.

That is the capture docs/208 owed and could not produce. Units render in the
widened columns.

## Also fixed here: the cull rect's other half

docs/208 widened `func_02038F28`'s gather rect by rewriting the immediate at
`0x02038FD0`. Reading the whole function rather than the one immediate shows
that was **half a fix** — see the correction appended to that document.

## Still open

* **Modelled special units** specifically. A unit is confirmed; that this
  particular one is a "special" is not. Act 3 was chosen partly for its roster
  but no capture yet identifies a modelled special by name.
* **Effects and particles** in the widened columns: still unaudited.
* Fog **edge-tile textures** (docs/203) — untouched here, and note that a
  reveal-map run cannot exercise them at all.
* HUD repositioning and scaling: not started.

## Army-select menu: RETRACTED -- there is no corruption there

**The paragraph that stood here claimed the left widened band was corrupted with
horizontal streak rows. That was wrong, and it was wrong in three ways at once.**

What it actually is: `lb_edge_fill` doing exactly its job. In the raw 448x192
surface the left extension has stdev **1.3** and max luminance **9.6**, and every
pixel in it equals the native band's first pixel on the same row:

    row   leftband[10]   leftband[50]   native[96]
      0   (0, 0, 24)     (0, 0, 24)     (0, 0, 24)
     64   (0, 8, 40)     (0, 8, 40)     (0, 8, 40)
    176   (0, 0, 32)     (0, 0, 32)     (0, 0, 32)

That is a smooth vertical gradient carried outward, which is what the fill is
specified to do.

The three mistakes, all mine:

1. **The 7x contrast boost manufactured the artefact.** The gradient spans about
   five distinct values across 192 rows -- `(0,0,24)`, `(0,0,32)`, `(0,8,32)`,
   `(0,8,40)` -- so multiplying by seven turns its quantisation steps into wide
   horizontal bands. The "streak rows" were the gradient's own staircase.
2. **The crop included the window title bar.** At 1920x1080 the only rows in the
   left band above luminance 30 are y 1..32, which is the chrome.
3. **The geometry was wrong.** The adaptive surface is sized from the window
   aspect, not fixed at 448. At 4:3 (640x480, 1024x768) the width is 256 and
   there is **no extension at all**, so the band being sampled was native
   content. At 16:9 the width is 341 and the extension is 43 columns per side,
   not 96 -- so the x 20..290 sample straddled the native picture.

Measured properly, the left extension on every 16:9 size is clean:

    1280x720   surf=341 ext=43  mean 8.48  sd 1.00  max 9.6
    1600x900   surf=341 ext=43  mean 8.52  sd 0.91  max 9.6
    1920x1080  surf=341 ext=43  mean 8.51  sd 1.05  max 9.6

**Army-select is not corrupted in this build.** The item comes off the open
list, and the lesson is the one already written down as
`read-the-picture-not-the-symbol`: a boosted image is not evidence about what is
drawn, and neither is a band measured with the wrong geometry.

**Negative result.** The obvious suspect was upstream's `wide_bg_slot` rule,
which extends the furthest-back 2D layer whatever it is; in a menu that layer's
tilemap has no data outside the 32 columns the game draws, so extending it would
decode the hardware wrap. Requiring `lb_widemap` to vouch for the columns
instead was tried and produced a **byte-identical** capture -- `bbox` of the
difference is `None`.

`g_adaptive_bg_extend` is false on this screen, so `wide_bg_slot` is already -1
and no 2D layer is extended there at all. The change was reverted rather than
kept: it fixed nothing measurable and would have taken upstream's rule away from
menus that legitimately want it.

`LB_BGMASK` was then used to split 3D from OBJ, and gave the result that finally
exposed the mistake -- the left band is **byte-identical** whichever layer is
masked:

    run              LEFT band (mean, sd, max)   NATIVE band
    baseline         (8.01, 5.51, 78.0)          (64.22, 61.92, 255.0)
    BG0 off (3D)     (8.01, 5.51, 78.0)          (14.79,  3.15,  24.1)
    OBJ off          (8.01, 5.51, 78.0)          (64.22, 61.92, 255.0)

The mask demonstrably works -- turning off BG0 collapses the native band, which
also confirms this menu is drawn almost entirely in 3D, and OBJ contributes
nothing here. A band that does not respond to *any* layer is not being drawn by
a layer. That is what sent the next measurement to the raw surface, where the
answer was plain.
