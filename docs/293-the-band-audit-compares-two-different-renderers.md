# 293 — The band audit compares two different renderers, and always did

docs/291 found `tools/lb_bandaudit.py` could not run. docs/292 made it run, drew
a conclusion, and retracted it when the tool's own control turned out to sit at
12-54% on nearly every frame. This is why, and it retires the instrument for
this question rather than leaving it to mislead a fourth time.

## Look at the two images

`lb_map` is `lb_field_render`'s decode of the level grid. `lb_comp` is the
composited frame. The audit crops the first to the camera rect and compares them
pixel by pixel. Cropped to the guest's own 256 columns, same frame, same rect:

* **the reference** is a muted olive field with sandy patches and a blue lake,
  in a washed-out palette;
* **the frame** is saturated in-game green with the player's base and a unit on
  it, and no lake.

Not the same palette, and not even the same content.

## The numbers, over that same 256x192 region

```
lb_field_render reference    mean RGB 127.4, 161.3, 137.7   saturation  81.1   colours 125
what the game drew           mean RGB  57.0, 154.0,  51.0   saturation 105.4   colours  91
```

A mean red of 127 against 57 is not a shading difference or a tile variant. The
reference is a **map-view render** — a schematic of the level for the
Battlefield panel to draw at a small scale — and the composited frame is the
game's own tileset output. They were never the same picture.

## What that explains

* **The control.** `guest` compares the cartridge's own columns against this
  reference. It "should be 100%" only if the reference is pixel-accurate, and it
  is not, so 12-54% is the honest answer and not a sign of misalignment. The
  premise docs/292 filtered on was wrong.
* **The single 100%.** One row of 28 reached it, at 743 detailed pixels. A
  frame that scores 100% against a reference this different is a frame where
  almost nothing was compared, not a frame that lined up.
* **The 12-23% figure** in `gpu2d.cpp`'s comments. Same tool, same construction.
  Its RIGHT and BOTTOM numbers are measuring the gap between two renderers, not
  between the band and the map.

## Status of the standing figure: DISCARDED

docs/291 called the 12-23% figure "undated". docs/292 called it "unsound", then
withdrew that for relying on a broken control. The correct status is stronger
than either: **discarded**. The tool cannot answer the question it was pointed
at, so no reading of it — old or new, favourable or not — is evidence about
whether the bands draw the right ground.

The comment in `gpu2d.cpp` should be read as a historical note about a tool, not
as a measurement of the port.

## What would actually answer the question

A reference that is the game's own renderer, not a second one. Two candidates,
both cheap, neither attempted here:

1. **Compare the band against the guest's own columns, in one frame.** Pan so a
   feature crosses the native boundary and check it is continuous — the same
   technique docs/270 used for the fog edge, which worked precisely because both
   sides came from the same renderer.
2. **Compare the same world position across two window widths.** A feature at
   map (x, y) should be identical whether it lands in the native columns at one
   width or in the band at another. Any difference is the band path's, with no
   second renderer involved.

docs/290's seam measurement already follows the spirit of (1): it reads only
composited frames and uses a camera pan as its control. That result stands
untouched by this — it never went near `lb_field_render`.

## The rule, stated once more

Three documents and three sessions of numbers came out of a comparison nobody
had ever *looked* at. One side-by-side crop, which cost one command, would have
ended it at the start. When an instrument produces a puzzling number, render its
two inputs and look at them before modelling why the number is puzzling.
