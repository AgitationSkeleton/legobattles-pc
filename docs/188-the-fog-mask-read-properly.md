# 188 — The fog mask, read properly

Status: **the fog RENDERING path is fully read.** What fills the mask is still
open. Corrects docs/183, which named this object wrongly.

Reached by reading code rather than searching memory, after four failed RAM
searches for a map-wide visibility array (docs/179).

## `02151F28` is the fog bitmask, not a "view dimension descriptor"

docs/183 called it that because `func_0203339c` sets it to 32x24 and the numbers
matched the viewport. The object is actually the **fog bitmask itself**, and its
fields are now read rather than inferred:

    +0x04  row stride in bytes = (width + 7) / 8
    +0x08  width  in tiles   (32)
    +0x0C  height in tiles   (24)
    +0x10  the bit buffer,    stride * height bytes
    +0x14  a row-pointer table, height * 4 bytes

`func_020eeb1c` is its **SetSize**: it frees the old buffers, stores width and
height, computes the stride, allocates both buffers from the new size, and fills
the row table. docs/183's finding that widening reallocates correctly stands --
only the name was wrong. The size itself comes from `func_0203339c`, seven
instructions holding 32 and 24 as literal immediates.

## The bit test

`func_020eebf4(obj, x, y)` is eight instructions and leaves nothing to guess:

```asm
ldr  r0, [r0, #0x14]      ; the row-pointer table
and  r3, r1, #7           ; bit within the byte
ldr  r2, [r0, r2, lsl #2] ; rows[y]
ldrb r1, [r2, r1, lsr #3] ; rows[y][x >> 3]
tst  r1, r0, lsl r3
```

One bit per tile, row-major, LSB-first within each byte. `func_020eec14` is a
convenience wrapper taking a point object whose `+4`/`+5` are x and y.

## How a set bit becomes fog on screen

`func_0203a4b4` is the only writer of the fog tilemap -- `func_0203d90c` has
exactly one caller -- and its inner loop over 32x24 tiles is:

```asm
mov r0, r6                ; the mask
mov r1, r7                ; tile x
mov r2, sb                ; tile y
bl  func_020eebf4         ; test the bit
cmp r0, #0
beq skip                  ; CLEAR -> leave the tile alone
bl  func_0203d90c         ; address the fog tilemap entry
strh r4, [r0]             ; SET -> write tile 0x64
```

So **a set bit means fogged**, and tile `0x64` (100) is the fog tile. The
addressing helper indexes its own row table at `02152090`, which is the fog BG's
scroll double-buffer -- consistent with the 64x32-tile `FoWMap001.NSCR` and with
docs/167's finding that these layers hold two screens' worth at a time.

`FoWTileset.NCGR` and `FowMap001.NSCR` are loaded by `func_0203d538`, whose other
branch loads `DebugText` -- the same loader serving two unrelated assets, which is
why the string xref pointed here.

## What this does and does not give

It gives the exact structure, addressing and tile convention of the fog layer,
which is what any host-side shroud would have to reproduce.

It does **not** give the map-wide state. This mask is **viewport-sized** -- 32x24
tiles, one screenful -- so it says what is fogged *where the camera is looking*,
not what the player has explored across a 64x63 map. Drawing shroud on the wide
field still needs that, and it is still unlocated.

The remaining lead is what writes bits into this mask each frame. It is not the
adjacent function (`func_020eec14` is the getter's wrapper), and the obvious
candidates among the seven functions referencing `02151F28` did not show it in a
first pass. `Battles::User::FogCircle` (docs/179) is suggestive: if visibility is
stamped per-unit as discs into a viewport mask each frame, then the *explored*
state -- which persists and is what the wide field needs -- is a separate
structure again, and finding the mask's writer may not lead to it.

Recorded so the next attempt starts from the mask's writer rather than from
another memory sweep.
