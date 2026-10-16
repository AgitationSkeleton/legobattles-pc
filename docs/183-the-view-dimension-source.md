# 183 — The view-dimension source, found; and it is not the FOV lever

Status: **located, patched, verified live — and it does not widen the rendered
field.** Two of the three results here are positive; the third is negative and is
the one that matters most for the widened view.

Prompted by docs/182's reading of the C&C Remastered sources, whose
`Set_View_Dimensions` owns the tactical view size and rebuilds every derived
structure from it. This cartridge has the same shape.

**NAMING CORRECTED -- see docs/188.** This document calls 02151F28 a
"view-dimension descriptor". It is the FOG BITMASK object: +0x04 stride,
+0x08 width, +0x0C height, +0x10 the bit buffer, +0x14 a row-pointer table.
Everything measured below about the reallocation stands -- only the name was
wrong, and the wrong name is what made "it changes nothing on screen" a
surprise rather than the expected result of resizing a fog mask.

## The source, which docs/163 said was "further up the chain"

`func_0203339c` is seven instructions and holds the constants as immediates:

```asm
0203339C  ldr ip, =020EEB1C      the dimension setter
020333A0  ldr r0, =02151F28      the view-dimension descriptor
020333A4  mov r1, #0x20          width  in tiles   <- the constant
020333A8  mov r2, #0x18          height in tiles
020333AC  bx  ip                 tail-call
```

Found by disassembling the caller chain from `0203A5BC` rather than by search:
`func_0203a4b4` loads its width and height from a **global** at `02151F28`, and
that global has seven referencing functions, one of which is this thunk.

Read live in a running level, the descriptor is exactly what the disassembly
predicts:

    +0x00  0214FA94   (descriptor/vtable)
    +0x04  4          row stride in bytes = (32 + 7) / 8
    +0x08  32         width  in tiles
    +0x0C  24         height in tiles
    +0x10  022451E8   the mask buffer,      stride * height = 96 bytes
    +0x14  02245258   the row-pointer table, height * 4 = 96 bytes

## docs/164's buffer overrun does not exist — RETRACTED

docs/164 read `func_020eeb1c` as a stride assignment and concluded:

> the stride was raised from 4 to 7 bytes here, which changes the buffer's
> required size from 96 to 168 bytes; nothing was allocated for that, and that
> is a latent overrun to resolve before this ships.

Eleven further instructions disprove it. The setter **frees and reallocates**:

```asm
020EEB2C  bl  020EEBB8          free the old buffers
020EEB30  str r6, [r4, #8]      width
020EEB38  lsr r1, r0, #3        stride = (width + 7) / 8
020EEB40  str r1, [r4, #4]
020EEB4C  mul r0, r1, r0        size = stride * height
020EEB54  bl  0204C6E4          ALLOCATE
020EEB58  str r0, [r4, #0x10]   the mask
020EEB64  lsl r0, r0, #2        height * 4
020EEB6C  bl  0204C6E4          ALLOCATE
020EEB70  str r0, [r4, #0x14]   the row table
```

Confirmed by running it. With the immediate patched to 56 tiles, the live
descriptor becomes stride 7, width 56 — and the row table **moves** from
`02245258` to `022452A0`, exactly the +0x48 = 72 bytes by which the mask grew
from 96 to 168. The allocator did its job.

So the port patches one immediate (`LB_VIEW_WIDTH=448`) and the game sets up a
56x24-tile view through its own code path, correctly sized.

## And it changes nothing on screen

This is the result that matters, and it is negative.

With the raster already widened (`LB_FORCE_WIDTH=448`, `LB_BG_EXTEND=1`),
captured with and without the view patch at the same point in the same save:

    stock descriptor (32 wide): content reaches x=356 of 448
    patched         (56 wide): content reaches x=356 of 448
    pixel difference: 171 of 86016, all inside (96,58)-(221,141)

171 pixels, confined to the centre of the screen where the units animate — that
is frame phase between two runs, not widening. **The rendered field is
identical.**

So `02151F28` is the **fog/visibility viewport**, not the raster viewport. That
is consistent with what docs/163 actually established about it — a one-bit-per-
tile mask sized to the view — and inconsistent with the hope, mine included, that
it was this cartridge's `Set_View_Dimensions` in the full C&C sense. C&C's one
function owns *both*; here they are separate, and this is only the fog half.

The content limit at x=356 is therefore somewhere else, and docs/167 already says
where to look: BG1/BG2 are a scroll double-buffer holding one screen's worth of
terrain in VRAM at a time. That is a VRAM streaming limit, not a viewport
constant, and no immediate patch will move it.

## What this is still worth

* The fog viewport **can** be widened safely, correctly sized, through the game's
  own allocator. Whatever eventually widens the raster will need exactly this,
  and it is now done and verified rather than feared.
* docs/164's overrun is retracted, which removes a blocker that had been stated
  as a shipping condition.
* The lesson repeats docs/175's: a confident negative inference ("nothing was
  allocated for that") stood unchallenged across a document, and eleven more
  instructions settled it. Reading further was cheaper than the caution had been.

`LB_VIEW_WIDTH` is off by default. It correctly widens the fog viewport and, on
its own, does nothing a player would see.
