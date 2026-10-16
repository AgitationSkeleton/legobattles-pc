# The tilemap staging buffers, and the 3x2 metatile stamp

> **The loop analysis in "The loop that calls them" is WRONG, corrected by
> [docs/119](119-the-field-of-view-constants.md).** That loop belongs to
> func_0203A318, a 3x3 neighbourhood autotile scan, not to the builder. Its
> outer pass runs three times over a neighbourhood, not twice over two layers,
> and the span is NOT data-driven: the real bound is the immediate #0xa at
> 0203A114. The DMA and metatile findings above are unaffected.

## The path from map data to VRAM

The field's tilemap is not written to VRAM directly. It is composed in **main
RAM** and pushed by **hardware DMA**, set up by an ITCM routine.

`func_01FF8224(channel, src, dst, cnt)` is `DMA_Start`: it computes
`channel * 12`, then stores its three arguments to `DMAxSAD`, `DMAxDAD` and
`DMAxCNT` at `040000B0 + channel*12`. The transfer fires on the `DMAxCNT` store,
which is why the bus watch attributes every tilemap write to `01FF8260`, the
instruction *after* it. There is no copy loop -- looking for one wastes time.

Capturing the DMA registers during a scroll gives the field's two transfers:

```
src=02152A20  dst=06000000  cnt=80000340    832 entries    (BG2, block 0)
src=021523A0  dst=06000800  cnt=80000340    832 entries    (BG1, block 1)
```

832 entries is **26 rows of 32 columns**. The two staging buffers are adjacent
in main RAM, `0x680` apart, which is exactly 832 halfwords -- one contiguous
allocation holding both layers' maps back to back.

Other transfers seen in the same capture, for orientation: `06207800` and
`06204000` (engine B maps and tiles), `07000000` and `07000400` (OAM),
`05000000` (palette).

## The builders write 3x2 metatiles

Two routines write into the staging buffers -- one ARM at `0203A47C`, one Thumb
at `020A34B2`. Both have the same shape: **six halfword stores per call**, three
through one pointer and three through another, at offsets 0, 2 and 4.

```
0203A47C  strh r0, [r1]        020A34B2  strh r1, [r5]
0203A484  strh r0, [r1, #2]    020A34B6  strh r1, [r5, #2]
0203A48C  strh r0, [r1, #4]    020A34BA  strh r1, [r5, #4]
0203A494  strh r0, [r2]        020A34BE  strh r1, [r4]
0203A49C  strh r0, [r2, #2]    020A34C2  strh r1, [r4, #2]
0203A4A4  strh r0, [r2, #4]    020A34C8  strh r1, [r4, #4]
```

Three tiles across, two rows down, per call: the terrain is built from **3x2
tile metatiles**, and the two pointers are consecutive map rows. The ARM version
reads its six tile indices as **bytes** from a table (`ldrb r0,[r3,#N]`), so a
metatile definition is six bytes.

## The loop that calls them

```
0203A438  add  sl, sl, #1
0203A43C  cmp  sl, sb          <- inner bound is a REGISTER
0203A440  ble  0203A3A8
0203A444  add  r8, r8, #1
0203A448  cmp  r8, #2          <- outer loop runs exactly twice
0203A44C  blt  0203A38C
```

The outer loop runs **exactly two** passes, matching the two layers and the two
staging buffers. The inner loop's bound is **`sb`, a register**, not an
immediate.

That distinction matters for the widescreen goal: the span is **data-driven**,
not a hardcoded 32 baked into a comparison. If the value in `sb` is derived from
a field width that can be raised, widening the built map may be a change of
input rather than a restructuring of the loop.

## Not established

**Where `sb` comes from.** The pop at `0203A458` restores nine registers, which
does not match the ten-register push at `02039FB4`, so that is a different
function and the prologue for this loop has not been located yet. Every
assignment to `sb` found so far belongs to the neighbouring function. Until the
right prologue is found, the origin of the bound is unknown, and no claim is
made about whether it can be raised.

## Measurement caveat

Both watch captures behind this document **saturated the 512-event ring in every
window** -- 16 of 16 for the staging buffers -- so the write *counts* are not
trustworthy and no negative may be drawn from them. What is used here is only
which PCs appear and what those instructions do, read from the static image.
The distinction is the one `docs/117` was written about.
