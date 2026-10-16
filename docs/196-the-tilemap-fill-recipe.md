# 196 — The tilemap fill, and how to widen the field without touching the DMA

Status: **reverse engineered from the cartridge, not yet implemented.** This is
the recipe for the one thing standing between the port and the widescreen it
targets: the black pillarbox either side of the field.

## Why the pillarbox exists

The guest maintains tile entries for exactly the 256 columns it draws. Widening
the raster past that shows nothing, which docs/193 confirmed by measurement —
`LB_BG_EXTEND=1` changes not one pixel, because the tiles out there are empty.

## The call chain

    func_020396AC (view)                     the filler, gated on the dirty
      ldrb r1, [r0, #0x66]                   flag at view+0x66
      ldr  r0, =0x02156CB0 ; ldr r0, [r0]    the level/map object
      ldrsh r2, [r0, #0x5E] / r3, [r0,#0x60] camera x, y
      blx  func_020A3234                     fill(map, dest, camx, camy)

    func_020396E0 (view)                     the scroller, next frame
      func_0202D1B4(layer, x, y)             writes BGxOFS at 04000010/14/18
      DMA 0x680 bytes from 021523A0          32 columns x 26 rows of u16
      DMA 0x680 bytes from 02152A20          the second layer

## func_020A3234 — the fill loop

Thumb. Arguments `(map, dest, camx, camy)`.

    r2 >> 3                       camera tile x          -> sp+0x2C
    (r3 >> 3) & 0xFFFE            camera tile y, even    -> r5
    func_0211FFC0(tilex, 3)       tile x mod 3           metatiles are 3 wide
    switch on that remainder      selects the phase and the partial-column case

    sp+0x18 = 12, +1 if (camy & 0xF)          METATILE ROWS
    inner:  cmp r7, #0xA ; blt                METATILE COLUMNS = 10
            r4 += 6      per metatile         3 tile entries, u16
    outer:  r4 += 0x44   per metatile row     10*6 + 68 = 128 = 2 rows x 32

10 metatiles x 3 tiles = 30, plus the 1–2 column partial the switch handles =
**32 columns**; 13 x 2 = **26 rows**. 32 x 26 x 2 = 0x680, which is exactly the
DMA length. Everything agrees.

## func_020A3478 — one metatile to six tile entries

    r3 -> the u16 metatile index for this cell
    r2 =  *(u16*)(0x0214B9D0 + 2)                 the tileset split threshold
    i  =  *r3
    base = (i < r2) ? map[0x338] : map[0x33C]     0xCE*4 and 0xCF*4
    src  = base + 2 + (i - (i < r2 ? 0 : r2)) * 12

    dst_row0[0..2] = src[0..2]      3 tiles across, top row
    dst_row1[0..2] = src[3..5]      3 tiles across, bottom row
    func_02034B44(map, dst_row0, dst_row1, ...)   post-pass, see below

So a metatile is **12 bytes: six u16 tile entries, 3 across by 2 down**, held in
one of two tables each with a 2-byte header, selected by a threshold.

`func_020A3218(map, vec)` is `func_020A322C(map, vec.y) + vec.x * 2` — a row
pointer into a u16 metatile grid.

## The way in: VRAM block 1 is already there and already empty

`BG1CNT 4187` and `BG2CNT 4081` both have the size bits set: these are
**512x256** backgrounds, which on DS hardware is two 32x32 screen blocks, the
second at **+0x800**. The game writes 832 contiguous entries, which fills block
0 rows 0..25 — columns 0..31 — and never touches block 1.

The port's own decoder already reaches block 1 correctly:

    map_addr = row_base + ((sx & 0xF8) >> 2) + ((sx & width_extra) << 3)

with `width_extra` = 0x100, so `sx >= 256` adds 0x800. Widened hardware columns
land there by themselves:

    hardware 256..351   -> sx 256..351   -> block 1 entries  0..11
    hardware -96..-1    -> sx 416..511   -> block 1 entries 20..31

**So the host does not need to touch the guest's staging buffers, its fill loop,
or its DMA at all.** It only has to write 24 tile columns into block 1 that the
guest leaves empty. That sidesteps the obstacle that killed the guest-side plan:
a 56-column row is not contiguous in VRAM, so the single block DMA cannot be
stretched to cover it.

This is view-only and cannot desync — docs/85 established the simulation never
reads view state, and a widescreen host already plays in lockstep with a vanilla
client.

## What still has to be settled before writing it

1. **Which staging buffer is which BG.** `021523A0` and `02152A20` are DMA'd by
   separate calls (`func_0210CA60` and `func_0210CB20`); the mapping to BG1 and
   BG2 has not been confirmed.
2. **`func_02034B44` — SETTLED, and it is not the shroud.** It was guessed
   above to be the fog pass; reading it says otherwise:

       ldr r3, =0x02156CB0 ; ldr r0, [r3]     the map object
       blx func_020A4524
       bl  func_02034B1C(cell) -> r4          the ENTITY in this cell
       popeq                                   nothing here, done
       ldr r0, =0x021569E4 ; [r0] ; [r0+0x14]
       bl  func_020EEC14(that, cell) -> r6     a visibility test
       ...func_020092A8 byte 0 and byte 0x1F, func_020A186C, func_020A223C
       cmp r6, #0 ; popeq                      not visible, done
       bl  func_0205E000(entity)
       ldrb r1, [r5, #4] ; tst r1, #1          picks one of two tables
       mla  r6, r0, #6, r2
       ldrb r0, [r4, #0x71] ; ldr r2, [r4, #0x160]

   `+0x160` is the entity blueprint field, so `r4` is an entity: this is the
   pass that **stamps buildings into the tilemap**, gated on whether the cell is
   visible, with what look like reveal-map overrides forcing the gate open. So
   buildings are tilemap content while units are 3D billboards — which is
   consistent with masking BG0 removing the units but is worth re-checking
   against the castle, which did disappear with BG0 masked.

   A host pass must replicate this too, or buildings standing in the widened
   columns will be missing from ground that is otherwise drawn.

   The base terrain therefore carries **no shroud of its own** — it comes
   straight out of the metatile tables. Where the fog is applied has not been
   found, and the Free Play matches captured for this pass show no fog at all
   (the terrain is fully visible in every direction), so it cannot be studied
   from those captures.
3. **The host VRAM write path.** `read_mem` does not reach VRAM in this project;
   the renderer view (`nds_vram_renderer_view`) is the read side, and the write
   side needs checking.
4. **When to run.** The guest refills on the dirty flag at `view+0x66`; the host
   pass has to run at least as often, and must not fight the guest for block 0.

## Constants worth keeping

    0x02156CB0        pointer to the level/map object
    view + 0x5E/0x60  camera x, y (s16)
    view + 0x66       tilemap dirty flag
    map  + 0x338      metatile tileset A base   (0xCE * 4)
    map  + 0x33C      metatile tileset B base   (0xCF * 4)
    0x0214B9D0 + 2    u16 threshold selecting between them
    metatile stride   12 bytes, tables start at +2
    021523A0/02152A20 staging buffers, 0x680 bytes each
    fill loop         10 metatile columns, 12(+1) metatile rows
