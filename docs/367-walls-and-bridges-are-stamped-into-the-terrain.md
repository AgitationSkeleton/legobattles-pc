# 367 — Walls and bridges are stamped into the terrain (gates are not)

Status: **read out of the cartridge. Nothing was implemented or run.** Every
address, offset and access width below comes from the disassembly of the real
ARM9 image (`out/eur/arm9/section_static_02000000.bin`, via `tools/lb_code.py`).
Each was cross-checked against a full Ghidra decompilation corpus (`decomp_all_v3`).
Where the corpus and the bytes disagree, the bytes win, and the text says so.

This note was commissioned under the working guess "walls and gates". **The
second pass is the bridge deck, not the gate.** Section 4 has the evidence.

**Update, 2026-09-24 (later): part of this is now in the build.** The camera
check in `sample_mismatch()` skips cells whose guest tile is in the wall
block 0x3C8-0x3FF (section 3). This fixes the bottom band being drawn one
metatile row off. Ground-truthed at camera y 528: the band rows now match the
cartridge's own drawing 100%, where they had matched 64-95%.

With walls on screen, `LB_SCANLOG=1`'s `[lb_terrain]` audit read 44-46/768
cells differing, every one `mine != 3C8/3C9`. That is this spec's tile block,
seen live. The replica itself, which draws walls and bridges in the band, is
not written yet.

This is the specification the band fill in `port/runner/src/lb_widemap.cpp`
needs so that walls and bridges in the widened field look exactly as the
cartridge draws them in its own 32 columns. It also settles whether buildings
are in the tilemap or in 3D.

## The short version

- `Terrain_DecodeMetatile` (`0x020A3478`) runs two passes over every metatile
  it decodes, in this order:
  1. **`FUN_02034B44`, the wall stamp.** It looks up the wall (blueprint
     category 19) standing on the cell through the map occupancy grid. It
     draws only if the local player **sees the cell now**, or a reveal
     override is on. It then overwrites up to six tiles. The tiles come from a
     connection shape cached on the wall, a parity table and a team column,
     plus a highlight offset and a damage offset.
  2. **`FUN_0200D8A8`, the bridge-deck stamp.** It looks up a finished
     bridge (category 17) on the cell and overwrites all six tiles with a
     team-coloured deck metatile from `map+0x338`. It has no visibility gate.
- Nothing else writes the BG1 staging buffer during play. Buildings are
  billboards, and so are gates. Walls are the only entities with no 3D or billboard drawing at
  all, which is why the polygon list is empty where the wall stands.
- **The wall gate reads the visible-now bit grid, `holder+0x14`.** It does
  not read the explored grid at `+0x10`, which is what `lb_fog.cpp` follows.
- **Two writes to guest state happen in this path:**
  - the wall's connection cache at `entity+0x224`, which is view state kept
    inside the entity;
  - a stale-handle clean-up inside the occupancy grid.

  A host replica must do neither. It can get the same answer from reads alone.
- The replica is pure reads, about 150 lines. It belongs in `metatile_tiles()`,
  the grid path, and not in `expand_metatile()`, which the shroud also calls
  with a fixed index. Four consumers pick it up there (section 7).

---

## 1. Where the passes run

```
Task_View_StreamBG1   0x020396AC   task p0x23, only when view+0x66 is set
  Terrain_FillWindow  0x020A3234   (map=*0x02156CB0, dest=0x021523A0, camx, camy)
    Terrain_DecodeMetatile 0x020A3478   three call sites:
        0x020A32FC   main loop, 10 x 12(13) metatiles
        0x020A3376   left partial column  (camera tile x mod 3 = 1 or 2)
        0x020A341E   right partial column (mod 3 = 0 or 2, only if tilex+32 < map_w*3)
```

Inside `Terrain_DecodeMetatile`, after the six `ldrh`/`strh` of the plain
decode:

```
020A34CA  ldr   r0, [sp, #0x10]        ; the cell (5th argument, on the stack)
020A34CC  blx   0x02034B44             ; wall stamp  (cell, top, bottom)
020A34D0  movs  r3, #0xCE ; lsls #2    ; 0x338
020A34D4  ldr   r0, [sp, #0x10]        ; the cell again
020A34D6  ldr   r3, [r6, r3]           ; *(map+0x338), table A
020A34DC  blx   0x0200D8A8             ; bridge stamp (cell, top, bottom, table A)
```

`top` and `bottom` are the three-entry tile rows the plain decode just wrote.
In the main loop they point into the staging buffer. In the two partial
columns they point to stack temporaries, which the fill copies one or two
columns from. **So partial columns are stamped too.** The stamp is always
per metatile, before the column pick.

Once a frame's tasks have run, the staging buffer `0x021523A0` holds the fill
from `FUN_020396AC` alone. `Task_View_CommitBGs` (`0x020396E0`, p0x01 of the
*next* run) DMAs it to VRAM. No other code in the image references it. No
overlay references any function in this section, or the staging buffer: all
92 overlay files were scanned for literal pointers and for BL/BLX targets.

## 2. The `cell` argument

It is a **`Battles::Vector2u8`**: vptr `0x0214FDF4` (RTTI typeinfo
`0x0214FDE4`). It is 8 bytes with this layout:

| offset | width | field |
|---|---|---|
| +0 | u32 | vptr `0x0214FDF4` |
| +4 | u8 | x, in **metatile** columns |
| +5 | u8 | y, in **metatile** rows |
| +6 | 2 | padding |

One game cell is one metatile: 3×2 tiles, or 24×16 pixels. The fog grids,
the occupancy grid, entity positions (`entity+0x124`, x at `+0x128`, y at
`+0x129`) and footprints all use this same type and grid.

`Terrain_FillWindow` builds the cell at `sp+0x30` with `FUN_020F2BBC`, the
Vector2u8 constructor. It stores the vptr, then `strb x,[r0,#4]`, then
`strb y,[r0,#5]`:

- **Main loop.** `tilex = camx>>3`. The start is
  x0 = `(tilex+2)/3` (the unsigned divide at `0x0211FFC0`, which returns
  the quotient in r0 and the remainder in r1; called at `0x020A32B2`) and
  y0 = `((camy>>3) & 0xFFFE) >> 1`, which is `camy>>4`.
  - x += 1 after each metatile (`strb` at `0x020A330E`);
  - at the end of each row, y += 1 (`0x020A3326`) and x −= 10 (`0x020A332C`).
- **Left partial.** x = `tilex/3` (`0x020A334A`), y = y0, then y += 1 per row.
- **Right partial.** x = `tilex/3 + 10 + (1 if a left partial exists)`
  (`0x020A33E6`), so +10 in phase 0 and +11 in phase 2. y = y0.

The cell reaches `Terrain_DecodeMetatile` as its fifth argument (`str r0,[sp]`
at the call). It is passed through unchanged to both stamps.

---

## 3. The wall stamp — `FUN_02034B44(cell, top, bottom)`

ARM, 0x240 bytes. The steps below are in execution order.

### 3.1 `FUN_020A4524(*0x02156CB0)` — no effect

It is a Thumb getter, `ldrb r0,[r0,#0x14] ; bx lr`, and returns the map width.
Its result is overwritten by the next instruction (`mov r0, r5`), so it has no
effect and no side effect. It is probably a bounds assertion compiled down to
its getter.

### 3.2 `FUN_02034B1C(cell)` — the wall on this cell, through the occupancy grid

```
FUN_0207D79C(*0x02155224, &h, cell)   ; find a wall's handle in the cell
FUN_020872DC(&h)                      ; handle -> entity, units only
```

This is a lookup in the **map occupancy grid**, not a scan of an entity list.

**The grid.** It lives at `*(u32*)0x02155224`: 0x184 bytes, built by
`FUN_0207D4C4`, allocated by `FUN_0207D458`.

| offset | what |
|---|---|
| +0x00, +0x24, +0x48 | three **layers**, 0x24 bytes each (`FUN_0207D790`: grid + L*0x24) |
| +0x6C | a byte-array object shared by all layers. Its data pointer is at **+0x70**, with one byte per cell. |
| +0x84 | s16[128], occupant count per row (`FUN_02080680`/`FUN_0208068C`) |

**A layer** (`FUN_0207D624`):

| offset | what |
|---|---|
| +0x00 | u8, the layer bit `1 << L` |
| +0x04 | pointer to the shared array object (grid+0x6C). Its +4 is the data. |
| +0x08 | u8, the shift `2*L` |
| +0x0C | an array object of 2-byte entity handles. Its **data pointer is at +0x10**. |

**The row table** is `u16[]` at `0x02155230`, where `rowoff[y] = y * map_w`.
`FUN_0207D4C4` fills it. The cell index is `idx = x + rowoff[y]`
(`FUN_0207E16C`).

**Each cell's state byte** holds two bits per layer, at `2L`. State 1 means
exactly one occupant, whose handle is in that layer's handle array. The lookup
ignores the other states.

**The walk, `FUN_0207D79C(grid, &out, cell)`, exactly:**

```
out = null handle (*(u16*)0x0215541C, via FUN_020873C4)
if !(cell.x < map_w && cell.y < map_h)          return   ; FUN_0207D9C0, u8 compares
if s16[grid+0x84 + 2*y] == 0                    return   ; FUN_0208068C
idx = x + u16[0x02155230 + 2*y]
kind = u8[0x0214AD08]                                    ; 5, set by the static init 0x02124F6C
if (u8[ *(grid+0x70) + idx ] & u8[0x02128010 + kind]) == 0  return   ; FUN_0207E3CC; mask 0x11
for L in 0, 1, 2:
    layer = grid + 0x24*L
    mask  = u8[0x02128018 + u8[layer+0]]                   ; 0x03 / 0x0C / 0x30
    state = (u8[ *(*(layer+4)+4) + idx ] & mask) >> u8[layer+8]   ; FUN_0207D6BC
    if state != 1: continue
    hp = *(layer+0x10) + 2*idx                             ; FUN_0207D678
    if FUN_020873D0(hp):  out = *hp ; return               ; a live Wall
```

The two mask tables are in the image:

```
0x02128010  00 01 04 05 10 11 14 15   index = layer set (bit L), gives the state's low bits
0x02128018  00 03 0C 0F 30 33 3C 3F   index = layer set, gives both state bits
```

`0x0214AD08` reads 0 in the file. A static initializer (`0x02124F6C`) stores 5
there at boot, and 7 at `+1`. So the early reject tests `& 0x11`, the low
state bit of layers 0 and 2. A replica should read the byte rather than
hard-code it.

**Handle to entity.** `FUN_02087350` calls `FUN_0205C9B4`. A handle is two
bytes, `[0]` the pool index and `[1]` the serial.

```
T = *(u32*)0x02155028                     ; entity pool
if index >= s32[T+0x0C]: null
e = u32[ *(T+8) + 4*index ]
if e == 0 or u8[e+0xDD] != serial: null   ; entity+0xDC/0xDD is the entity's own handle
```

**The filters.**

- `FUN_020872DC`: a unit-class entity, meaning `u8[ *(*(e+0xE0)+4) + 8 ] == 0`.
  This is the blueprint type tag, 0 for `UnitBlueprint` (docs/11). Units,
  gates and bridges are all unit-class.
- `FUN_020873D0`: the above, and also category `u8[ *(*(e+0x160)+8) + 0x5C ] == 0x13`.

`e+0xE0` and `e+0x160` hold the same pointer. `FUN_0205D6E0` passes its r2 to
the Entity base constructor, which stores it at +0xE0, and then stores it at
+0x160 itself. It is the 12-byte blueprint object, and it holds the record
pointer at both +4 and +8 (`FUN_020010B0`, `FUN_02002B68`).

So **`FUN_02034B1C(cell)` returns the live Wall entity registered as the sole
occupant of `cell` in layer 0, 1 or 2, or 0.**

### 3.3 The visibility gate

```
02034B70  ldr r0, =0x021569E4 ; ldr r0,[r0] ; ldr r0,[r0,#0x14]
02034B80  bl  FUN_020EEC14(that, cell)   ; tail-jumps to FUN_020EEBF4(desc, cell.x, cell.y)
```

`FUN_020EEBF4` is `BitArray2D::Get` (docs/63):
`rows = *(desc+0x14); bit = (rows[y][x>>3] >> (x&7)) & 1`.

The descriptor is **`holder+0x14`, the VISIBLE-NOW grid**. `FogMap_Update`
(`0x020A15D8`, task p0x19) clears it and refills it from the local player's
vision sources every run (docs/63, docs/348). **It is not `holder+0x10`, the
explored grid.** docs/247 and `lb_fog.cpp` follow `+0x10`. Both grids are
map-sized, so band cells carry real bits: `FUN_020A146C` creates the holder
with `map_w, map_h`, and `FUN_020A12FC` allocates both grids at that size.

In game terms: **a wall is drawn only while the local player currently sees
its cell.** An enemy wall under fog-of-war is not stamped, and the ground
under it shows as plain terrain. Own walls are normally in sight.

If the bit is clear, three overrides can still open the gate. They are tested
in this order:

```
S = *(u32*)0x02150A70                  ; FUN_020092A8, the 0x21-byte rules object
u8[S+0x00] != 0   -> open    ; red brick 0 "Reveal Map" (docs/28, docs/171)
u8[S+0x1F] != 0   -> open    ; unnamed; no toggle and no writer found (docs/28)
FUN_020A223C(0x02156A24)     ; the session object, returned by FUN_020A186C
```

`FUN_020A223C(G)` is Thumb. It returns 1 when **both** of these hold:

- *(a)* The local player's *active* team equals the null team.
  - `FUN_020A2224` calls `FUN_02088144(*(G+0x64))`, which calls
    `FUN_02086DC8(*0x0215540C, id)`.
  - That walks the player list (`+8` array, `+0xC` count). It takes the player
    whose `+0x10` equals the id **and** whose `+0x9C` state is 0 or 4, and
    returns `&player+0x14`, the team.
  - Otherwise it returns a lazily-initialised copy of the null team at
    `0x02155408`.
  - The comparison is against `u8[0x02155420]` (`FUN_02087634`).
- *(b)* `u8[G+0x70] != 0`. `G+0x6C` is a `Battles::CreateGameEvent::GameParametersType`
  (vptr `0x0213B728`), so this is its byte at +4.

The reading is "an eliminated or observing local player sees everything, if
the game parameters allow it". That reading is inferred. Section 9 has the
details.

The Reveal Map test here has **no owner gate** (compare docs/28's red-brick
predicates). It is a global view override.

### 3.4 The shape — `FUN_0205E000(wall)`, and the cache at `entity+0x224`

`entity+0x224` is a **u16 connection mask**. Its bits:

| bit | neighbour |
|---|---|
| 0 | east (x+1) |
| 1 | north (y−1) |
| 2 | west (x−1) |
| 3 | south (y+1) |
| 4 | **dirty** |

The direction table is `s32[4][2]` at `0x02127FF0`:
`(1,0) (0,−1) (−1,0) (0,1)`.

The Unit constructor stores `0x10` (`strh` at `0x0205D7A8`, and in the sibling
constructor at `0x0205D698`). Every wall is born dirty.

```
v = u16[e+0x224]
if !(v & 0x10): return v                          ; clean: the cached mask
old = v & ~0x10
n = 0
for d in 0..3:                                    ; the neighbour cell, u8-wrapped
    c = Vector2u8((e.x + dx[d]) & 0xFF, (e.y + dy[d]) & 0xFF)
    FUN_0207D79C(grid, &h, c)                     ; the same wall lookup as 3.2
    if FUN_020873D0(&h) and *FUN_02087390(&h) == u8[e+0xE4]:
        n |= 1 << d                               ; a wall of the SAME team
r = n | (old & ~0x08)                             ; = n | (v & ~0x18)
if r == 0:
    for d in 0..3: if old & (1<<d): r = (d == 3) ? 2 : (1<<d); break
u16[e+0x224] = r                                  ; <-- GUEST WRITE (0x0205E11C)
return r
```

Four things about this:

- **Bits 0 to 2 are sticky.** Only bit 3 is recomputed from scratch. When
  the result would be 0, an old bit 3 turns into 2, so an isolated wall keeps
  a vertical orientation. That is the cartridge's rule, quirks included. A
  replica copies it and does not "fix" it.
- **What marks a wall dirty.** `FUN_0205E138` marks it (`ldrh`, `orr #0x10`,
  `strh`), and it has one caller: `FUN_02039DCC(view, cell)`.
  - `FUN_02039DCC` marks the four neighbouring walls dirty and sets `view+0x66`.
  - The view's message handler `FUN_020399B8` reaches it for message 0x2D
    (`FUN_02039EE8`, "an entity event whose subject is a wall") and message
    0x31 (`FUN_02039DB0`, "a cell event").
  - `FUN_0205E7F4`, the health setter, posts 0x31 whenever a wall's health
    percentage crosses 33 or 66, which is what brings the damage tiles in.
- **Nothing in the simulation reads `+0x224`.**
  - A scan of every ARM function for loads and stores at +0x224, both direct
    and through `add rX,rY,#0x200`, finds only the two constructors,
    `FUN_0205E000` and `FUN_0205E138`.
  - The Ghidra corpus agrees; its other `0x224` hits are other objects.
  - So this is view state that happens to live in the entity. With no
    simulation reader it is not a lockstep hazard. In vanilla each peer's
    value already depends on where that peer's own camera has been.
- **A band wall is never recomputed by the guest.** The guest fills only its
  own window, so a wall that has only ever stood in the band keeps its dirty
  bit. The replica computes `r` without storing it. It gets the same value
  the guest will store the first time the wall enters the native columns,
  because both start from the same stored `old` and the same neighbours.

### 3.5 The tile tables

The cell's x parity picks the table:

```
02034BD0  ldrb r1,[r5,#4] ; tst r1,#1
          ldrne r2, =0x02127DE0     ; x odd
          ldreq r2, =0x02127E40     ; x even
02034BE4  mla  r6, shape, #6, r2    ; entry = table + shape*6
```

Each table is `u8[16][6]`. The order within an entry is top row left, middle,
right, then bottom row left, middle, right. **`0xFF` means leave the terrain
tile.** The even table is the odd one with bit 0 of every byte flipped, which
is what makes the colours alternate across neighbouring cells.

| mask | links | x odd (`0x02127DE0`) | x even (`0x02127E40`) |
|---:|---|---|---|
| 0 | none | `00 01 00 / 10 11 10` | `01 00 01 / 11 10 11` |
| 1 | E | = 0 | = 0 |
| 2 | N | `FF 01 FF / FF 11 FF` | `FF 00 FF / FF 10 FF` |
| 3 | N E | `FF 01 00 / FF 11 10` | `FF 00 01 / FF 10 11` |
| 4 | W | = 0 | = 0 |
| 5 | W E | = 0 | = 0 |
| 6 | N W | `00 01 FF / 10 11 FF` | `01 00 FF / 11 10 FF` |
| 7 | N W E | = 0 | = 0 |
| 8 | S | `FF 01 FF / FF 00 FF` | `FF 00 FF / FF 01 FF` |
| 9 | S E | `FF 01 00 / FF 00 10` | `FF 00 01 / FF 01 11` |
| 10 | S N | = 8 | = 8 |
| 11 | S N E | = 9 | = 9 |
| 12 | S W | `00 01 FF / 10 00 FF` | `01 00 FF / 11 01 FF` |
| 13 | S W E | `00 01 00 / 10 00 10` | `01 00 01 / 11 01 11` |
| 14 | S W N | = 12 | = 12 |
| 15 | all | = 13 | = 13 |

A byte `b` names one tile of a 2×2 brick block:

- bit 0: the grey column (0) or the team column (1);
- bit 4 (0x10): the brick's top (0) or its side (1).

The middle-bottom byte shows the rule for a vertical run. It uses a *top* tile
when the wall continues south and a *side* tile when it ends.

### 3.6 Highlight and damage

```
02034BE8  ldrb r0,[r4,#0x71]  -> r5 = (r0 != 0) ? 4 : 0
02034BF8  ldrh r1,[r4+0x1A0]  ; current health (u16)
          ldr  r1,[*(r4+0x160)+8] ; ldrh [r1,#0x62] ; max health (u16), docs/11
02034C14  bl   __aeabi_idivmod(cur*100, max)          ; 0x0211FDB4, quotient
          lsl/asr #16                                  ; to s16
          pct < 0x21 (33) -> r5 += 0x20
          else pct < 0x42 (66) -> r5 += 0x18
```

`entity+0x71` is a **highlight flag kept by the view**:

- `Task_View_DrawField` (`0x02039148`, p0x1D) clears it for every entity
  that `FUN_020882BC` gathers from the occupancy grid inside the cull rect
  (`FUN_02038F28`) (`strbne` at `0x02039230` with `sb = 0`).
- It then sets it to 1 for every entity in two handle lists, `0x0215800C`
  and `0x02158024` (`0x020392B0`, `0x020392F4`). Each list is `+4` handles
  and `+8` s32 count.
- Those lists belong to the selection code: `FUN_020E2368` clears both, and
  `FUN_020E3078` requests a BG1 refill whenever the selection changes.
- The tiles agree: the `+4` columns are the same bricks with a cyan border.

The Wall blueprint (index 120) has 250 health. So below 33 % means health
up to 82, and below 66 % means up to 164.

### 3.7 Team colour, and the final value

```
for i in 0..5:
    b = entry[i];  if b == 0xFF: keep the terrain tile
    t = u16[0x02127DB4 + 2 * ((b & 1) * u8[e+0xE4])]     ; smulbb, then ldrh
    tile = (r5 + ((b + t) & 0xFFFF)) & 0xFFFF           ; strh, replaces the entry
```

`0x02127DB4` is `u16[] = 03C8 03C9 03CA 03CB`. The words after index 3
(`113F 005C …`) are unrelated data. Even bytes always take `0x3C8`. Odd bytes
take `0x3C8 + team`. For teams 0 to 2 this gives the tile block
**`0x3C8`–`0x3FF`**:

| offset | state | tops: grey, team 0, 1, 2 (sides +0x10) | highlighted (+4) |
|---|---|---|---|
| 0 | healthy | `3C8`–`3CB` (`3D8`–`3DB`) | `3CC`–`3CF` (`3DC`–`3DF`) |
| +0x18 | below 66 % | `3E0`–`3E3` (`3F0`–`3F3`) | `3E4`–`3E7` (`3F4`–`3F7`) |
| +0x20 | below 33 % | `3E8`–`3EB` (`3F8`–`3FB`) | `3EC`–`3EF` (`3FC`–`3FF`) |

That was checked against the art. Tiles 0x3C0–0x3FF were rendered from the
King, Mars and Pirate tilesets (`out/eur/plain/*Tileset.NCGR/NCLR`). All
three have the same block at the same indices:

- column 8 is grey;
- columns 9, 10 and 11 are **blue, red and green**, so teams 0, 1 and 2;
- columns 12 to 15 are the same bricks with a cyan selection border;
- rows 0x3E and 0x3F are the damaged versions.

A synthetic L-and-T wall run through these exact tables formed a continuous
alternating grey and red brick wall. That confirms the E/N/W/S bit order and
the parity tables. A team of 3 or more would run off the table and collide
with the highlight column (team 3, byte 0x11, offset 0x24 gives 0x400). That
is consistent with the three-player limit (docs/97).

---

## 4. The bridge-deck stamp — `FUN_0200D8A8(cell, top, bottom, tableA)`

ARM, 0xD0 bytes.

### 4.1 Why this is the bridge and not the gate

- **The category test.** Its lookup `FUN_0200D880` calls `FUN_0207D8A4`,
  which is the same grid walk as 3.2. Its filter is `FUN_020872DC` plus
  `ldrb r0,[bp,#0x5C] ; cmp r0,#0x11` (`0x0207D984`). 0x11 is 17, **Bridge**.
- **The entity factory agrees.** `FUN_0205C7A4` builds a `Sim::Bridge` for
  category 0x11 and a `Sim::Gate` for 0x12.
- **The blueprints agree.** Indices 121 to 126 are `BridgeSmall/Medium/Large`
  `H/V`, category 17. Their footprint indices (`+0x1D`) are 7, 4, 8, 5, 9, 6,
  which give (3,2), (2,3), (6,2), (2,6), (9,2), (2,9): two cells wide or tall,
  exactly what the orientation test below expects. `GateH`/`GateV` (127, 128)
  have category 18 and footprints (4,1)/(1,4). Nothing tests category 0x12
  against the tilemap.
- **The art agrees.** The metatiles it uses (below) render from `KingTiles.tbp`
  as team-coloured brick decks: blue, red and green.

### 4.2 Semantics

```
b = FUN_0200D880(cell)                     ; a live Bridge in the cell, else 0
if b == 0 or u8[b+0x231] == 0: return      ; +0x231 = construction finished
fp = FUN_02001170(*(b+0xE0))               ; footprint, see below
a  = b+0x124                               ; the anchor cell (x +0x128, y +0x129)
if fp.w > fp.h:                            ; horizontal bridge, two cells tall
    base = (cell.y == a.y) ? 0xD5 : 0xDA   ; top row : bottom row
else:                                      ; vertical bridge, two cells wide
    base = (cell.x == a.x) ? 0xD0 : 0xD1   ; left column : right column
mt  = (base + u8[b+0xE4] * 15) & 0xFFFF    ; rsb r0,r0,r0,lsl #4 = team*15
src = tableA + 2 + mt*12                   ; ALWAYS table A (map+0x338); no split test
top[0..2] = u16 src[0..2];  bottom[0..2] = u16 src[3..5]   ; all six replaced
```

**`+0x231`.** The `Sim::Bridge` constructor `FUN_020582A4` zeroes it.
`Sim::Bridge::virtual_06` (`0x02058478`) sets it to 1. That handler also
calls `FUN_0205860C`, which drives the continent code (`FUN_02057F38`,
`FUN_02057E70`) across the footprint's two ends. If `+0x232` is set, it also
takes the footprint out of occupancy layer 0 (`FUN_0207E0AC(grid, cell, 0,
bridge)`). So this reads as the "construction finished, the shores are linked"
handler, which is inferred from what it does. An ARM-wide field scan finds no
other writer, and the stamp is its only reader.

**The footprint.** `FUN_02001170(bp)` returns `0x0213AB2C + (u8[*(bp+4)+0x1D]−1)*8`.
That is a lazily built table of `Vector2u8`s, with x at +4 and y at +5,
guarded by bit 0 of `0x02150894`. The pairs are immediates in the function:

```
idx  1:(1,1)  2:(2,2)  3:(3,3)  4:(2,3)  5:(2,6)  6:(2,9)
     7:(3,2)  8:(6,2)  9:(9,2) 10:(1,4) 11:(4,1)
```

**The anchor is the footprint's top-left cell.** `Sim::Bridge::virtual_05`
and `virtual_06` walk the footprint as `anchor + (0..w−1, 0..h−1)`.

**The metatiles.** Team 0 uses 0xD0 to 0xDE, team 1 0xDF to 0xED, team 2
0xEE to 0xFC. Offsets 0, 1, 5 and 10 are the four the stamp uses. In all three
`*Tiles.tbp`, team 3's block (0xFD onward) is filler tile 0x0016. The largest
index used, 0xF8, is below the table split (440), so table A holds all of them.

This pass has **no visibility gate, no damage variant and no highlight**. It
runs after the wall pass, so where both would apply the bridge wins.

---

## 5. What the two passes write

| write | where | when |
|---|---|---|
| the six tile entries | the fill's staging buffer or its stack temporaries | always; this is the output |
| **`u16 entity+0x224`** | `FUN_0205E000` at `0x0205E11C` | a visible wall whose dirty bit is set |
| **null handle over a stale grid handle** | `FUN_02087350` at `0x02087378`, reached via `FUN_020873D0`/`FUN_020872DC` on a pointer *into* a layer's handle array (`FUN_0207D79C`, `FUN_0207D8A4`) | only if a state-1 cell holds a handle that no longer resolves |
| one-time lazy statics | `FUN_02001170` (footprint table, guard `0x02150894`, plus its destructor registrations); `FUN_02086DC8` (null-team copy, `0x02155408`) | the first call ever; long done in any battle |

`FUN_020A4524` writes nothing. Everything else in both passes is a read. The
`&out` handles in `FUN_02034B1C` and `FUN_0200D880` are stack locals, so
cleaning those up is invisible to the guest.

---

## 6. Other callers, and everything else that is tile-based

- **Callers.** `FUN_02034B44` and `FUN_0200D8A8` are called only from
  `Terrain_DecodeMetatile`. It is called only from `Terrain_FillWindow` (three
  sites). That is called only from `Task_View_StreamBG1`, which is referenced
  only as a literal in `View_RegisterTasks` (`0x02038504`). The first four
  (the two stamps, the decoder and the fill) have no literal-pool reference
  anywhere, so nothing calls them indirectly.
- **The helpers are used elsewhere, but not for drawing:**
  - the wall lookup `FUN_0207D79C` is also used by `FUN_02039DCC` (dirtying
    the neighbours) and by `FUN_0205E000`;
  - the bridge lookup `FUN_0207D8A4` is also used by `FUN_020A34E8`, which
    finds the nearest *bridge site* from the map's site list at
    `map+0x2C8`/`+0x2CC`. The construction commands use it.
- **How each class is drawn** (`View_DrawEntities`, `0x0203A9E8`):
  - `FUN_02036CA0` sends everything **except** categories 5, 6 and 19 to the
    billboard builder `Unit_BuildBillboard`.
  - `FUN_02036CD0` sends categories 5 and 6 (transports and siege) to the 3D
    model path.
  - So **walls are drawn only as BG1 tiles.** Bridges get a billboard *and*
    the deck stamp. Gates get a billboard only. Castles, towers and other
    buildings are billboards. That is why the 0/768 diff of docs/203 was true
    on its runs, which had no wall and no bridge on screen, and why the castle
    stays in the band.
- **The metatile grid itself changes.** `FUN_020A4204` rewrites a cell's
  metatile index; it is called from three places, and the likeliest is
  harvesting (inferred). That is not a stamp. The band already follows it,
  because it reads the grid live.

So the complete list of BG1 content is: the metatile decode, walls, and
finished bridges.

---

## 7. The host replica for the band

The replica reads guest memory only. It never calls guest code or writes guest
memory. Names and addresses:

```
occupancy grid   *(u32)0x02155224    row offsets  u16[] 0x02155230
layer-kind byte  u8   0x0214AD08     mask tables  u8[8] 0x02128010, 0x02128018
entity pool      *(u32)0x02155028    (+8 entity*[], +0xC s32 count)
fog holder       *(u32)0x021569E4    (+0x14 visible BitArray2D: +8 w, +0xC h, +0x14 rows)
rules object     *(u32)0x02150A70    (u8 flags; +0x00 Reveal Map, +0x1F)
session          0x02156A24          (+0x64 local player id, +0x70 u8)
player manager   *(u32)0x0215540C    (+8 player*[], +0xC count; player +0x10 id, +0x14 team, +0x9C state)
null team        u8   0x02155420
wall tables      0x02127DE0 (x odd), 0x02127E40 (x even), u8[16][6]
wall team base   u16[4] 0x02127DB4   directions s32[4][2] 0x02127FF0
selection lists  0x0215800C, 0x02158024  (+4 u16 handles*, +8 s32 count)
footprints       0x0213AB2C, 8-byte Vector2u8 entries (or the immediates in 4.2)
```

```cpp
// All reads. Validate every pointer as main RAM before dereferencing, as
// lb_widemap does; treat any failure as "no stamp".
uint32_t resolve(uint16_t h) {                      // FUN_02087350, minus its write
    uint32_t T = rd32(0x02155028);
    unsigned i = h & 0xFF, serial = h >> 8;
    if ((int32_t)i >= (int32_t)rd32(T + 0xC)) return 0;
    uint32_t e = rd32(rd32(T + 8) + 4 * i);
    return (e && rd8(e + 0xDD) == serial) ? e : 0;
}
bool unit(uint32_t e)       { return rd8(rd32(rd32(e + 0xE0) + 4) + 8) == 0; }
int  category(uint32_t e)   { return rd8(rd32(rd32(e + 0x160) + 8) + 0x5C); }

uint32_t occupant(int x, int y, int want_cat) {     // FUN_0207D79C / FUN_0207D8A4
    if (x < 0 || y < 0 || x >= map_w || y >= map_h) return 0;
    uint32_t G = rd32(0x02155224);
    if ((int16_t)rd16(G + 0x84 + 2 * y) == 0) return 0;
    uint32_t idx = x + rd16(0x02155230 + 2 * y);
    uint8_t  st  = rd8(rd32(G + 0x70) + idx);
    if (!(st & rd8(0x02128010 + rd8(0x0214AD08)))) return 0;
    for (int L = 0; L < 3; ++L) {
        uint32_t layer = G + 0x24 * L;
        uint8_t m = rd8(0x02128018 + rd8(layer));
        if (((rd8(rd32(rd32(layer + 4) + 4) + idx) & m) >> rd8(layer + 8)) != 1) continue;
        uint32_t e = resolve(rd16(rd32(layer + 0x10) + 2 * idx));
        if (e && unit(e) && category(e) == want_cat) return e;   // stale handle: skip, as the guest does
    }
    return 0;
}

bool gate_open(int x, int y) {
    uint32_t V = rd32(rd32(0x021569E4) + 0x14);      // VISIBLE grid; not +0x10
    if ((rd8(rd32(rd32(V + 0x14) + 4 * y) + (x >> 3)) >> (x & 7)) & 1) return true;
    uint32_t S = rd32(0x02150A70);
    if (rd8(S) || rd8(S + 0x1F)) return true;
    if (!rd8(0x02156A24 + 0x70)) return false;
    uint32_t M = rd32(0x0215540C), id = rd32(0x02156A24 + 0x64);
    for (int i = 0; i < (int32_t)rd32(M + 0xC); ++i) {
        uint32_t P = rd32(rd32(M + 8) + 4 * i), s = rd32(P + 0x9C);
        if (rd32(P + 0x10) == id && (s == 0 || s == 4))
            return rd8(P + 0x14) == rd8(0x02155420);
    }
    return true;                                     // no active local player: null team
}

uint16_t wall_shape(uint32_t e) {                    // FUN_0205E000, minus its store
    uint16_t v = rd16(e + 0x224);
    if (!(v & 0x10)) return v;
    uint16_t old = v & ~0x10, n = 0;
    int px = rd8(e + 0x128), py = rd8(e + 0x129);
    for (int d = 0; d < 4; ++d) {
        int nx = (px + (int32_t)rd32(0x02127FF0 + 8 * d)) & 0xFF;
        int ny = (py + (int32_t)rd32(0x02127FF0 + 8 * d + 4)) & 0xFF;
        uint32_t o = occupant(nx, ny, 0x13);
        if (o && rd8(o + 0xE4) == rd8(e + 0xE4)) n |= 1 << d;
    }
    uint16_t r = n | (old & ~0x08);
    if (!r) for (int d = 0; d < 4; ++d) if (old & (1 << d)) { r = (d == 3) ? 2 : (1 << d); break; }
    return r;
}

bool highlighted(uint32_t e) {                       // what DrawField would store at +0x71
    for (uint32_t L : {0x0215800Cu, 0x02158024u})
        for (int i = 0; i < (int32_t)rd32(L + 8); ++i) {
            uint32_t h = resolve(rd16(rd32(L + 4) + 2 * i));
            if (h == e && unit(h)) return true;
        }
    return false;
}

void stamp(int x, int y, uint16_t t[6], uint32_t tableA) {
    if (uint32_t e = occupant(x, y, 0x13); e && gate_open(x, y)) {       // walls
        uint32_t ent = ((x & 1) ? 0x02127DE0 : 0x02127E40) + 6 * wall_shape(e);
        uint16_t off = highlighted(e) ? 4 : 0;          // or rd8(e+0x71), see 8.2
        uint16_t mx = rd16(rd32(rd32(e + 0x160) + 8) + 0x62);
        int16_t pct = mx ? (int16_t)((int32_t)(rd16(e + 0x1A0) * 100) / (int32_t)mx) : 100;
        off += pct < 33 ? 0x20 : pct < 66 ? 0x18 : 0;
        int team = rd8(e + 0xE4);
        for (int i = 0; i < 6; ++i) {
            uint8_t b = rd8(ent + i);
            if (b == 0xFF) continue;
            uint16_t tb = rd16(0x02127DB4 + 2 * ((b & 1) * team));
            t[i] = (uint16_t)(off + (uint16_t)(b + tb));
        }
    }
    if (uint32_t b = occupant(x, y, 0x11); b && rd8(b + 0x231)) {        // bridges
        uint32_t fp = 0x0213AB2C + 8 * (rd8(rd32(rd32(b + 0xE0) + 4) + 0x1D) - 1);
        bool horiz = rd8(fp + 4) > rd8(fp + 5);
        int base = horiz ? (y == rd8(b + 0x129) ? 0xD5 : 0xDA)
                         : (x == rd8(b + 0x128) ? 0xD0 : 0xD1);
        uint32_t src = tableA + 2 + 12u * (uint16_t)(base + 15 * rd8(b + 0xE4));
        for (int i = 0; i < 6; ++i) t[i] = rd16(src + 2 * i);
    }
}
```

The max-health guard is the replica's own addition. A wall's max health is
250, so it never fires in practice.

**Where it goes.** Call `stamp(mx, my, m.t, table_a)` in `metatile_tiles()`,
after `expand_metatile()` returns. Do not put it in `expand_metatile()`: the
shroud calls that with a fixed index, and a stamp there would be wrong. From
`metatile_tiles()` it reaches four consumers:

1. **The band fill** (`g_ext`), which is the defect.
2. **The camera check `sample_mismatch()`.** Today a stamped tile on one of
   its 48 native sample cells fails every candidate. This was ground-truthed
   live as the bottom band drawn one metatile row off. The
   check could instead skip cells where `occupant()` finds a wall or finished
   bridge, which keeps it immune to the one-run lag in 8.4.
3. **The `[lb_terrain] mine vs guest BG1` audit** (`LB_SCANLOG=1`). This
   becomes the acceptance test. With walls or a bridge on screen it should
   read 0/768, where today it would list `3C8`–`3FF` tiles.
4. **`lb_widemap_metatile()`**, which `lb_field.cpp`'s host terrain uses. That
   file also has its own decode (`metatile_src`), which would need the same
   call.

**Cost.** Most cells are rejected by one row-count read and one state-byte
read. A wall adds two short list scans. Only a dirty wall adds four neighbour
walks. Per band cell per frame this is small next to the existing fills.

**Scenes to grade:**

- the campaign tutorial's wall (the reported case);
- a wall line crossing the seam;
- a selected wall (cyan tiles);
- a wall at or below 164 and 82 health;
- an enemy wall in fog-of-war, which must stay bare terrain;
- the same with `LB_CHEATS=0` (Reveal Map), which must draw it;
- a finished bridge, horizontal and vertical. The bridge missions
  (`CampaignMission2_BridgeTheIslandsAct3`, `CampaignMission6_TheBridgeAct1`)
  are the obvious places.

---

## 8. What can make the replica differ from the cartridge

### 8.1 The visibility gate at band cells

The visible grid covers the whole map and is rebuilt every run from all of the
local player's vision sources, so band cells carry true bits. The two ways to
get it wrong:

- **Reading the explored grid.** `lb_fog.cpp` follows `holder+0x10`, which
  would draw enemy walls under fog-of-war.
- **Inferring the overrides from the picture.** The band fog does this
  (`g_fx_reveal`). For walls, read the three overrides directly.

`lb_widemap`'s `fx_read_record()` already reads both grids for the whole map
each call. `g_fx_state == 2` is exactly the visible bit, so the gate can reuse
it when the record validated this frame.

### 8.2 Entities outside the guest's window

The occupancy grid, the pool, health, team and `+0x231` are simulation state
and are valid across the whole map. Two fields are **maintained by the view,
and only where it looks**:

- **`+0x224`** is recomputed only for walls the guest's own fill reaches. The
  pure recompute in `wall_shape()` handles band walls. Its result is what the
  guest will store later, because both start from the same `old`.
- **`+0x71`** is cleared only for entities inside the gather rect, but set
  for every listed entity. A wall deselected while outside the rect keeps a
  stale 1. The port widens the rect (`lb_patches.cpp`, `kCullImmAddr` and
  `kCullMinAddr`, plus a vertical twin by its comments), so inside it `+0x71`
  is fine. The
  list test in `highlighted()` is exact everywhere, and it is what the guest
  would compute for a wall inside the rect.

### 8.3 Stale handles

The guest overwrites a stale grid handle with the null handle and treats the
cell as empty. The replica skips it and gets the same answer without the
write.

### 8.4 Timing

This is the one real lag. The guest stamps at p0x23 of run N, and p0x01 of run
N+1 uploads that. The host reads live state at compose time, which can be one
run newer. Every input that changes a stamp also triggers a guest refill:

- **vision changes:** `holder+0x24` makes DrawField set `view+0x66`;
- **wall events:** message 0x2D (an entity event whose subject is a wall) and
  message 0x31 (a cell event). Which game events post each was not traced
  beyond the health setter;
- **a wall crossing 33 % or 66 % health:** message 0x31;
- **bridge health changes during construction:** message 0x31, for
  blueprints 121 to 126;
- **selection changes:** `FUN_020E3078`;
- **a camera tile step:** `View_CommitCamera`.

So the native columns are at most one run behind the band, and only on the
frame of a change. If that single-frame seam matters, there is a way to get
the guest's exact inputs:

- `Task_View_StreamBG1` has zero direct callers, so it is overridable.
- The host would run its band stamp inside the override **before**
  tail-calling the original, only when `view+0x66` is set. That follows the
  "one lifted call, last" rule for this port's overrides.
- At that moment the state is exactly what the guest's fill will see.

### 8.5 Edges and garbage

- Cells off the map get no stamp. The guest's `u8` compare rejects them, and
  x−1 at column 0 wraps to 255.
- A team of 3 or more reads past the four-entry team table, and a bridge team
  of 3 or more reads filler metatiles. Read the tables from guest memory
  rather than hard-coding them, and the replica reproduces whatever the
  cartridge would draw.

### 8.6 Reading at vblank

`lb_widemap`'s header notes that `0x02156CB0` can read 0 in the vblank hook,
which is why `lb_field_map_chain()` exists. This doc did not check whether
`0x02155224`, `0x02155028` or `0x021569E4` behave the same way. Validate them
every frame, or capture them where the map chain is captured.

---

## 9. Confidence

| item | source | confidence |
|---|---|---|
| call chain, argument order, order of the two passes | disassembly | certain |
| the `cell` type and layout, where Terrain_FillWindow builds it | disassembly and RTTI | certain |
| `FUN_020A4524` has no effect | disassembly (result overwritten) | certain |
| the occupancy walk: grid layout, masks, state==1, handle resolve, filters | disassembly; the `0x0214AD08`=5 static init read | certain for the walk; the *meaning* of layers 0 to 2 and of states 2 and 3 is not established |
| the gate uses the visible grid `+0x14` | disassembly; the visible/explored naming from docs/63 and docs/348 | certain |
| override flags `+0x00` Reveal Map and `+0x1F` | disassembly; the name from docs/28 and docs/171 | certain that they are read; `+0x1F` has no writer, found or named |
| `FUN_020A223C` = "eliminated or observing local player, and a game parameter" | disassembly for the test; the meaning is **inferred** | medium |
| the shape recompute, including sticky bits, the fallback, same-team links and the direction table | disassembly and a table read | certain |
| `+0x224` has no simulation reader | an ARM-wide field scan plus the corpus; the Thumb register-offset forms are covered only by the corpus | high |
| shape tables, team table, tile block `0x3C8`–`0x3FF` | table reads, plus a render of three tilesets | certain |
| `+0x71` = selection highlight | disassembly of DrawField; the cyan tiles; the list owners **inferred** as selection | high for the mechanism, medium for which list is which |
| damage thresholds and arithmetic | disassembly | certain |
| pass 2 is the **bridge** deck, not the gate | category compare, factory, blueprints, footprints, rendered metatiles | certain |
| `+0x231` = construction finished | the writers (ctor 0, `Bridge::virtual_06` 1); the meaning **inferred** from what `virtual_06` does | high |
| gates, castles and buildings are billboards; walls have no 3D | the `View_DrawEntities` predicates | high (not checked live) |
| no other BG1 writer; no overlay involvement | xref scans of the image and all 92 overlays | high |
| the timing lag and the refill triggers | disassembly of the triggers; the task order from the renderer audit | high |
| the camera-check failure | the code, plus the live ground truth at camera y 528 | high |
| the replica code | written from the above; **not compiled, not run** | untested |

## Not settled

- **What `rules+0x1F` is.** Ten functions read it, all in the same reveal
  pattern next to `+0x00`. The rules dispatcher cannot toggle it (docs/28).
  No store to it was found: none among the image's callers of the getter
  `FUN_020092A8`, and no overlay calls the getter at all. The constructor's
  0x21-byte clear is the only write seen. It is probably a development switch
  that stays 0 in retail.
- **What `session+0x70` (GameParameters `+4`) and player states 0 and 4
  mean.** The override is copied exactly, so the replica does not need the
  names.
- **Which occupancy layer holds a wall, and which holds a finished bridge**
  (the bridge leaves layer 0 on completion). The replica runs the same
  three-layer walk, so it inherits whatever the guest does.
- **Which selection list is which** (`0x0215800C` or `0x02158024`).
- **Whether the entity globals read null at vblank** (8.6).
- **Whether sticky connection bits ever show on screen.** This is a property
  of the cartridge, not something the port should change.
- The retraction comment in `lb_widemap.cpp` says "`func_0200D8A8` (gates)".
  It should say **bridges**. This doc does not change it.
