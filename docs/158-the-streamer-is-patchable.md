# The streamer is patchable, and its bound behaves as documented

## Guest code in RAM takes patches

The four immediates `docs/119` identified are present in live RAM exactly as
predicted:

```
0203A114 = E359000A    cmp sb, #0x0A     the column bound
0203A100 = E2862040    add r2, r6, #0x40 the row stride
0203A108 = E2866006    add r6, r6, #6    three tiles per metatile
0203A0CC = E3A0700C    mov r7, #0x0C     the row count
```

Patching `0203A114` to `E3590005` -- halving the bound -- and nudging the camera
to force a rebuild changes **78.6%** of the field. **Guest code patched in RAM
takes effect.**

That is not obvious in a static recompilation, where generated C would ignore a
RAM edit. It works because most gameplay runs through the dynamic tier
(`docs/143`: 146 million tier-3 instructions), which reads the guest's own
bytes. So the streamer is patchable while the three settings in `docs/143`
remain unhookable -- the same property helps here and hurts there.

## And the bound means what it was read to mean

Halving it produces **horizontal stripes**: each row's terrain fills its left
half and the remainder holds stale content. That is precisely what a per-row
column count does when halved -- the builder emits 5 metatile columns of 3 tiles
instead of 10, and the rest of each 32-entry row is never written.

The picture confirms the reading in a way a byte count could not: it fails
*along the axis the constant controls*.

## The arithmetic for widening, and the obstacle

```
10 metatile columns x 3 tiles = 30 tiles written
32 entries per row available
--------------------------------------------------
 2 spare entries per row
```

There is **no useful slack**. Raising the bound to 11 writes 33 tiles into a
32-entry row and overflows into the next row, which is the striping seen above
in a worse form.

So widening needs the row stride raised from `0x40` to `0x80` **and** a
destination buffer twice the size. The two existing staging buffers sit `0x680`
apart -- exactly 832 entries -- so doubling the first would run into the second.
The builder's destination pointer therefore has to be redirected to a larger
scratch area before the bound can be raised.

## Next

Find where the builder initialises its destination pointer (`r6` at loop entry
in the caller around `0203A0C4`). That single value is what stands between a
proven-patchable bound and a genuinely wider map -- and `docs/155`'s freed
VRAM_D, or any main-RAM scratch, is somewhere to point it.
