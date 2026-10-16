# Models, fonts, and a scale pair that proves 20.12

**Status: the last three partial formats are decoded. `NSBMD`, `NSBCA` and
`NFTR` payloads now parse, 86 files, nine structural identities, every one
holding on every file. Data decoded moves from 96.8% to 99.1% and the *partial*
tier is empty.** 50 of 50 verification checks pass.

`docs/50` left 87 files at *partial*: 39 models, 39 animations, 8 fonts, and
`sound_data.sadl`. This pass takes all of them. The `.sadl` is `docs/52`; the
other 86 are here.

## The G3D dictionary, and why it checks itself

`MDL0`, `TEX0` and `JNT0` all reach their named resources through one structure.
Read out of `K_Ballista.nsbca` at offset `0x1C`:

```
+0x00  u8   revision, 0
+0x01  u8   entry count
+0x02  u16  total dictionary size
+0x04  u16  constant 8
+0x06  u16  offset to the entry header, from the dictionary start
...         patricia tree nodes, 4 bytes x (count + 1)
+ofs   u16  entry size
+ofs+2 u16  offset to the names, from the entry header
...         entries, `entry size` bytes each
...         names, 16 bytes each
```

Three identities follow, and each uses fields the others do not:

```
ofsEntry    == 12 + 4 * count           the tree is exactly count+1 nodes
ofsName     ==  4 + count * entrySize   the names start after the entries
sizeDictBlk == ofsEntry + ofsName + count * 16
```

For `K_Ballista.nsbca`: count 1, `ofsEntry` 16 = 12 + 4, `entrySize` 4,
`ofsName` 8 = 4 + 4, `sizeDictBlk` 40 = 16 + 8 + 16. The names region then runs
`0x34`..`0x44`, and `0x1C + 40 == 0x44`. It lands.

`lb_nitro.g3d_dict` **raises** rather than returning a wrong answer, so a G3D
payload that parses at all has already satisfied all three. All 78 do.

## `MDL0` -- three counts that must agree

The model header sits where the dictionary's single entry points:

```
+0x00  u32  model size          +0x17  u8   node count
+0x04  u32  offset to the SBC   +0x18  u8   material count
+0x08  u32  offset to materials +0x19  u8   shape count
+0x0C  u32  offset to shapes    +0x1C  u32  position scale
+0x10  u32  offset to EvpMtx    +0x20  u32  inverse position scale
                                +0x24  u16  vertices
                                +0x26  u16  polygons
                                +0x28  u16  triangles
                                +0x2A  u16  quads
```

**`triangles + quads == polygons`.** Nothing in the format forces those three
fields to agree; they agree only if the header is being read at the right
offsets. `K_Ballista`: 34 + 63 = 97. **39 of 39 models hold it.**

### The scale pair, and what it confirms

`position scale` and `inverse position scale` are a reciprocal pair in the same
20.12 fixed point the simulation uses (`docs/25`), so:

```
pos_scale * inv_pos_scale == 2^24        1.0 x 1.0
```

**39 of 39.** The five values that occur are exact powers of two:

| pos_scale | as 20.12 | inverse | models |
| ---: | ---: | ---: | ---: |
| 4,096 | 1 | 4,096 | 14 |
| 8,192 | 2 | 2,048 | 6 |
| 16,384 | 4 | 1,024 | 2 |
| 32,768 | 8 | 512 | 13 |
| 65,536 | 16 | 256 | 4 |

This is worth more than a parse check. `docs/26` established 20.12 from the
simulation's own arithmetic — shifts by 12 in unit speed and damage code. Here
the *3D exporter*, a completely separate part of the toolchain, stores a
reciprocal pair whose product is `2^24`. Two unrelated subsystems, one fixed-
point convention, and neither was told about the other.

## `TEX0` -- two stores that tile

```
+0x0C  u16  texture bytes >> 3     +0x30  u32  palette bytes >> 3
+0x0E  u16  offset to the texture dictionary
+0x14  u32  offset to the texture data
+0x34  u32  offset to the palette dictionary
+0x38  u32  offset to the palette data
```

The two stores sit back to back at the end of the block:

```
ofsTex + textureBytes == ofsPal          no gap between them
ofsPal + paletteBytes == block size      and the palettes end the block
```

**39 of 39 on both.** Both sizes are stored shifted right by three; reading them
unshifted misses the first identity by a factor of eight on every file, so a
wrong reading is loud rather than quiet.

`K_Ballista`: `0x90 + 512 = 0x290`, `0x290 + 32 = 0x2B0` = the block size. The
texture dictionary's `texImageParam` says format 3 (16 colours) at 32x32, which
is 512 bytes at 4bpp — the same 512 the header declares, from a different field.

Across the cartridge: **39 models, 292 bones, 44 materials, 46 shapes, 11,505
vertices, 3,963 polygons (1,775 triangles + 2,188 quads), 39 textures, 39
palettes, 35,840 bytes of texture and 9,184 of palette.** One model, one
texture and one palette per file, without exception.

## `NSBCA` -- one animation each

Every `.nsbca` holds a single `JNT0` block whose dictionary has exactly **one**
entry, named for the model. 39 files, 39 animations. The joint animation data
that follows starts with the signature `4A 00 41 43`; its contents are per-bone
keyframe streams and are not unpacked here.

That the animations are one-per-file matters for a port: there is no animation
*set* to index into, so whatever selects between walk, attack and death is doing
it somewhere else. `docs/20` put the combat model in the simulation; this says
the animation selection is not in the model files.

## `NFTR` -- five identities, eight fonts

```
FINF  +0x01 u8  line feed        +0x08 u32 offset to CGLP payload
      +0x05 u8  default width    +0x0C u32 offset to CWDH payload
      +0x07 u8  encoding         +0x10 u32 offset to CMAP payload
CGLP  +0x00 u8  cell width       +0x02 u16 cell bytes
      +0x01 u8  cell height      +0x06 u8  bits per pixel
CWDH  +0x00 u16 first char       +0x02 u16 last char
      then 3 bytes per glyph: left bearing, glyph width, advance
CMAP  +0x00 u16 range start      +0x04 u16 mapping method
      +0x02 u16 range end        +0x08 u32 offset to the next CMAP payload
```

1. **`FINF`'s three offsets each land eight bytes past a section start** — on
   that section's payload. This ties the header to the section table without
   either being assumed. 8 of 8.
2. **`cellBytes == ceil(width * height * bpp / 8)`.** 8 of 8.
3. **`lastChar - firstChar + 1` cells fit inside `CGLP`.** 8 of 8. For the
   English font that is 82 glyphs x 17 bytes = 1,394, inside a 1,412-byte
   section.
4. **The `CMAP` sections form a linked list** whose `next` lands on the
   following `CMAP` payload and ends in zero. 8 of 8, two maps each.
5. **`FINF`'s line feed equals `CGLP`'s cell height** — different sections,
   same number, in all eight. The fonts have no leading.

| font | glyphs | cell | bpp | cell bytes | line feed |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Font/DK/font.NFTR` | 84 | 10x13 | 1 | 17 | 13 |
| `Font/DN/font.NFTR` | 82 | 10x13 | 1 | 17 | 13 |
| `Font/EN/font.NFTR` | 82 | 10x13 | 1 | 17 | 13 |
| `Font/ES/font.NFTR` | 85 | 10x14 | 1 | 18 | 14 |
| `Font/FR/font.NFTR` | 83 | 10x14 | 1 | 18 | 14 |
| `Font/IT/font.NFTR` | 82 | 10x13 | 1 | 17 | 13 |
| `Font/JP/font.NFTR` | 102 | 15x15 | 1 | 29 | 15 |
| `Font/MSMincho-12.NFTR` | 161 | 11x12 | 1 | 17 | 12 |

Every font is **1 bit per pixel**. The English font's second `CMAP` uses
method 2, a list of character/glyph pairs, and contains exactly two: `U+2019`
(a right single quote) and `U+2026` (an ellipsis) mapping to glyphs 80 and 81 —
the two highest indices, which is also the `lastChar` the width section
declares. Two sections, one number, arrived at differently.

The `JP` font has 102 glyphs and `MSMincho-12` has 161. Neither is remotely
enough for Japanese text, so the Japanese build must compose its text some other
way. **That is a gap, not a finding** — recorded here so it is not mistaken for
a conclusion.

## Where the tiers stand

```
decoded  3,878  99.1%    was 3,791 / 96.8%
partial      0     -     empty for the first time
known        2   0.1%    the .sdat sound archive and the banner
opaque      35   0.9%    24 .mods videos, 10 .hps particles, Animations.abp
```

`partial` was always a holding pen: header understood, body not. Emptying it
took four passes (`docs/47`, `49`, `50`, this one) and the same method each
time — read one file, find the arithmetic that must hold, run it over the
population, report the count.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | **99.1%** | 3,878 of 3,915 files |
| decompilation | 0.157% | 377 functions byte-verified |
