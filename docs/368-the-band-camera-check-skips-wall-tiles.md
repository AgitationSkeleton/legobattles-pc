# 368 — The band's camera check no longer fails on walls

Status: **fixed, verified, deployed** (the camera half of the defect).

## What was wrong

With walls on screen, the terrain in the widened band could be drawn one
metatile row (16 px) out of place. It happened in the tutorial, below the
DS's own 192 rows. A forest stopped in a straight line at the bottom edge of
the native picture, with the rest of it drawn 16 px too high underneath.

It was ground-truthed without any guessing. The D-pad scrolls the camera by a
known amount, which brings the band's ground into the DS's own window, where
the cartridge draws it itself. Comparing the same world pixels, the band
agreed with the cartridge 64-95% per row at camera y 528, and 100% at 512
and 544.

## Why

Each frame, before the band is built, `lb_widemap` checks that it has the
right camera. It samples 48 native cells (tile rows 1, 5, 9 ...) and compares
its own decode of the map with the guest's BG1. The check demands 0
mismatches, and so does its fallback, `choose_camera`. When both fail, the
frame is held and the band keeps its previous table.

Walls are BG1 tiles too, but the cartridge stamps them over the map's
metatiles (docs/367), and the port's decode does not. So whenever a wall sat
on a sampled row, every frame failed, and the band froze on the table from
the last camera it had accepted. At y 528 the walls were on sampled row 5.
The last accepted camera was 544, exactly 16 px away. At 512 and 544 the
walls fell between sampled rows.

## The fix

`sample_mismatch()` skips any sampled cell whose guest tile is in the wall
block 0x3C8-0x3FF. docs/367 found that block in all three tilesets and rated
it certain. The remaining cells still decide. A wrong camera, one pan step
off, disagrees on nearly all of them, so the check keeps its purpose.

## Verified

- **The tutorial at 1720x720, walls on sampled row 5 (camera y 528).** The
  bottom band matches the cartridge's own drawing 100% in every row above the
  HUD box.
- **The audit.** `LB_SCANLOG=1`'s `[lb_terrain]` audit showed 44-46/768
  cells differing with the walls on screen. Every one was the guest's
  0x3C8/0x3C9 against terrain in the decode, which is exactly what is now
  skipped.
- **Exactness gate: identical.** Six of six instruction-count pairs, 8 of 8
  layer files against gate-quiet, 59.83 fps.
- **Vanilla: 240 of 240 identical.**
- **Also in this build, off by default:** the text box five-slice stretch
  (`LB_HUDSTRETCH=1`), which is still being tuned.

## Still open

Walls and bridges themselves are still not drawn in the band. They show only
once they are inside the DS's own window. The replica that fixes that is
specified in docs/367.
