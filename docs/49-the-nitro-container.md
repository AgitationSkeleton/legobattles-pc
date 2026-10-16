# The Nitro container

**Status: all 3,475 Nitro-container files parse, their sections tile exactly to
the end of every file, and the three graphics formats have their payloads
interpreted. Data decoded moves from 10.3% to 78.0%.** 25 of 25 verification
checks pass.

## The tier that was being left on the table

`docs/30` has always kept a *known format* tier — publicly documented Nintendo
formats that this project could name but had no parser for — deliberately out
of the decoded figure, on the ground that "it is not this project's work".

That was the right call while it was true. It stopped being a reason not to do
the work: 3,477 files, **88.8% of the cartridge by count**, sat in that tier
untouched for the whole project.

## Confirmed from the files, not recalled

The container is well known, which is exactly why it needed reading rather than
remembering. One file of each type was dumped before a line of parser was
written:

```
+0x00  char[4]  magic, stored REVERSED   'RLCN' is NCLR, 'TTLP' is PLTT
+0x04  u16      byte order mark, 0xFEFF
+0x06  u16      version
+0x08  u32      total file size
+0x0C  u16      header size
+0x0E  u16      section count
```

Then one 8-byte header per section: magic and size.

**The check that makes it a reading rather than an assumption**: the sections
must tile — their sizes, from the header size onward, land exactly on the end of
the file. That holds on **3,475 of 3,475**, and a wrong section-table layout
would miss on nearly all of them rather than some.

### The 3D formats are laid out differently, and said so

The first run parsed 3,436 of 3,475, failing on all 39 `.nsbca` with a section
size of **810,831,434 bytes**.

`BMD0` and `BCA0` do not carry an inline header per section. They carry a `u32`
**offset table** at the end of the header, each offset pointing at a block that
then has its own magic and size. Reading `A_Fighter.nsbca`: header size 16, one
block, offset `0x14`, block `JNT0` of 2,276 bytes — and `0x14 + 2276 = 2296`,
the file size exactly.

An impossible number is a good failure. 810 MB in a 2 KB file cannot be
misread as plausible, so the difference announced itself instead of producing
39 quietly wrong parses.

## The payloads

| section | fields read | format |
| --- | --- | --- |
| `PLTT` | bpp flag, data size, data offset | NCLR — 694 files |
| `CHAR` | tile width/height, bpp flag, data size, data offset | NCGR/NCBR — 1,927 |
| `SCRN` | width, height, data size | NSCR — 32 |

`bpp_flag` 3 means 4 bits per pixel and 4 means 8. **Past those fields the
payload is indexed pixels and BGR555 colours** — there is no further structure,
so these three formats are decoded rather than merely opened.

### A check the data gave for free

`tiles_w × tiles_h` should equal the tile count implied by `data_size` and
`bpp`. Those come from *different fields of the same section* and nothing makes
them agree except the format being read correctly.

**1,466 agree, 0 disagree.**

The remaining 461 NCGR files declare `0xFFFF` dimensions — NCBR "bitmap mode",
where the data is a linear image rather than a tile grid. They are **excluded
from the check rather than counted as passes**, which is the distinction
`docs/48` got wrong last pass by letting a filter shrink its own denominator.

Across the cartridge: **749,946 tiles**, 1,013 files at 8bpp and 914 at 4bpp.

## What stays partial

`NCER` (368), `NANR` (368), `NSBMD` (39), `NSBCA` (39) and `NFTR` (8) — 822
files — parse as containers with their sections listed, and their payloads are
not interpreted. Cell banks, animation banks, 3D models and fonts each need
their own reading.

```
decoded  3,055  78.0%    was 402 / 10.3%
partial    823  21.0%    was 1
known        2   0.1%    .sdat and the banner
opaque      35   0.9%    .mods, .hps, Animations.abp
```

The byte column tells a different story — 19.0% — because the 62.9 MB sound
archive and 34.6 MB of video dwarf everything. `docs/30` has said from the start
that the file column is the honest one for gameplay data, and this is the case
it was written for.

## The judgement call, stated

Promoting 2,653 files on a header parse deserves scrutiny, so here is the
standard applied: **a format is decoded when a parser here reads it and a
consumer can use the result.** For NCLR, NCGR and NSCR that is true — you can
extract every palette, every tile and every tilemap entry from what this tool
reports. For NCER and NANR it is not, and they stayed partial.

The work was not hard. It had simply never been done, because the tier it sat
in made it look like someone else's.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | **78.0%** | 3,055 of 3,915 files |
| decompilation | 0.157% | 377 functions byte-verified |
