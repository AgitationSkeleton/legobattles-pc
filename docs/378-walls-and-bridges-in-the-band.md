# 378 — Walls and bridges in the band, and the minimap box's map size

Status: **fixed, both.** Walls are measured in and out of the band.
Bridges use the same code path, but no bridge was on screen in a test.

## 1. Walls and bridges are drawn in the widened band

The case seen in play: in the campaign tutorial, a
wall in the native rows vanished as soon as a D-pad scroll moved it into
the band.

docs/367 read the cause out of the cartridge.

* **Walls and finished bridges are BG1 tiles.** `Terrain_DecodeMetatile`
  (`020A3478`) stamps them over each cell's metatile with two passes: the
  wall stamp `FUN_02034B44` and the bridge-deck stamp `FUN_0200D8A8`.
* **The band never ran them.** The guest runs both passes only for its own
  32 columns, and the band fill in `lb_widemap` decoded metatiles alone.

**The fix is docs/367 section 7's replica**, in `lb_widemap.cpp` (namespace
`lbstamp`). `metatile_tiles()` calls it after the plain decode.

* **Reads only.** It calls no guest code and writes no guest memory.
* **Same answers without the guest's writes.** The guest's passes store the
  wall's connection cache (`+0x224`) and clean stale occupancy handles. The
  replica computes the same answers without doing either.
* **Everything the cartridge reads, it reads:**
  * the occupancy walk over three layers;
  * the visible-now grid, with its three overrides;
  * the sticky connection-mask rule;
  * the parity tables, team column, highlight lists and damage thresholds;
  * the bridge's finished byte, footprint orientation and team deck
    metatile from table A.
* **Every pointer is validated as main RAM.** Some of these globals read 0
  at the vblank hook (section 2 below), and a failure means no stamp.

`metatile_tiles()` is the one lookup behind four consumers:

1. the band fill;
2. the band camera check (`sample_mismatch`, whose docs/368 skip of
   wall-block tiles is now redundant but harmless);
3. `LB_SCANLOG`'s terrain audit;
4. `lb_widemap_metatile()`, used by the battlefield panel.

`expand_metatile()` stays plain, because the shroud calls it with a fixed
index. `LB_STAMPS=0` turns the replica off.

The audit line now also counts the guest's wall-block tiles (0x3C8-0x3FF),
so one line says both that walls were on screen and whether the port's own
decode agreed with them.

**Measured.** In a held Free Play battle at 1473x830, a builder put a
three-cell wall right of the base (`scratchpad/wallab.py`; fast building).
The camera was then panned six steps so that the wall stood in the left band
(native x about -76..-4).

| | replica on (exp-wall378) | `LB_STAMPS=0` (exp-wall378off) |
|---|---|---|
| audit, one cell built | 0/768 differ, 6 wall tiles | 6/768 differ, 6 wall tiles |
| audit, three cells | 0/768 differ, 18 wall tiles | 18/768 differ, 18 wall tiles (the guest's 3C8/3C9/3D8/3D9 against plain terrain) |
| the wall in the band | drawn, red and blue bricks | absent: the builders stand beside bare grass |

The picture is `exp-wall378/win/band-ab.png` (off left, on right).

* **Exact everywhere the guest also draws.** The native audit is 0
  differences, connection shapes included: the middle cell joins east and
  west.
* **The band draws what the guest would.**
* **Not measured live:** a bridge deck, a damaged wall (health under 66% or
  33%), a highlighted (selected) wall, and an enemy wall under fog. They use
  the same reads, and their tables are docs/367's, taken from the ROM.

## 2. The minimap box: the map's size read 0 at vblank

docs/370 caps the box's metatile counts at N-2 and N-1, with N = `u8
map+0x14`, so that `mt2px[]` (N entries) is never read past its end. It read
N through `*0x02156CB0`.

* **What was read at vblank.** Every `[lb_mmbox]` line in these runs said
  "map 0 metatiles wide". The pointer reads 0 at the vblank hook (docs/367
  section 8.6), so N was 0 and the caps never applied.
* **What was really there.** The debug server, reading between frames, got
  `0x022BD84C`, with `+0x14` and `+0x15` both 64.
* **What that cost.** Past a visible width of 24*(N-2)+8 px, which is 1496
  on a 64-cell map, the cartridge read `mt2px[k+1]` past the table's end.
  A 3440x1440 window is a 1720-wide surface.

When the pointer reads 0, `apply_minimap_box` now takes the same byte from
one of two places:

* the map pointer that `lb_field_map_chain` captured, which exists only on
  the opt-in host-terrain path;
* failing that, the last value `lb_widemap`'s own fill read from the map
  (`lb_widemap_map_px`).

**Measured** at a 1728-wide view (`LB_PIXEL_SCALE=1`, window 1920x700, a
held battle resized through five sizes, `scratchpad/mmresize.py`):

* **Resizes.** Every write says "map 64 metatiles wide", and the extent is
  capped at 1488 (24x62) wherever the view is wider.
* **The only "map 0" write left** comes in the menus before a battle, where
  no minimap exists yet to read it: exp-mmwide378, line 100, and then
  capped from the battle's first write.

## Verification

* **Gate** (exp-gate378): counts are identical to gate370b, the eight
  pictures under 5972 are identical to gate377, and it ran at 59.78 fps.
  The gate demo has no wall on screen, and the minimap patch only changes
  anything past 1496 visible columns.
* **Vanilla** (exp-van378): 240/240 identical.

## Also measured

`func_0208938C` (docs/376) clamps the sound window's x with `u8 map+0x15`
and its y with `u8 map+0x14`. So `+0x14` is the height and `+0x15` the
width. docs/367 calls `+0x14` the width. On the square maps measured so far
the two are the same.
