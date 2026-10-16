# 348 — One camera per frame: pairing the layers by the camera the game drew them with

Written for: whoever picks this up next.

Status: **built, measured and deployed** -- `build/LegoBattles.exe`,
2026-09-23 08:25, `e7a9316e` (sha256 `1b540944...`, the player's
settings.json kept). The tables at the end say what each change was graded
against.

Frame budget of the deployed build, no dumps, quiet machine, the same pan:
512x384 59.6 fps (emulation 14.5 ms, compositor 1.3 ms); a
1473x830 window 51.4 fps (emulation 16.5 ms, compositor 2.0 ms). The
compositor is not what stands between this and 60 at that size; the
emulation is (20-22 ms late in a recorded battle, ~66% of it the game's own
recompiled ARM9 code).

> **CORRECTED 2026-09-23 (docs/349).** The emulation was the obstacle, but
> not the game's code: that "ARM9 share" included the port's own overrides,
> and a quarter of the main thread was one of them (`lb_dispatch_original`)
> scanning the dispatch tables on every call. Fixed there; the recorded
> battle now runs at the 60 fps cap. Not graded by any instrument, and left to
hands-on testing: the selection against the drawn box (item 3 -- the decomp causes
are fixed and all 43 patched sites verified applied, but no per-unit test ran),
and the clip-space vertical band, which the available recordings never put
world geometry under (no regression on 25 north-edge frames of the recorded
play session, and no frame that could show the fix either).

## What was wrong, in one paragraph

The widened field is assembled from four sources: the guest's own 256x192 BG
columns (VRAM), the widened BG columns this port builds from the map
(`g_ext`, `g_ext2`), the 3D raster (units, buildings, effects), and the HUD
moved out to the window edges. Each of them was taken at a different moment
of the game's update cycle, so while the camera moved they carried different
cameras: sprites shuddered 16 px against the ground they stand on (docs/342),
the widened columns showed a different camera from the native columns next to
them (the "original-aspect region flickers" of docs/343 item 3), and fog in the
widened columns was drawn from a two-state approximation of the game's
three-state fog (docs/324 item 13, shroud reappearing even in explored
areas). This document replaces "which moment" guesses with the camera itself:
every layer is tagged with the camera the game drew it with, read from the
game's own view object at the moment the game hands the layer over, and the
composite uses layers whose tags agree.

## 1. Why the 3D ran one update ahead of the ground (decomp)

One game update is one task list:

| prio | task | what it does with the camera |
|---|---|---|
| 0x01 | `Task_View_CommitBGs` 020396e0 | writes BG1/BG2 scroll from `view+0x5E/+0x60` **as it stands now** and DMAs the tilemaps staged last update |
| 0x15 | `Task_View_Update` 0203942c -> `View_CommitCamera` 020398b8 | commits the new camera C(k) |
| 0x1D..0x20 | draws | units/buildings at (camx-4, camy), effects at (camx, camy) -- all C(k) |
| 0x23 | `Task_View_StreamBG1` 020396ac | stages BG1 for C(k) |
| 0x25 | `Task_SwapBuffersAndOam` 02024d08 | SwapBuffers: the C(k) list latches at the next VBlank |

The ground for C(k) is committed by task 0x01 of the **next** update. That
pairs the two only if the next update starts at the very next VBlank. NitroMain
(020f6730) decides that, and the disassembly is unambiguous:

    020f68d0  bl 020f2648        ; t0 = milliseconds
    020f68f8  bl 0210fb24        ; OS_WaitVBlankIntr
    020f6900  bl 02089e60        ; TaskList_Run
    020f6904  bl 020f2648        ; t1
    020f6910..020f6928           ; if (t1 - t0) < 20: OS_WaitVBlankIntr again

An update that finishes under 20 ms waits a **second** VBlank, so its 3D list
is on screen for one frame over the previous update's ground. With the game's
update time the loop settles into two updates in three VBlanks, and one frame
in three shows the sprites one camera step ahead. On a real DS a busy battle
overruns 16.7 ms and the pairing holds; this port's recompiled guest is fast
enough to take the second wait.

**Measured, from the tags** (`LB_PAIRLOG=1`, 512x384, a D-pad pan):

    frame 245  ground 137,144  latest 3D 139,144  previous 3D 137,144 -> previous
    frame 246  ground 139,144  latest 3D 139,144                      -> latest
    frame 247  ground 143,144  latest 3D 143,144                      -> latest
    frame 248  ground 143,144  latest 3D 151,144  previous 3D 143,144 -> previous
    ...                         (the same three-frame pattern for the whole pan)

Over the run: latest raster 7,272 frames, previous raster 54, **no match 0**.
The camera is pixel-precise (137, 139, 143, 151, 167, 183 ... the D-pad pan
accelerates to 16 px per update), and every frame had a raster drawn with
exactly the ground's camera.

## 2. The changes

All in the widened composite only; vanilla (`--adaptive-widescreen none`) does
not run any of it.

1. **Camera tags.** `lb_tag_on_swap` (GPU3D command 0x50, SwapBuffers
   executing) and `lb_tag_on_bg1` (the BG1 scroll write of task 0x01) read
   `view+0x5E/+0x60`. The swap tag is promoted with its list at the VBlank
   latch and stored beside the 3D snapshot.
2. **Pairing** (`lb_pair_pick`). The composite chooses the live raster or the
   snapshot, whichever carries the ground's camera, and takes that raster's
   band with it. `LB_PAIRCAM=0` restores the old rule. Before the rows fan out,
   the composite waits once for the live raster on the main thread: the soft
   renderer's per-scanline semaphore posts once at the end of a frame, and
   several worker threads blocked in `GetLine` would leave all but one blocked.
3. **Widened tables built for the ground's camera** (`lb_widemap_compose_prep`),
   at composite time, from the same VRAM the native columns are decoded from.
   The tag is checked against the BG1 tilemap first (the 48-cell test
   `choose_camera` uses, zero tolerance); if it fails, `choose_camera` is asked
   against the same VRAM; if that fails the last tables stand. The learners
   (`scan_shroud`, `learn_fog_edges`) stay at scanline 191. `LB_COMPOSEFILL=0`
   restores the old placement.
4. **Bounded fills.** Both fills walked all 73,809 cells of a table sized for a
   1728x768 surface on every frame the camera moved. They now walk only the
   cells the current surface can ask for (`bounds_for_surface`, derived from
   the decoder's own column and row arithmetic), and the terrain expands each
   metatile once instead of three times.
5. **The band a list is placed by comes from that list's camera**
   (`list_relatch_x/y`), latched at the list's first vertex, which comes after
   its update's camera commit. It used to be the camera read at the previous
   VBlank, a pan step behind at a map edge.
6. **The vertical band moved into clip space** (`LB_CLIPY=0` restores). It was
   applied after the clipper, which had already cut a frustum centred on the
   camera: at the north edge of a tall surface the bottom `(H-192)/2` rows got
   no 3D at all; at the south edge the top rows went empty and polygons pushed
   past the surface were dropped. The horizontal axis was moved for exactly
   this reason in docs/268; this is its twin. The clip matrix is also marked
   dirty when the render height changes.
7. **The cartridge's own fog rule in the widened columns** (section 3).
8. **Command icons punch the fog in the widened columns** (section 4).
9. **Touch takes off the band of the frame on screen**, not the band latched
   at the last VBlank, so a click or drag corner lands where it is aimed near
   a map edge while the camera moves.
10. **HUD mover told both bands**: the polygons it reads belong to the live list;
    on a frame that composites the previous raster they are converted with the
    list's band and placed with the raster's.
11. **The per-vertex `getenv("LB_VIEWPORTLOG")`** in the viewport transform is
    now read once (code audit P2).

## 3. The fog, reproduced from the cartridge's rule

`Fog_FillWindow` (02039fb4) builds BG2 a metatile at a time. `Fog_NeighbourCode`
(0203a318) gives the metatile and its N, W, E, S neighbours a state each --
**2** visible (the record's `+0x14` bit grid, or everything under a reveal
override), **1** explored (`+0x10`), **0** neither or off the map -- and
`Fog_CodeToTiles` (0203a460) maps the 10-bit code through the byte table at
0x0213C8FC to one of 48 six-tile sets at 0x0213C7DC. Read out of the running
game's static data:

* a **visible** centre is six clear tiles (0x64), whatever its neighbours;
* an **explored but unseen** centre is the fog-of-war set (0xF5, with edge
  variants EB..FF and AC..B0);
* an **unexplored** centre is the cloud (C2 C0 C1 / BD BB BC), with edge
  variants chosen by whether each cardinal neighbour is explored or visible.

The port's widened fog knew one bit (explored) and learned edge tiles by
watching; it drew explored-but-unseen ground **clear** where the cartridge
beside it draws fog-of-war. That is docs/324 item 13 exactly: ground drawn
clear in a widened column acquires the cartridge's fog-of-war the moment a pan
brings it into the native columns.

The port now evaluates the same rule on the same record and tables
(`fx_tiles`), read live -- nothing is copied from the ROM -- and grades itself
against the guest's own BG2 window every frame (`fx_grade`, `LB_FOGEXACTLOG=1`):

    768 cells a frame, 4,193,280 graded: 0.49% disagree -- every one of them a
    cell the cartridge drew CLEAR where the rule predicts fog, i.e. the UI
    holes of section 4, which the rule deliberately does not include.

The reveal override (`bVar2` in `Fog_NeighbourCode`: two cheat/debug flags at
`*(0x02150A70)` +0 and +0x1F, or an ownership test through FUN_020a223c) is
not reimplemented; the grade reads which branch the cartridge took from its own
window (plenty of fog predicted, none drawn, three frames running).
`LB_FOGEXACT=0` falls back to the older path.

## 4. Command icons under the fog (item 5)

The command icon is the SpriteUIEngine element at 0x0215760C, drawn every
frame from task 0x1C through FUN_0203393c, which calls `SpriteUI_MarkCells`
(0203420c) for every piece it draws. That marks the 8x8 screen cells the quad
covers -- `cx = (x-4)>>3 .. (x-4+w)>>3`, `cy = y>>3 .. (y+h)>>3`, both ends
exclusive -- clamped to the DS's 32x24; `Fog_ApplyUiCellMask` (0203a4b4) then
writes the clear tile into BG2 at column cx, row `cy + ((camy>>3)&1)`. The icons
are 3D quads under the fog layer (BG2 priority 1, 3D priority 2), so that hole
is the only reason they show on a DS. The clamp dropped every cell outside the
native screen, so in the widened columns the icon was under the fog.

`SpriteUI_MarkCells` has direct callers and cannot be overridden, so the port
rebuilds the cells from the icon's quads in the render list (depth 4,275,712,
the icon's z of 502) with the same formula and no clamp, keeps only the cells
outside the native 32x24 (the cartridge punched the rest), and writes the clear
tile into the widened fog there. Trace: the icon object is constructed by
FUN_02125328 -> FUN_02032f50(0x0215760C) with vtable 0x02144AA4 (slot +8 =
0x0203393C) and drawn from task 0x1C (FUN_020e4b0c, registered by 020e40dc)
through FUN_020dd708 -> FUN_020a89fc -> FUN_0203393c; every draw mode of
FUN_0203393c calls `SpriteUI_MarkCells`, and FUN_02033404 (run on every setup)
stores 0 at element+0x7A, the flag that would suppress marking.

### 4b. The "revealed rectangles in the shroud" are the HUD's own holes

Parked in cloud, two small rectangles of ground showed through the fog at the
left edge of the native columns (16x48 and 16x24 px, native x 4..20, rows
16..64 and 168..192). Those are exactly the cells the same mechanism punches
under the HUD elements at their native positions: the player markers
top-left and the deselect button bottom-left. On a DS each element sits over
its own hole. This port moves the elements to the window's corners, and the
hole stayed behind.

The first version rebuilt the punched cells from the moved quads and closed
only half of each hole: the cartridge marks a piece's cells from its LOGICAL
size, and the rasterised polygon runs a pixel short of it (pieces at native x
3..12 and 12..19 mark column 0 alone by the polygon; the cartridge punched 0
and 1). So the port now reads the holes themselves: Fog_ApplyUiCellMask
keeps a copy of exactly the grid it punched with (BitGrid_Alloc at
view+0x164, data at +0x174, row table at +0x178), the same copy
Task_View_DrawField compares against. A cell is refilled when it is in that
copy AND within a cell of an element the port moved AND not within a cell of
an unmoved element or a command icon. `lb_widemap_fog` draws those cells --
the only native cells the port ever substitutes -- from the exact fog rule's
table, which holds what the cartridge would draw there without the hole.

Verified on the frame that showed the rectangles (camera parked in cloud at
the map's south edge, 768x1024): uniform cloud, the moved HUD drawn over it.

### A regression caught before it shipped

With the tables built only by the composite, **vanilla** lost its leftmost 4
columns to black: the native decode also uses the tables, for the one-tile
sliver the scroll bias samples outside the cartridge's 32 columns, and in
vanilla the composite never runs. The scan now builds the tables itself
whenever the composite has not in the last two frames. Checked against the
previous day's vanilla capture of the same route: the field is
**byte-identical on all four frames** (0 of 49,152 pixels differ); the top
screen differs only in the three animated resource-bar icons.

## 4c. Item 3: what the selection reads (decomp trace)

Two causes, both confirmed in the disassembly:

1. **The current corner's row was still a byte.** The rectangle is built by
   FUN_020AA8E4 from a stack slot it reads back with LDRSH (020AA8F8); the
   drag-in-progress path (020AF1BC, FUN_020AF15C -> preview set 0x0215800C)
   and the pen-up path (020AF310, FUN_020AF23C -> the selection 0x02158024)
   load that slot's row with `ldrb r0,[r5,#9]`. The press corner and both x
   values were already on the 16-bit shadows; this was the one axis left, so
   a drag reaching outside the native rows selected against a row wrapped
   modulo 256 -- and, after the docs/344 crash clamp, against 0 or 191. Moved
   to the +0x12 shadow (kOrderFix group 128), with the four remaining readers
   of the same byte in these handlers (020AF21C, 020AF3A6, 020AF1D8,
   020AF36A). docs/344's "selection is unaffected" was wrong when written and
   is corrected in place in lb_patches.cpp.
2. **The drag edge-scroll fired in the middle of the picture.** While a drag
   is in progress Task_Input (020ABB0C) scrolls the camera 8 px a frame when
   the pen is within 8 px of the DS screen's edge (020ABC20..020ABCA0). In the
   widened window the panel is given a clamped position, so a pen anywhere in
   the widened columns or rows read as "at the edge" and the camera scrolled
   under the drag. The game's rectangle keeps its press corner fixed in the
   world, so it grew away from the box on screen. The two loads now read
   proxy bytes (recogniser +0x16/+0x17) that say "at the edge" only within 8
   px of the SURFACE's edge -- the edge of what the player sees.

And the port's own yellow box now anchors its press corner in the world, as
the game's does, so when the camera does scroll the two stay together.

## 5. Results

### The pixel gate (tools/lb_layershift.py), same binary, 512x384, same pan

| arm | exactly 0 | within 1 px | median | p90 | max |
|---|---:|---:|---:|---:|---:|
| `LB_PAIRCAM=0` (the old rule) | 21.1% | 36.8% | 4 | 16 | 16 |
| pairing by camera | **78.6%** | **92.9%** | **0** | **1** | **2** |

The residual 1-2 px appears at the same frames in both arms (animated sprites
moving under the overlay match), not as a layer offset.

### The pairing on every run

| run | surface | frames on the latest raster | on the previous | no match |
|---|---|---:|---:|---:|
| harness pan | 292x218 | 7,272 | 54 | 0 |
| harness pan | 1096x616 | 6,108 | 20 | 0 |
| harness pan | 1462x616 | 5,520 | 7 | 0 |
| recorded play session replayed | 842x474 | 7,439 | 103 | 0 |

### The fog rule against the cartridge's own window

Every frame, 768 native cells, the rule's tile against the guest's BG2 in
VRAM (`LB_FOGEXACTLOG=1`), excluding the cells the cartridge punched for UI
sprites (its own snapshot of the grid at view+0x164):

| run | cells graded | disagree |
|---|---:|---:|
| south-edge pan, 438x584 | 4,090,141 | 52 (0.001%) |
| recorded play session | 5,567,076 | 4,501 (0.08%) |

Split by the metatile's own state, the recorded play session first agreed 99.998%
on visible and 99.94% on cloud, but only 47% on fog-of-war cells. Splitting
frames whose record had been stable for three frames from the rest settled
it: on stable frames fog-of-war agreed 99.7%. The window lags the record by
an update -- Task_View_DrawField builds the BG2 staging after an update's
simulation and Task_View_CommitBGs DMAs it at the start of the next, before
that update's simulation -- so a record read live ran the widened fog an
update AHEAD of the native fog beside it. The record is now taken at the BG2
scroll write (the commit), and the grade after that:

| run | cells graded | disagree |
|---|---:|---:|
| south-edge pan, 438x584 | 4,181,595 | **0** |
| recorded play session | 5,700,041 | 852 (0.01%) |
| 512x384 pan, final build | 4,520,532 | 420 (0.01%) |

with visible 100% and fog-of-war 99.8% on the recorded play session. The residue is
cloud cells in the native columns only (which the port does not draw): at the
map's east edge Fog_FillWindow skips its right partial column when the window
runs past the map (`uVar1 + 0x20 < width*3`), leaving the cartridge's own
staging stale there.

### Ultra-wide (2560x1080 window, 1462x616 surface)

The two defects docs/342 reproduced here -- terrain showing through the cloud
in stripes every ~16 px, and a ~256 px hard-edged revealed rectangle -- are
gone: the cloud across the unexplored widened area is uniform with the
cartridge's own rounded edges. Both came from the port's window-derived fog
record and learned edge tiles, which the exact rule replaces.
