# 190 — The left-edge strip is terrain, half-decoded

Status: **characterised, not fixed.** Also retracts an inference in docs/185.

## It is not an OBJ sprite

docs/185 guessed the in-field HUD was "a BG layer; that is an inference from
*they did not move while an OBJ did*, not a layer-toggle measurement", and the
same reasoning had me expecting the left-edge strip to be an OBJ that the HUD
anchor had displaced.

Reading OAM settles it. Every one of engine A's 128 entries is parked at
`x=0, y=192, 8x8, tile=0x0C0` — the game's hide-everything idiom, placing sprites
one line below a 192-line screen — and `DISPCNT` bit 12 reads **0**, so OBJ is
disabled outright on that engine. There are no sprites on the battlefield screen
to displace. **The OBJ explanation is withdrawn.**

### Reading OAM needs the video module

`read_mem` returns 1024 zero bytes for `0x07000000`. OAM belongs to the video
module, not the CPU bus, exactly as VRAM and palette RAM do — the trap already
recorded at the top of `lb_field.cpp` and hit again here.

Those zeros are worse than an error, because they decode as 128 perfectly valid
enabled sprites at the origin. The first pass reported "128 enabled sprites" and
that number was pure fiction. `nds_vram_renderer_oam()` is the correct accessor,
now exposed as an `oam` debug command.

## What the strip actually is

Sampling the leftmost columns of the battlefield screen:

    y=0..7    28A228  000000  28A228  000000      grass, every other column zero
    y=8..11   DBC3AA  E3DBCB  E3DBCB  E3DBCB      sand, solid

`28A228` is the grass colour `lb_field` renders from the tileset, so these are
**terrain pixels**, not garbage. What is wrong is that in the upper rows every
second column is zero.

Alternate pixels dropping to zero is the signature of **4bpp tile data being
consumed as though it were 8bpp**: each byte holds two 4-bit indices, and reading
one index per byte leaves every other pixel unwritten. That the lower rows are
solid says whatever goes wrong is not uniform down the column.

So: a BG fetch producing half-decoded terrain in the leftmost few columns.

## A measurement that measured nothing

The obvious next step was to toggle each layer in `DISPCNT` and see which one
removes the strip. Writing bits 8..12 of `0x04000000` and sampling gave
**310.3 for the baseline and 310.3 for all five toggles** — identical to one
decimal place.

That is not five negative results, it is one broken experiment: the game rewrites
`DISPCNT` every frame, so a write through the debug server is overwritten before
the next capture. A test whose output does not move when its input does has
measured nothing, and reporting those five identical numbers as "no layer is
responsible" would have been worse than reporting nothing.

Identifying the layer needs the toggle applied from inside the port, per frame,
where it survives the game's own writes.

## The standing hypothesis, marked as one

docs/167 established that BG1/BG2 are a **scroll double-buffer**: only one
screen's worth of terrain is materialised in VRAM at a time. If the visible
window begins a few pixels inside that buffer, the columns before it are read
from territory the game has not written for this scroll position.

That would explain terrain-coloured but incorrectly-decoded pixels in a narrow
strip at one edge, and it fits the architecture already established. It is a
hypothesis: nothing here tests it.
