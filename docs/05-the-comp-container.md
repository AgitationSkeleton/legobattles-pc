# The `COMP` container — solved

**Status: complete.** All 2,178 containers in the European ROM decode with zero
structural failures, 20.68 MB packed to 43.49 MB (2.10×). Implemented in
`tools/lb_comp.py`; re-verify at any time with:

```powershell
python tools\lb_comp.py verify out\eur
```

This matters more than a compression format usually would. Almost everything the
game owns is inside one — every map, the unit and faction tables, the tilesets,
and 1,917 of the 1,927 NCGR graphics. Nothing downstream can read a byte of game
data until this is decoded, and now everything can.

## Layout

```
offset  type      field
0x00    char[4]   'COMP', stored little-endian — the bytes on disk are P M O C
0x04    u32       decompressed_size
0x08    u32       block_count N
0x0C    i32       scratch_hint          advisory, see below
0x10    i32       block_packed_size[N]  each padded up to a 4-byte boundary
0x10+4N           N back-to-back blocks
```

Each block is one of two things, chosen by the **sign** of its table entry:

- **positive** — a standard Nintendo **LZ11** stream (compression type `0x11`),
  which declares its own decompressed length in its own 4-byte header. The entry
  is the packed length, rounded up to four.
- **negative** — the block is **stored raw**, and `|entry|` is its length. There
  is no LZ11 header. This is the packer's escape hatch for data LZ11 cannot
  shrink, and 170 files in the ROM use it — every one of them a single raw block
  covering the whole file, `BP/KingTiles.tbp` and the minigame backgrounds among
  them.

Blocks are almost always 4,096 bytes decompressed, with the last one holding the
remainder. **Almost.** 2,125 of the 2,178 containers use some other split, so the
block boundary must be read from each LZ11 header rather than derived from
`decompressed_size`. Assuming the 4 KB split was the first wrong answer this
project produced and it looked convincing on the five files that were checked by
hand first.

## Verified invariants

These hold for all 2,178 files and are checked on every decode:

- `0x10 + 4·N + Σ|block_packed_size[i]| == file size`
- for a compressed block, `consumed ≤ slot ≤ align4(consumed)`
- `Σ decoded block lengths == decompressed_size`

## `scratch_hint` is advisory

It is the decompressor's working-buffer size and tracks the largest packed
block, but it is **not reliably equal to it** and must not be enforced. Two
containers store a value four bytes below their table's true maximum:

| file | `scratch_hint` | `max(table)` |
| --- | ---: | ---: |
| `UI/AllInOne/BG_EA3M2.NCGR` | 3,060 | 3,064 |
| `UI/AllInOne/BG_WA1M1.NCGR` | 3,384 | 3,388 |

In both, the winning block's *unpadded* length rounds **down** to the stored
value while its padded slot rounds up. Whether that is a packer quirk or a
deliberate unpadded measurement is not established, and it does not affect a
single decoded byte. `tools/lb_comp.py` reports these as advisories and decodes
them correctly.

## Repacking

`comp_encode()` writes a container whose blocks are valid LZ11 streams that
*store* their input verbatim — a zero flag byte before every eight literals.
Output is 1/8 larger than the input and the game decodes it identically to a
properly compressed stream, which is all a mod tool needs.

The size table is rebuilt from the payload on every write, so **a replacement
asset need not be the same size as the original.** This is the enabling property
for the modding scope, and it is corroborated independently: the community
rebalance patch's `Entities.ebp` is 18,148 bytes against the cartridge's 17,760
and the game loads it.

A real LZ11 compressor is not written yet. It is not on the critical path —
correctness first, and NitroFS has room.

## What came out

The first four bytes of the 2,178 decoded payloads:

| stamp | count | file |
| --- | ---: | --- |
| `RGCN` (NCGR) | 1,917 | tile graphics |
| `MAPT` | 122 | `Maps/*.map` — `MAP` magic followed by a `TERR` chunk |
| `BPNZ` | 1 | `BP/Entities.ebp` |
| `FANZ` | 1 | `BP/Factions.fbp` |
| various | 137 | `.tbp` tile blueprints and `.abp`, which carry no inner stamp |

`tools/lb_comp.py decode-all out\eur` mirrors the whole filesystem with every
container unwrapped, so no downstream tool needs to know this format exists.
