# Cells and animations

**Status: `NCER` and `NANR` payloads are decoded, 368 of each, every one
passing a structural check. Data decoded moves from 78.0% to 96.8%.** 27 of 27
verification checks pass.

`docs/49` left 823 files at *partial* — containers validated, payloads
untouched. The two largest groups were the sprite formats, 368 files each, and
both turned out to carry offsets that check themselves.

## `NANR` — three offsets and two counts

The `ABNK` section header:

```
+0x00  u16  sequence count
+0x02  u16  frame count
+0x04  u32  offset to the sequence table
+0x08  u32  offset to the frame table
+0x0C  u32  offset to the frame data
```

Reading `Sprites/Anim1.NANR`: 1 sequence, 6 frames, offsets 24, 40 and 88.

```
24 + 1 × 16 = 40      the sequence table is 16 bytes per entry
40 + 6 ×  8 = 88      the frame table is 8 bytes per entry
```

Both land exactly. That gives the entry sizes without guessing them — **the
offsets are the measurement**, and the counts confirm it.

**Both identities hold on all 368 files.** Three offsets agreeing with two
counts, across 368 files, is not something a wrong reading survives: a wrong
entry size would break the second identity everywhere, and a wrong count field
would break the first.

Across the cartridge: **368 sequences and 380 frames** — most animations are a
single sequence of one frame, which fits a UI sprite set rather than character
animation. The real animation lives in the 39 `NSBCA` files.

## `NCER` — a flag that predicts a size

The `CEBK` section header:

```
+0x00  u16  cell count
+0x02  u16  flags
+0x04  u32  offset to the cell table
+0x08  u32  mapping mode
```

Cell entries are not a fixed size, and **the low bit of `flags` selects it**:
set means 16 bytes per entry, clear means 8.

That is a claim about a single bit, so it needs the population to test it. **364
files set the bit and 4 do not**, and the cell table fits inside its section in
all 368 under the size the bit selects. Had the bit meant something else, the
four odd files would be the ones to overflow — they are exactly the ones the
rule treats differently, and they fit.

541 cells across the cartridge.

## What that leaves

```
decoded  3,791  96.8%    was 3,055 / 78.0%
partial     87   2.2%    NSBMD 39, NSBCA 39, NFTR 8, .sadl 1
known        2   0.1%    the .sdat sound archive and the banner
opaque      35   0.9%    24 .mods videos, 10 .hps particles, Animations.abp
```

The 87 partial files are the 3D model and animation formats, the fonts, and the
sound symbol table — each needing its own reading rather than a shared
container. The 35 opaque are the genuinely unknown ones.

By bytes the picture is unchanged at 19.1%, because the 62.9 MB `.sdat` and
34.6 MB of video dominate everything. `docs/30` has said from the beginning that
the file column is the honest one for gameplay data.

## The shape of the last three passes

`docs/47`, `docs/49` and this one all worked the same way, and it is worth
naming because it is repeatable:

1. **Read one file of the type** and write down what is actually there.
2. **Find the arithmetic that has to hold** if the reading is right — sections
   tiling to the file end, `tiles_w × tiles_h` matching a byte count, offsets
   landing on each other.
3. **Run it over every file** and report the count, not a verdict.

Each step's check used *different fields than the ones being tested*, which is
what makes agreement evidence rather than restatement. None of it required
reading code, and none of it required guessing — `docs/44`'s three failed
hypotheses are what guessing costs, and they were the only wasted pass in the
sequence.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | **96.8%** | 3,791 of 3,915 files |
| decompilation | 0.157% | 377 functions byte-verified |
