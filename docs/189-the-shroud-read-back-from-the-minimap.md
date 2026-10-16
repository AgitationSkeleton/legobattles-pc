# 189 — The shroud, read back from the minimap

Status: **working.** The wide field no longer draws the whole battlefield
revealed. Closes the fidelity violation docs/179 opened.

## Five approaches had failed

Four memory searches (docs/179: cheat diff, teleport diff, zero-to-nonzero
filter, and a shape search at three encodings) and one code read (docs/188: the
fog rendering path) all failed to produce a map-wide visibility array. docs/188
established why the code read could not: the bitmask the fog renderer consults is
**viewport-sized**, 32x24 tiles, so it knows what is fogged where the camera
looks and nothing about the rest of the map.

## The game draws what it knows

It does not need to be found in memory, because the cartridge **renders** it
every frame, on the minimap. Reading that back is the same move that made the
unit stencil exact (docs/184): take the game's own output rather than
reconstructing what it must have meant.

Sampled colours inside the minimap panel, early in a match:

    (40, 32, 48)   7322 px   unexplored
    (40,154, 40)    688 px   explored grass
    (40,121, 32)    129 px   explored terrain
    (130,162,113)    44 px   explored olive
    (251,251,251)    64 px   the camera box outline
    (211,138, 56)     7 px   a unit blip

One distinctive colour for unexplored and everything else explored or UI, so the
classifier is an equality test rather than a threshold — no tuning, no drift.

The map-to-minimap transform is docs/180's, measured against the cartridge's own
camera indicator, so no new assumption is introduced: each metatile cell's centre
maps to a minimap pixel, and the pixel's colour classifies the cell.

## Drawn last, and only outside the camera

The shroud is applied after terrain and reconstructed units, because a unit
standing on unexplored ground must not show through the fog hiding the ground it
stands on. It is skipped inside the camera rectangle, where the guest has already
drawn its own fog and a second pass would double-darken.

## Honest limits

* **Coarse by construction.** 92 minimap pixels span 1536 map pixels — about 1.4
  minimap pixels per metatile cell. That decides a cell but cannot place an edge
  precisely, so the shroud is blocky at cell granularity. The game's own fog is
  per-tile too, so this is the right order of resolution, but it is not the same
  edge the DS draws.
* **It depends on the minimap being on screen.** A menu or transition would
  classify every cell the same way, so a frame where every cell reads explored or
  every cell reads hidden is rejected and the previous state kept. Nothing is
  hidden when the classification is unavailable — failing open, so a bad frame
  cannot blank the map.
* **The panel bounds are this title's**, measured at 92x92; another UI layout
  would need re-measuring.

Verified: `screenshots/20260831-204831-shroud/shroud_full.png` shows explored
ground around the player's base and shroud across the rest of the battlefield,
with the camera box inside the explored region.
