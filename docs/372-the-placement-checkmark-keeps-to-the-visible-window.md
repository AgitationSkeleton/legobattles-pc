# 372 — The placement checkmark keeps to the visible window

Status: **the checkmark's jump is fixed, verified live.** Item 5 of the
2026-09-25 defect list: the red/green build placement indicator was
inconsistent, and its checkmark moved unexpectedly.

This covers the checkmark. What looks like the "inconsistent" half, the
footprint drawn over the checkmark out in the band, is below under **Found,
not fixed**.

## The cartridge's rule

The decomp was re-checked here against the ROM image. The checkmark is widget 631 (`*0x021575B4`), a
16x16 button. It is placed every frame of a placement by `func_02036FBC`, the
function the port's `kFpPost` override already sits on:

    02037038  bl  FUN_0203CA10          p = site_tile * (24, 16) - camera
    02037044  add p.x, #4
    0203704C  cmp p.y, #8               y >= 8 ? y -= 8      (above the site)
    0203705C                                   : y += h*16   (below it)
    0203706C  cmp p.x, #0               x <  0 ? x += w*24 - 8  (its right end)
    0203709C  blx [vtable+0x10]         -> 020E5B7C -> 020EA4D8, SetPosition

* **The tile-to-point conversion.** `FUN_0201479C` makes `(tx*24, ty*16)` in
  fx32, and `FUN_0203CA10` subtracts the camera at view+0x5E/+0x60, in s16.
* **What the two tests are.** They keep the checkmark inside the DS's own
  screen: x = 0 is its left edge and y = 8 its top margin. There is no rule
  for the right or bottom edge.

## Why it jumps in the widened view

Native x = 0 and y = 8 are lines through the middle of what the window shows.
Dragging a site across native x = 0 moves the checkmark from the site's
top-left to its right end, a jump of w*24-8 px (40 for a 2x2 building), in
plain view. Crossing native y = 8 flips it from above the site to below.

## The fix (lb_patches.cpp, `checkmark_stash` and `widget_setpos_observer`)

The same rule is measured from the visible window: its left edge L and top
edge T in native coordinates, `x < L` and `y < T + 8`. Here L and T are
`(gw - vis)/2 - band` and `(gh - visr)/2 - band_y`, the same edges the HUD
anchor uses. L = T = 0 gives the ROM's rule.

It cannot be two patched compare immediates. `func_02036FBC` is overridden
(`kFpPost`), and an override's `original()` is the lifted body, which never
re-reads its bytes. So it is split, following the rule in the
lb-lifted-body-never-returns note (arrange what the guest sees before its
own call):

* **`fp_post_override`** keeps its band work. Then, after the camera is
  restored, it computes the cartridge's p with the cartridge's arithmetic and
  the widened thresholds, and stashes it with the checkmark widget.
* **`widget_setpos_observer`** sits on `SetPosition` (`0x020EA4D8`, Thumb).
  Only for that widget, called from `func_02036FBC`'s widget call (lr
  0x020E5BEA), it writes the stashed point into the caller's point struct.
  Then it tail-calls the cartridge's `SetPosition`. Every other call is a pure
  tail call.

Whether the checkmark is shown (active, a valid site, the mode), the widget
dispatch, and the record writes all stay the cartridge's.

Gated on Enhanced and a widened view; `LB_CHECKMARK=0` turns it off alone.
`LB_CHECKLOG=1` prints the stock rule's point beside the widened one whenever
either changes.

## Verification

Held Free Play battle at 1473x830 (a 735x414 view; camera 136,144, band
132,111, so the visible window's top-left is native L -132, T -111). The
steps were:
* order a builder from the base, select it, and pick a 2x2 building, all
  through the frontend and the docs/371 hit test;
* drag the ghost left across native x = 0.

    [lb_check] site 6,15 (2x2) ... stock 12,88 -> widened 12,88
    [lb_check] site 5,15 (2x2) ... stock 28,88 -> widened -12,88
    [lb_check] site 4,15 (2x2) ... stock 4,88  -> widened -36,88
    [lb_check] site 3,15 (2x2) ... stock -20,88 -> widened -60,88

At site 5 the stock rule jumps the checkmark +40 to the site's right end. The
widened one keeps it on the top-left corner.

On open grass (`chkstep.py`), the checkmark widget's own record was read
back:

| site | record x0,y0 | stock rule would give |
|---|---|---|
| 6,17 | 12,120 (shown) | 12,120 |
| 5,17 | **-12,120** (shown) | 28,120 |
| 4,17 .. 2,17 | hidden | the site was red, and the cartridge hides the checkmark on a red site, as vanilla |

The captures show the checkmark on the site's top-left corner on both sides
of native x = 0.

**A recorded play session places a building.** exp-gate372 replays
`lb-session-1790122638` at 1473x830:

    [lb_check] site 16,14 (2x2) cam 16,240 window L -12 T -111: stock 372,16 -> widened 372,-24

That site is in the top band (native y -16):
* **The stock rule** saw its native top margin crossed and flipped the
  checkmark below the site.
* **The widened rule** keeps it above, where the site is, well inside the
  visible window.

Exactness and Vanilla:
* **Counts:** identical to gate370b at all six checkpoints. The same code
  runs; only the point's values differ.
* **Layer maps:** 8/8 identical.
* **Frame rate:** 59.65 fps.
* **Vanilla** (exp-van372): 240/240 identical.

## Found, not fixed: the footprint over the checkmark in the band

In the step-1 capture the checkmark sits out in the left band (native x
-12..4). There, the green footprint's studs show **through** it. In the
native columns (step 0), the checkmark is drawn over the footprint, as the
cartridge layers them: the footprint is BG tiles under the 3D layer.

So in the band the port composites the harvested footprint above the 3D
layer. That fits the "inconsistent" half of item 5. It is the band
footprint harvest of docs/320, and is next.
