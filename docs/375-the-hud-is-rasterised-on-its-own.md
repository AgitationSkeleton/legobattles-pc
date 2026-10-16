# 375 — The HUD is rasterised on its own

Status: **fixed.** Defect item 9 (2026-09-25): sprites partially truncate
(Eagle Command).

## What cut the sprite

The in-field HUD is 3D geometry in the same polygon list as the units
(`lb_hud.cpp`'s header; docs/212). `lb_hud` moves each element to the
window's corner by moving **pixels** of the finished 3D line, and it had to
clear the element's old place first. It did that by zeroing every opaque
pixel inside the element's native rectangle:

    if ((v >> 24) & 0x1Fu) g_scratch[(size_t)x] = 0u;   // opaque

One flat line cannot say who drew a pixel, so any world sprite standing in
that rectangle lost those pixels too. A reading of the code found this;
this doc measured it.

* **Repro.** A held Free Play battle at 1473x830 (surface 736x414, band
  240/111) with an invincible hero (`LB_CHEATS=15,2`). Select the base, pan
  so it sits under the moved brick column's native rectangle (native x 4..20,
  y 16..64).
* **Result.** The Eagle Command and its health bar lose exactly that
  rectangle: the ground shows through the top-left of the building and the
  bar has a gap (`screenshots/exp-trunc/win/a3-base-zoom.png`, and the left
  half of `exp-t375on/win/ab-375.png`).
* **Two more faults in the same place:**
  * A world pixel showing through a transparent part of an element was copied
    to the corner along with it.
  * The pixels an opaque element covered on the DS were never rasterised at
    all.

Because of the second fault, a depth-aware vacate is not enough. Even
knowing which pixels were the HUD's, nothing would be left to show where
they were.

## The fix

The soft renderer draws the HUD's polygons into a set of buffers of their
own (apply_hooks.py stanza 0zzzzzz, `GPU3D_Soft`):

* **Which polygons.** `lb_hud`'s own test: minimum `FinalZ` under
  `hud_z_threshold()` (4271616, z 503 and up). Shadow polygons are never
  included.
* **The world pass.** `RenderScanline` skips those polygons in its normal
  pass. After it, on the rows they reach, it draws them into
  `lb_HudColor/lb_HudDepth/lb_HudAttr`. The set is cleared only in the
  columns the HUD reaches: the union of the HUD polygons' x ranges.
* **Finishing a row.** `lb_FinishRow` runs both final passes, keeps the
  world-only row (`lb_WorldOut`), then blends the HUD row back over the world
  with the renderer's own `AlphaBlend`. `GetLine` therefore returns the same
  picture as before to every consumer.
* **Colour stores.** The HUD colour and world-only stores are per colour
  store, alongside docs/359's `ColorStore`, so a row the compositor holds
  stays paired with its halves.
* **`lb_SplitRows(line)`** tells `lb_hud_shift_line` about the row it was
  handed:
  * **1:** here are its world-only and HUD-only halves.
  * **2:** the frame was split, but no HUD reached this row.
  * **0:** this is not a split row.
* **`lb_hud_shift_line` on a split row:**
  * **Vacate.** A moved element's native rectangle is vacated with the
    **world** pixel, and only where a HUD pixel was.
  * **Capture and move.** Only HUD pixels are captured and moved.
  * **Destination.** Each moved pixel is blended over what is already there,
    with the same formula.
  * **Unsplit rows** keep the old path, unchanged.

**When it applies.** Only on widened frames (`RenderWidth > 256`), and only
with edge marking off, because edge marking tests neighbours and would see
the empty HUD set. Battle frames have it off: DISP3DCNT is 0019. Enhanced and
HUD anchoring must both be on. `LB_HUDSPLIT=0` turns the split off.

> **Superseded (docs/388):** battle frames have edge marking ON from the first
> transport or siege unit on screen until the game restarts (0029). Such
> frames now split too, and `lb_EdgeMerge` marks edges on the merged picture.
> `LB_EDGECHECK=1` measured 0 px different from the unsplit render.

**Rows that take the old path.** A row the compositor holds as a copy is not
in the colour store, so it takes the old path. That happens with the opt-in
`LB_PAIR` or `LB_SNAPCOPY`, or with a scrolled 3D layer. The default path
keeps pointers (docs/359).

**Cost.**

* **Memory:** about 53 MB more in the soft renderer, for the HUD's colour
  set, depth set, attribute set and the world-only rows.
* **Time:** the extra work is confined to HUD rows and HUD columns. HUD
  polygons move from one pass to the other; they are not drawn twice.

## Verification

* **A/B on the repro**, same route and inputs (a one-off script, `truncab.py`):
  * `exp-t375off` (`LB_HUDSPLIT=0`) cuts the base and its bar as before.
  * `exp-t375on` draws both whole under the same native rectangle, and the
    moved bricks and cancel button still draw at their corners.
  * The log confirms the path: `[lb_hudsplit] first moved-HUD row with
    split 1 (world and HUD rows from the renderer): row 128, HUD columns
    243..270`, which is native x 3..30.
* **Counts:** exp-gate375 is identical to gate370b at all six checkpoints.
  This is a host-renderer change.
* **Pictures:** all eight layer maps and composites below frame 5972 are
  byte-identical to gate374. No world sprite stood under a moved element at
  those frames, and the merge reproduces the unsplit picture exactly: every
  HUD quad lands on whole pixels, so no AA edge blends differently.
* **Vanilla** (exp-van375): 240/240 identical, and no split was logged. A
  256-wide frame never splits.
* **Cost**, timed on the render thread with split and unsplit frames in turn
  in one run (`LB_HUDSPLIT=ab LB_3DWAIT=1`, exp-t375ab, 736x414):
  * Split renders take about 0.2 to 0.4 ms longer. Examples: 1.220 against
    1.462 ms, 1.192 against 1.414, 1.076 against 1.292. That is out of a 1 to
    3 ms render, which runs beside the emulation thread.
  * The emulation thread's waits for rows do not follow the split. They read
    0.05 to 0.37 ms per frame in both phases alike.
  * The replay ran at 59.82 fps.

## Also found on the way (items 6 and 15)

The window-size sweep that preceded this found no HUD or menu defect in a
steady state:

* **Nine sizes** (4:3, 16:9, 21:9, 1:1), with the building list open in the
  reference replay (`exp-hudsz4/sizes`):
  * the same element set, moved to the corners of each surface;
  * captures just after a resize matched the settled ones, apart from the
    animated shroud.
* **Menus:** clicking the drawn Stats and Main Menu bricks and the Conquests
  icon reached them at 900x900 and at 1720x720.
* **The tall green stalk** seen at wide sizes beside the player's base is a
  3D sprite: most likely a Forest Spawn or Jungle Growth effect (LOC strings
  263 and 272), clean pixel art rather than a stretched quad. Because such
  trees are depth-sorted 3D sprites and not BG1 tiles, they are exactly what
  item 7's depth seams can put a unit on top of. That is the tier-1 depth
  fix, next.

Items 6 and 15 need the failing case itself (a capture, or how the window was
resized) before anything more can be said about them.
