# `BP/*.tbp` — the tile blueprints, and how terrain is drawn

**Status: the format is solved and validated on all 136 files. The rendering
chain for the map's `u16` plane is established; one of the byte planes is not
what it first looked like, and that is recorded rather than smoothed over.**

## The format

```
u16     count
u16[6]  entry[count]
```

That is all. **136 of 136 files fit `2 + count * 12` exactly**, which is the
whole self-test — a wrong element width or a wrong header size would miss on
every file, not some of them.

| file | entries |
| --- | ---: |
| `KingTiles.tbp`, `MarsTiles.tbp`, `PirateTiles.tbp` | **5,120** each |
| `DetailTiles_<map>.tbp`, 133 of them | 45 – 590 |

The three world tilesets have *identical* entry counts, which matches everything
else about this game's structure: the worlds are parallel, not different.

## The six values are NCGR tile indices

Each entry's six `u16` are indices into the matching `<world>Tileset.NCGR`.
Those files decompress to 65,600 bytes = **2,048 8×8 4bpp tiles**, and the
largest index any `.tbp` uses is 1,452 — comfortably inside.

> **CORRECTED, docs/387.** The NCGRs are **8bpp, 1,024 tiles** (65,600 bytes
> is 1,024 × 64 plus the header), and an entry `u16` is a BG screen entry, not
> a bare index: bits 0–9 the tile number (≤ 0x3FF), bits 10/11 the h/v flip
> (used only by Mars's table A). So "1,452" was an entry with a flip bit set. The first entries
of `DetailTiles_ck1_1.tbp` read `0x018F 0x0190 0x0191 0x0192 0x0193 0x0252`:
five consecutive tiles and one from elsewhere, exactly what a reused corner or
edge piece looks like.

Six 8×8 tiles per map cell means a cell is **24×16 or 16×24 pixels**.

## The rendering chain, for the `u16` plane

`docs/15` found the `TERR` chunk's fifth plane to be `u16[w×h]` with per-row
pointers built at load. It is the tile index:

```
map plane E[x, y]
  -> <Tileset>Tiles.tbp entry
    -> six NCGR tile indices
      -> <Tileset>.NCGR
```

Checked across **all 830,720 cells of all 122 maps**: plane E ranges 13 to 919,
and **not one cell exceeds the 5,120 entries** its tileset provides. That bound
holding on the whole corpus, with no exceptions to explain away, is what makes
this a conclusion rather than a guess.

## Plane B is *not* a DetailTiles index

The obvious next step was that plane B — the sparse byte plane `docs/15` found
tracking region boundaries — indexes the per-map `DetailTiles_<map>.tbp`. The
first map checked fit perfectly: every value under 261, and `ck1_1`'s detail
file has exactly 261 entries.

**It fits only 70 of 121 maps.** `ma1_1` has 289 cells above its detail file's
143 entries; `ck1_3` has 239 above 216. So the reading is wrong, and it would
have been adopted on one file's evidence.

What the failures show is more interesting than the failure. **The overflow
maximum is 244 in every failing map** — never 245, never 250 — and `ck1_1`'s
plane-B values fall in three bands with nothing between them:

```
1, 4, 7, 32, 36, 41, 47        128, 129, 148, 151        224, 233, 244
```

Three clusters, a hard ceiling at 244, and a separate `255` sentinel. That is
the shape of a **packed field** — a type in the high bits and an index in the
low ones — not a flat index into anything. It is left unnamed.

The 87%-vs-24% boundary correlation from `docs/15` still stands and is
consistent with this: whatever plane B encodes, it is placed at the edges
between plane-A regions.

## What this leaves

- **Terrain is now renderable.** Plane E, the tileset blueprint and the NCGR are
  a complete chain, and NitroPaint already reads the NCGR.
- Plane A (4 values) and plane C (10 values) are still unnamed. Neither is a
  tile index — their ranges are far too small — so they are logical layers:
  ground type, passability, or build-legality.
- The `DetailTiles_<map>.tbp` files are indexed by *something*, and it is not
  plane B as read. The `TERR` chunk's variable-length block at `+0x28 + 3·w·h`
  (231 bytes in `ck1_1`, 45–801 across the corpus) is the obvious candidate and
  has not been examined.

  > **ANSWERED, docs/387.** By plane E, the u16 metatile plane (map+0x30):
  > Terrain_DecodeMetatile (0x020A3478) reads table A (`BP/<World>Tiles.tbp`)
  > for a value below the split `*(u16*)0x0214B9D2` = 440, and
  > `DetailTiles_<map>.tbp` at value − 440 otherwise.
