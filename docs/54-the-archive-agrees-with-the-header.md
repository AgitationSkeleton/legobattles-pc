# The archive agrees with the header

**Status: `sound_data.sdat` is decoded — the container, both symbol tables, the
499-entry FAT, and the file the whole thing sits in. Its 1,311 symbol names are
identical to the shipped header's 1,311 `#define` names, at the same indices,
with nothing in either file pointing at the other.** Data decoded 99.1% by
file; **the byte column moves from 19.5% to 71.4%.** Six new checks; 56 of 56
pass.

This is the largest file on the cartridge: 62,902,976 bytes, 51.9% of it by
weight. `docs/30` filed it as *known format, no parser here*, which was the
honest tier while nothing here read it. It is read now.

## The container

```
+0x00  char[4]  "SDAT"        +0x10  u32 x2  SYMB offset, size
+0x04  u16      BOM 0xFEFF    +0x18  u32 x2  INFO offset, size
+0x06  u16      version       +0x20  u32 x2  FAT  offset, size
+0x08  u32      file size     +0x28  u32 x2  FILE offset, size
+0x0C  u16      header size
+0x0E  u16      block count
```

```
SYMB  at 0x00000040  size      41,623   block says 41,624  (padded)
INFO  at 0x0000A2D8  size       6,744   block says 6,744
FAT   at 0x0000BD30  size       7,996   block says 7,996
FILE  at 0x0000DC6C  size  62,846,548   block says 62,846,548

0xDC6C + 62,846,548 == 62,902,976 == the file size
```

The FILE block ends the archive exactly. Note the padding on `SYMB`: the header
gives 41,623 and the block's own size field gives 41,624. **The block's own size
is the one that tiles**; using the header's would put `INFO` one byte early. A
reader that trusts only the outer table gets a plausible, wrong answer on the
very first block.

## SYMB and INFO

Both blocks are a magic, a size, and then **eight offsets, one per record
type**, in a fixed order:

```
SEQ   SEQARC   BANK   WAVEARC   PLAYER   GROUP   STRMPLAYER   STRM
```

Each offset points at a `u32 count` and then that many entries. `SEQARC` is the
exception on the SYMB side: its entries are **eight** bytes, a name offset plus
an offset to that archive's own table of sequence names.

That nesting is the thing to notice. `docs/52` recovered exactly this structure
from the header alone, by cutting the 784 `SE` lines wherever the value returned
to zero and asserting that the resulting 217 runs were the 217 archives. **The
`.sdat` carries the same nesting explicitly**, and it can be compared.

## The comparison

Two files, built by two different tools at two different times, neither
referencing the other. `sound_data.sadl` is source text a compiler consumed;
`sound_data.sdat` is a binary the audio pipeline emitted.

```
                SYMB   INFO   header
SEQ                0      0        -
SEQARC           217    217      217
BANK             200    200      200
WAVEARC            8      8        8
PLAYER             2      2        2
GROUP              0      0        -
STRMPLAYER         2      2        2
STRM              98     98       98
```

Every count agrees three ways. Then, name for name at the same index:

```
SEQARC        217 of 217
BANK          200 of 200
WAVEARC         8 of 8
PLAYER          2 of 2
STRMPLAYER      2 of 2
STRM           98 of 98
nested SE     784 of 784
total       1,311 of 1,311      and the header has 1,311 #define lines
```

**1,311 of 1,311.** The `.sdat`'s symbol strings are the C macro names verbatim,
`SEQARC__KING_TOWER` and `SE_KING_MELEE_ONHIT` and all the rest, stored as
NUL-terminated text inside a 62.9 MB binary.

Two things follow, and the second is why this pass matters more than the byte
count:

1. The header is not a stale leftover. It describes the archive that shipped.
2. **`docs/52`'s run-cutting was right.** That was a derivation — a hypothesis
   about emission order, checked only against itself. The `.sdat` states the
   grouping outright, and it matches 784 out of 784. A derived reading and a
   declared one, agreeing completely.

## FAT and FILE

```
FAT: 499 entries, FILE block declares 499
     499 of 499 lie inside the FILE block
     62,840,312 bytes of payload
     payload magics: SSAR 205, SBNK 188, STRM 98, SWAR 8
```

Each FAT entry is 16 bytes: offset, size, and eight reserved. Every one of the
499 lands inside the FILE block, and the four payload magics are the four
Nintendo sound formats — sequence archives, banks, streams and wave archives.

## Twelve towers, explained three times

`205 SSAR files` against `217 SEQARC` symbols, and `188 SBNK` against `200
BANK`. Twelve of each are shared, and **they are the same twelve**: each
faction's `TOWER`, `TOWER2` and `TOWER3` point at one sequence-archive file and
one bank file. 18 names, 6 files, both ways.

This closes something `docs/52` got slightly wrong. It called the 36 duplicate
`#define`s "a bug in the shipped source" — a copy-paste never renamed. It is
not a slip. **The data is deliberately shared**: the three tower tiers of a
faction are one sound. The names were never renamed because there was nothing to
rename them to. `docs/52` is corrected in place.

Four independent measurements now land on the same twelve archives:

| measurement | source |
| --- | --- |
| 36 duplicate `#define` names | the header's text |
| 12 archives whose `SE` names miss their `SEQARC` name | the header's ordering |
| 12 shared `SEQARC` file ids | the archive's INFO block |
| 12 shared `BANK` file ids | the archive's INFO block |

None of the four knows about the others.

## What this does to the byte column

```
             files      %          bytes       %
decoded      3,879   99.1%    86,515,116   71.4%     was 19.5%
known            1    0.0%         2,112    0.0%     the banner
opaque          35    0.9%    34,726,008   28.6%     24 .mods, 10 .hps, one .abp
```

`docs/30` has said from the beginning that the file column is the honest one for
gameplay data, and that stays true. But the byte column had been carrying one
62.9 MB "known format, not read here" entry for the whole project, and it is
read now. What is left is video.

## Tips and pitfalls

* **Trust the block's own size field, not the outer table's.** They differ by
  one byte on `SYMB` here, and the outer one does not tile.
* **`SEQARC` symbol entries are 8 bytes, every other type's are 4.** A uniform
  stride reads 217 archives as 108 and then walks off into the sub-tables.
* **The `GROUP` and `SEQ` tables are present but empty.** Zero is a legitimate
  count, not a missing offset; a reader that treats a zero count as "table not
  present" and a zero *offset* as the same thing will conflate them.
* A "known format" tier entry is where work goes to be forgotten. This one and
  the `.sadl` in `docs/52` were both sitting in a tier that said, in effect,
  *somebody already understands this*. Neither was read. **Re-read the tier
  assignments, not just the tier totals** — that is now twice in two passes.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded, by file | **99.1%** | 3,879 of 3,915 |
| data decoded, by byte | **71.4%** | 86.5 MB of 121.2 MB |
| decompilation | 0.157% | 377 functions byte-verified |
