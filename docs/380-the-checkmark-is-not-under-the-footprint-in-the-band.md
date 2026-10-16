# 380 — The checkmark is not under the footprint in the band

Status: **fixed.** The "inconsistent" half of the placement-checkmark defect,
found but not fixed in docs/372: in the band the footprint shows through the
checkmark.

## What the cartridge does

* **The layers,** read live with the debug server's `read_io` during a
  placement. DISPCNT is 00012F08. The priorities
  are BG3 0, BG2 1, BG0 (3D) 2 and BG1 3. The placement footprint is BG3 and
  the shroud is BG2, so **both are in front of the 3D layer** the HUD is
  drawn in.
* **Why the checkmark still shows.** The cartridge clears their cells under
  every UI element (`SpriteUI_MarkCells`, docs/348), on a grid clamped to
  its own 32x24 cells. In the native columns the footprint's cells under the
  checkmark are simply missing, and the checkmark shows whole
  (`exp-fp5b/win/chk-native-vs-band.png`, left).
* **Why it didn't in the band.** docs/372 keeps the checkmark beside the
  site when the site is in the band. There the checkmark is outside the
  grid, nothing clears the cells under it, and the footprint's studs draw
  over its lower half (the same picture, right).

## The fix

The port already keeps the shroud and the footprint off the HUD pixels it
**moves**: `lb_hud_cover_row`, whose mask the compositor applies to BG2 and
BG3. Now it also covers **every UI pixel the cartridge drew outside its own
256x192**, which is exactly where the cartridge's cell mask cannot reach.
The pixels come from docs/375's HUD-only row. The raster's band is kept by
`lb_hud_scan`.

That was not enough by itself.

* **The checkmark is not in the HUD set.** An `LB_POLYLIST` during a band
  placement shows it as a 16x15 quad at guest x -164, **depth 4,275,712
  (z 502)**. That is the UI engine's layer 0, the command icons' layer, and
  docs/375's split had left it in the world set.
* **So the split takes layer 0 too** (`lb_hud_ui_threshold`, apply_hooks
  stanza 0zzzzzzzz).
* **The merge flags those pixels** with bit 31 of the HUD colour, above the
  5-bit alpha.
* **`lb_hud` handles flagged pixels separately.** It moves and vacates only
  unflagged ones (command icons are world-anchored and never move), and it
  covers all of them in the band.
* **The merged picture is unchanged.** Layer 0 is nearer than every world
  sprite (z 448 at most), so HUD-over-world merging gives the same result
  as the single pass.

## Verification

* **The repro:** a builder's placement is panned
  nine steps into the left band. With the fix the checkmark is whole over
  the footprint, as in the native columns (`exp-chk380b/win/band-ab.png`:
  before left, after right).
* **docs/375 still holds.** Its truncation repro (now moving the Top Screen
  panel aside first) draws the base whole, with a continuous
  health bar, under the brick column's native rect (exp-t380b).
* **Gate** (exp-gate380): counts are identical to gate370b, and the eight
  pictures under 5972 are identical to gate377. The merge with layer 0 in
  the split changed no pixel of the gate replay. 59.84 fps.
* **Vanilla** (exp-van380): 240/240 identical.
