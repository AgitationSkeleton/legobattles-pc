# 371 — The cartridge's widget hit test moves with the picture

Status: **fixed, verified live.** Defect items 4 and 15 (2026-09-25):

* Item 4: clicking within the original aspect still hits moved HUD elements'
  original click points.
* Item 15: menu/UI elements are stretched or misaligned from their clickable
  positions at some resolutions.

Item 4 is fixed: a press on a moved button's face presses it, and a press on
the spot it left is a field click. Item 15's clickable half follows from the
same change. Its "stretched" half is presentation and is not touched here.

## How the game hit-tests the HUD

The decomp is from static analysis, re-checked here against the ROM image.

* **One pen handler.** `FUN_020E489C` handles every pen event and tests the
  widgets first (`FUN_020E3EAC` → `FUN_020E14BC`, which walks each screen's
  widget array). Only if no widget consumes the press do the screens' handlers
  run. The battle view's handler calls the field click `FUN_020A8794`.
* **The rectangle test.** It is **`FUN_020EA1A4`** (Thumb):

      r0 = record+0x20 (margins), r1 = widget, r2 = x, r3 = y
      record = [W+4]; group = [record+0x14] (offset s16 +8 / +0xA)
      hit iff x0+gx+m0*8 <= x <= x1+gx+m2*8, likewise y (blt/bgt: signed)
      x0 = s16 [+6], x1 = [+0xA], y0 = [+4], y1 = [+8]

  It is called by the hit test (`FUN_020E5610`) and by the held-button
  re-test (`FUN_020E5C34`).
* **Drawn or not.** The walk skips hidden and handler-less widgets. It never
  asks whether the widget was drawn this frame.

## Why the old spots still hit

The port moves a HUD element's picture (lb_hud). The widget stays at its
layout rectangle. The frontend corrected clicks from outside the game, in two
ways:

* **The unmap.** `lb_hud_unmap_touch` rewrote a click on a moved element's
  face to its native point.
* **The phantom drop.** `lb_hud_touch_phantom` dropped a press on the vacated
  spot.

Both read the polygons of the last composited frame, which made them blind on
the frames the cartridge submits no HUD quads. docs/349 s9c measured those:
while the camera steps, and for about three frames after. A press on the old
spot then reached the invisible widget, which is item 4's defect. When the
phantom test did fire, the press was thrown away. The player had aimed a field
click there, for example a move.

## The fix: correct the test itself, per widget

* **`widget_rect_observer`** (lb_patches.cpp, an override on
  `0x020EA1A4`). It reads the widget's rectangle exactly as the cartridge does
  (record, group offset, margins) and asks `lb_hud_hit_unmap`. There are three
  answers:
  * **the point is on the element's drawn face:** r2/r3 become the native
    point under it, so the cartridge's own test hits;
  * **the element was moved and the point is off its face:** x is set far
    left of every widget, so the test misses. The manager then gives the
    press to the screens' handlers, and so to the field click;
  * **not moved:** unchanged.

  Then a single tail call of the cartridge's test.
* **`lb_hud_hit_unmap`** (lb_hud.cpp).
  * **The element:** the scanned box whose source rect holds the widget's
    centre (the smallest, if several do). The inverse move is the same
    `hud_unmap_box` the frontend unmap uses, now shared, so the two cannot
    disagree about where an element is.
  * **Threads:** the boxes are published by copy under a lock at the end of
    every scan, with the band that turns surface coordinates into guest ones.
    The scan runs where the frame is composited; the test runs inside the
    guest.
  * **Frames with no HUD:** the scan finds no boxes then. For those, the last
    box seen for that widget rectangle (keyed by the rectangle and the band)
    is used.
* **The frontend.** While the correction is live (`lb_hud_hit_live()`:
  Enhanced, not `LB_HUDHIT=0`), it delivers the true coordinate and neither
  rewrites nor drops a HUD click.
* **The held re-test.** `FUN_020E5C34` truncated the point to 8 bits before
  calling the rectangle test. Two `kTouchPatches` words sign-extend it, the
  same fix as the hit test's own pair at `020E4994`/`98`. The stock words are
  checked against the ROM: `020E5C50 061B0612 -> 041B0412` and
  `020E5C58 0E1B0E12 -> 141B1412`.
* **Recordings.** A `.lbdemo` stores guest touches after the frontend's
  mapping, so every older recording holds its HUD clicks already rewritten to
  native points. New recordings say `hudhit=1` in their header. A recording
  without it replays with the correction off, through the old path, so it
  still reproduces the game its player saw.

## Verification

Held Free Play battle at 1473x830 (a 735x414 view; camera near the west
edge, band 132; `LB_HUDHITLOG=1` prints every answer that is not "as
stock"). Clicks went through the frontend (`frontend_input`, window pixels).

* **Deselect, drawn face** (base selected; moved -135,+112 to the visible
  bottom-left):

      [lb_hudhit] widget 628 rect 4..28 x 168..192: point -120,291 -> ON ITS FACE, native 15,179

  The HUD went away: the base was deselected.
* **A brick, drawn face** (moved -135,-127 to the top-left): widget 605 said
  MISS for the point on 606's face, and 606 said ON ITS FACE at native 11,40.
* **Deselect's vacated spot, with the hero selected** (band 132,64):

      [lb_map] win=294,487 ... -> ext=15 native=15,178
      [lb_hudhit] widget 628 rect 4..28 x 168..192: point 15,178 -> moved away, MISS
      [lb_hudhit] widget 604 ... MISS   [lb_hudhit] widget 605 ... MISS   ...
      [lb_touchrect] touch=(15,178) ...

  Every widget missed, and the field click `FUN_020A8794` received the press
  at its true point. The hero walked to it and stayed selected: Deselect
  still showed in the corner. Before, this press was either dropped (the
  phantom test) or, on a frame with no HUD drawn, pressed the invisible
  Deselect.
* **The recording** of that run begins `lbdemo 1 cadence=30 hudhit=1`.

**Exactness** (exp-gate371, the reference replay at 1473x830):
* **Counts:** identical to gate370b at all six checkpoints. That replay
  predates `hudhit=`, so it runs the old path, and the two held-test patch
  words change no instruction count.
* **Layer maps:** 8/8 identical to gate-quiet.
* **Vanilla** (exp-van371): 240/240 identical.

**The frame rate.** gate371 read 51.64 fps against gate370b's 59.50, with the
machine 54-78% busy before any game started. Back-to-back replays under that
load gave 58.02 (off), 59.41 (on) and 52.42 (off). That is noise in both
directions, not a cost of the change.
