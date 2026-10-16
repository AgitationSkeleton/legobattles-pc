# Subsystem map of the 200 largest ARM9 functions

Companion to `subsystem_map.tsv`. The 200 largest non-thunk ARM9 functions are
**240,816 bytes — 23.8% of the 1,012,091-byte ARM9 corpus** (6,833 functions).

## Bytes per subsystem

| subsystem | bytes | % of the 200 | functions | high / med / low |
| --- | ---: | ---: | ---: | --- |
| UI/menus | 59,086 | 24.5% | 43 | 18 / 25 / 0 |
| simulation/units | 56,668 | 23.5% | 47 | 25 / 22 / 0 |
| 2D rendering | 23,856 | 9.9% | 19 | 15 / 4 / 0 |
| AI | 21,096 | 8.8% | 19 | 13 / 6 / 0 |
| effects/particles | 17,264 | 7.2% | 16 | 13 / 3 / 0 |
| 3D rendering | 15,212 | 6.3% | 16 | 9 / 7 / 0 |
| math/util | 12,372 | 5.1% | 10 | 9 / 1 / 0 |
| FMV/video | 7,068 | 2.9% | 6 | 2 / 4 / 0 |
| networking | 5,172 | 2.1% | 5 | 1 / 4 / 0 |
| pathfinding | 5,148 | 2.1% | 3 | 2 / 1 / 0 |
| save/load | 4,606 | 1.9% | 5 | 4 / 1 / 0 |
| runtime/C++ ABI | 3,896 | 1.6% | 3 | 1 / 2 / 0 |
| audio | 2,924 | 1.2% | 2 | 1 / 1 / 0 |
| mission/scripting | 2,604 | 1.1% | 2 | 1 / 1 / 0 |
| file/archive | 1,740 | 0.7% | 2 | 1 / 1 / 0 |
| game events/commands | 1,264 | 0.5% | 1 | 1 / 0 / 0 |
| **unknown** | **840** | **0.3%** | **1** | **0 / 0 / 1** |
| **total** | **240,816** | 100% | **200** | 116 / 83 / 1 |

## What decided each row

| basis | functions | bytes |
| --- | ---: | ---: |
| read the decompiled C (and, for 18, the disassembled ARM9) | 124 | 152,554 |
| RTTI vtable slot shipped in the ROM | 37 | 47,414 |
| RTTI caller-derived (`::helper` — every caller is a method of the class) | 19 | 19,804 |
| translation-unit adjacency only | 20 | 21,044 |
| nothing sufficient | 1 | 840 |

61.4% of the bytes are `high` confidence, 38.2% `medium`, 0.3% `low`. Only one
function — `0x020F616C`, 840 bytes — resisted classification entirely: it has no
callers, no callees, and its own isolated 0x40-byte data block.

## Which subsystems dominate

**UI/menus and simulation/units together are 48% of these bytes.** That is the
real shape of this game: a large menu/front-end surface (43 functions across
~40 screen classes) sitting on top of a large deterministic RTS simulation
(47 functions of entity state, commands, combat, production).

Rendering is fragmented rather than small. `2D rendering` (23,856) +
`3D rendering` (15,212) + `effects/particles` (17,264) + `FMV/video` (7,068) is
63,400 bytes, 26% — bigger than either headline category, but spread over four
mechanisms that share no code.

## The band nobody had named: 0x02038000–0x0203C000

Six of these functions (6,388 bytes) sit in an address band that carries **no
RTTI at all**, which is why it had no name. It is the **in-game battlefield
renderer**, and it is a hybrid:

- terrain is a scrolling BG tilemap — `0x02039FB4` writes 2×2 u16 metatiles over
  a 10×12 cell grid, three tiles per cell horizontally;
- units are 3D models pushed through GXFIFO under an orthographic projection
  sized to the 256×192 screen — `0x0203B3AC` writes `DISP3DCNT` (0x04000060) and
  the ortho volume;
- `0x02039148` is the root: it culls to the visible rect, brackets the passes in
  MTX_PUSH/IDENTITY/POP, and rebuilds the terrain tilemap only when the
  visibility bitmap changed.

This was established by disassembling the ARM9 rather than trusting Ghidra
(every hardware address in the C is hidden behind a `DAT_` literal-pool symbol).
It corroborates `docs/174` and `docs/226`, which independently place the field
camera and the entity-gather rect inside `FUN_02039148`.

## Most tractable to reimplement natively

1. **simulation/units, AI, pathfinding** (82,912 bytes). Pure integer/fixed-point
   logic over plain structs. No hardware, no SDK. The RTTI gives class and method
   names for most of it, and the deterministic simulation must be reimplemented
   exactly anyway for lockstep multiplayer to work — so "tractable" and
   "mandatory" coincide here. This is where native reimplementation pays best.
2. **save/load** (4,606 bytes). A self-contained MSB-first bitstream serializer
   (`0204BA48`/`0204BAB0` write, `0204BA64` read, `0204BB84` flush) over numbered
   records. Small, closed, and testable against real cartridge saves.
3. **UI/menus** (59,086 bytes). Tedious but shallow: mostly `switch` over widget
   ids, a shared focus-chain helper (`020E0E10`, 86 callers), and asset-path
   string building. Volume, not difficulty.
4. **math/util** (12,372 bytes). printf formatting and IEEE-754 soft-float —
   replace with the host libc outright, do not port.

## Least tractable

1. **2D rendering** (23,856 bytes). Every function writes DS-specific hardware:
   BGxCNT field packing, 4bpp tiled-surface addressing
   (`+0x84 + (y>>3)*0x200 + (y&7)*4`), VRAM bank juggling, OAM. None of it maps
   onto a modern GPU without being rewritten from intent rather than translated.
2. **3D rendering** (15,212 bytes). The geometry engine is a command FIFO with
   20.12 fixed-point matrices and a 2048-polygon budget. Behaviour depends on
   hardware quirks, not on the code.
3. **FMV/video** (7,068 bytes). A hand-written-assembly codec in 0x0212C000+
   whose only external entry is `020F7964`. Ghidra cannot even recover its
   calling convention (`unaff_r6`, `unaff_r11`). Replace the container, do not
   port the decoder.
4. **networking** (5,172 bytes). Sits on the Nintendo wireless (WM) library and
   the ARM7 over IPC FIFO. There is no host equivalent; the transport must be
   replaced wholesale, keeping only the lockstep command format above it.

## Honest limits of this map

- **The 200 are the largest functions, not the most important ones.** Large
  correlates with "contains a big switch", which over-samples dispatchers and
  under-samples hot inner loops. `Entity_Tick` (1,316 bytes) is here;
  many of the small per-frame functions it calls are not.
- **Coverage is uneven across subsystems.** Measured against a coarse
  whole-corpus band map, these 200 cover ~26% of simulation bytes, ~27% of AI,
  ~20% of UI — but only **7.4% of mission/scripting, 9% of audio and 4.6% of
  file/archive**. Those three are built from many small functions, so their
  representation here understates them badly. Mission logic is additionally
  spread across 91 ROM overlays that are not in this corpus at all.
- **`medium` mostly means "the mechanism is certain, the label is a judgement
  call"** — chiefly at two seams: entity-command ticks that both query terrain
  passability *and* enqueue a MoveUnitAction (filed under simulation, since the
  route search lives inside the action), and widget code that both lays out a
  screen *and* emits tiles.
- **Ghidra's C was treated as a hint throughout.** Dropped call arguments were
  observed directly in this pass — e.g. `FUN_02096C4C` is called with four
  arguments but decompiles with three, and `0205D7F0`'s recovered prototype
  disagrees with its own body. No verdict in the map rests on an argument list.
