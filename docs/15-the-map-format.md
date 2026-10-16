# `Maps/*.map` — the battlefield container and its terrain

**Status: solved. The container, the terrain chunk and all four record chunks
parse to the byte on all 122 maps.**

`python tools\lb_map.py out\eur\plain\Maps --all --tsv docs\data\maps.tsv`

```
maps parsed: 122   failed: 0
dimensions: [((96, 96), 65), ((64, 64), 56), ((48, 48), 1)]
tilesets:   {'KingTileset': 41, 'MarsTileset': 40, 'PirateTileset': 41}
```

## The container

```
'MAP'                3-byte magic
'TERR' … 'RRET'      terrain, fixed size, closed by its own tag reversed
'EVNT' … 0x21        events
'TRIG' … 0x21        triggers
'MARK' … 0x21        markers
'MINE' … 0x21        mines
'PAM'                3-byte terminator — the magic, reversed
```

The chunk tags came from the third-party project's `Battles::Map` string
constants (`docs/02`); the layout is read from the loader.

**Two different terminators, and the reason is structural.** `TERR` is a fixed
size the loader computes from the width and height, so it closes with a marker
(`RRET`) that the loader simply steps over. The other four are *record streams*
of unknown length, so they end with a sentinel byte `0x21` (`'!'`) that the loop
tests each iteration. `Battles::Map::LoadEvents` shows it plainly:

```arm
adds r0, r1, #4        ; past the tag
adds r0, r0, #1        ; past the first record's mark byte
ldrb r1, [r1, #4]
cmp  r1, #0x21         ; '!' — an empty chunk
beq  done
```

Validated on all 122 maps: the four record chunks always appear in the order
`EVNT TRIG MARK MINE`, each one immediately follows the previous chunk's `0x21`,
`EVNT` immediately follows `RRET`, and every file ends `PAM`.

## The `TERR` chunk

From `Battles::Map::LoadTerrain` at `0x020A3A08`:

| offset | type | field | where the loader puts it |
| --- | --- | --- | --- |
| `+0x00` | char[4] | `'TERR'` | |
| `+0x04` | u8 | **width** | `map+0x14` |
| `+0x05` | u8 | **height** | `map+0x15` |
| `+0x06` | u8 | unknown (3 in `ck1_1`) | |
| `+0x07` | u8 | unknown (2 in `ck1_1`) | |
| `+0x08` | char[32] | **tileset name**, NUL-padded | a string at `map+0x18` |
| `+0x28` | u8[w·h] | plane A | `map+0x2E8` |
| | u8[w·h] | plane B | `map+0x2F0` |
| | u8[w·h] | plane C | `map+0x2EC` |
| | u16 | `n` | |
| | u8[n] | extra block | `map+0x2F4` |
| | u16[w·h] | plane E | `map+0x30`, with per-row pointers built at `map+0x34` |
| | char[4] | `'RRET'` | |

The loader also builds a `Rect` covering `(0,0)`–`(w-1,h-1)` before anything
else, which is what confirms `+0x04`/`+0x05` are width and height rather than
something else two bytes wide.

**The self-test.** `0x28 + 3·w·h + 2 + n + 2·w·h` must land exactly on `RRET`.
It does, for all 122 maps. That is a strong check: the three byte planes, the
length-prefixed block and the 16-bit plane all have to be right simultaneously
for the sum to hit a four-byte marker.

### What the planes hold

`tools/lb_map.py --pgm` writes them as images, and rendering three maps settled
most of it at a glance. For `ck1_1` (64x64), planes A, B and C left to right:

- **Plane A - ground type.** Four values, in large contiguous regions. Looks
  like a map.
- **Plane B - the transition layer.** Almost entirely `0`, with values appearing
  as thin lines tracing the *boundaries between plane-A regions*. That is the
  shape of an auto-tiling blend index: which corner/edge tile to draw where two
  ground types meet.
- **Plane C - a second regional layer.** Ten values, large contiguous regions,
  but its boundaries are **not** plane A's - smoother, and it fills in holes
  that A leaves. Passability, height or a second visual layer; not settled.

The plane-B reading was checked rather than left as an impression. Over all 122
maps, classifying every cell by whether plane A changes value in any of its
eight neighbours:

| plane B | on an A boundary | in an A interior | share on a boundary |
| --- | ---: | ---: | ---: |
| has a real value | 88,289 | 13,197 | **87.0%** |
| is `0` or `255` | 175,022 | 554,212 | 24.0% |

A 3.6x enrichment over the base rate. That is strong support for "B indexes the
transition between A regions" and it is **not a definition** - 13% of B's values
sit in a region interior, so either the layer does more than blending or the
eight-neighbour test is too coarse for it. Recorded as strongly supported, not
as established.

## Three tilesets for six factions

Only **three** tileset names exist across all 122 maps: `KingTileset` (41 maps),
`MarsTileset` (40), `PirateTileset` (41). So the two campaigns of each world
share terrain art entirely, which matches what `docs/12` found about the
rosters — the factions are a reskin over shared structure, and here they do not
even get that.

The three `*Tileset.NCGR` files are the largest graphics in the ROM's root
(`docs/04`): 39,064, 38,828 and 36,756 bytes.

## Map sizes

| size | maps |
| --- | ---: |
| 96×96 | 65 |
| 64×64 | 56 |
| 48×48 | 1 (`test1`) |

So a battlefield is at most 9,216 tiles, and the terrain data is 5 bytes per
tile plus a small extra block — 46 KB for a large map, which is why the map
files are the only `COMP` payloads of that size.

## The four record chunks

Every one is a stream of records introduced by `0x4C` (`'L'`) and closed by
`0x21`. Their grammars come from the loaders, and each sub-record's size is
simply **the number of times its reader advances the shared cursor** - countable
from the disassembly, and in every case unconditional, because all the cursor
stores precede the first branch.

| reader | size | what it reads |
| --- | ---: | --- |
| `func_020A5C2C` | 9 | an event condition |
| `func_020A4610` | 8 | an event action |
| `func_020A5DE0` | 3 | an event's third list |
| `Battles::Map::LoadMarker` | 3 | one marker |
| `func_020A5494` | 1 or 11 | a trigger body, gated by its first byte |

### `EVNT` - `func_020A4B90`

```
'L'                       frame; 0x21 here ends the chunk
u8    id                  an even enum, 0...46
u8    n_points
(u8 x, u8 y) * n_points
u8    area_flag
u8[4]                     only when area_flag is set
u8    n_conditions   ;  9 bytes each
u8    n_actions      ;  8 bytes each
u8    n_third        ;  3 bytes each
```

### `TRIG` - `func_020A5494`

```
'L'   u8 id                an even enum, 2...44
u8    flag
u8[10]                     only when flag is set
```

### `MARK` - `Battles::Map::LoadMarkers`

```
'L'   u8 group_id
u8    count
u8[3] * count              one marker each
```

### `MINE` - `Battles::Map::LoadMines`

```
'L'   u8 count
(u8 x, u8 y) * count
```

and there are **exactly four groups in every map, one per team.**

### Totals across all 122 maps

| chunk | records | contents |
| --- | ---: | --- |
| `EVNT` | 1,391 | 4-31 per map |
| `TRIG` | 157 | 154 with the flag set, 3 without |
| `MARK` | 746 groups | 2,747 individual markers |
| `MINE` | 488 groups (4 x 122) | 335 individual mines |

Most events are small: 920 of 1,391 carry no points, 1,113 carry no actions, and
1,334 carry an empty third list. The heavy ones are rare and are presumably the
mission logic proper.

### A coincidence that was nearly a conclusion

The `EVNT` ids are 24 even values `0...46` and the `TRIG` ids are 22 even values
`2...44`. `Battles::EventBase` has **exactly 22 subclasses** in the RTTI, so
"trigger id divided by two selects an `EventBase` subclass" is an inviting
reading.

It is wrong. `func_020A483C`, the constructor the event loader calls, is not a
factory at all - it writes one fixed vptr, `0x0214BA78`, which the RTTI resolves
to **`Battles::User::Map::OnEventData`**. The id is an enum stored in the
record, not a class selector. And `Battles::EventBase`'s subclasses are
`ButtonEvent`, `PenEvent`, `WiFiErrorEvent`, `PauseGameEvent` - the UI and
netplay event bus, nothing to do with maps.

Two unrelated things in this game are both called "event", they have similar
counts, and the enum being even made the coincidence look like a mechanism.
Checking what the constructor actually stored took one lookup.

**Still unnamed:** what the id enums mean, and the field layout inside the 9-,
8-, 3-, 10- and 3-byte sub-records. Those need the readers' bodies rather than
just their cursor arithmetic.

## What this unlocks

- **Terrain is now readable and rewritable** — the three planes are plain
  `w × h` byte arrays at a computable offset, and `COMP` (`docs/05`) rebuilds
  its size table, so an edited map of a different size loads.
- **Custom maps are a container away.** The community already ships map mods;
  this is the format they edit.
