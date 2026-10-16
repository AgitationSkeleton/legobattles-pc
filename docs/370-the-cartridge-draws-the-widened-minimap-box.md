# 370 — The cartridge draws the widened minimap box

Status: **fixed, verified live.** The defect: the top-screen camera square
flickered back to vanilla size on the minimap while in motion.

The port no longer draws the box. The cartridge does, sized for the view the
window shows, from four patched words in its own plotter. 715 top-screen frames
captured during pans in all four directions contain no stock-size box.

## The cartridge's plotter

The decomp was done by static analysis and re-checked here against the ROM
image.

* **The call chain.** `Task_View_Update` (`0203942C`, priority 0x15, every game
  update) calls the minimap update `FUN_020267F8` right after the camera
  commit, and that calls the box plotter **`FUN_02025CF8`** at `02026EC8`.
  Each has exactly one caller.
* **What the plotter draws into.** CPU-written 4bpp pixels in a main-RAM tile
  canvas (`mm+0x84`, `mm = *(view+0x34)`), queued to VRAM every update. It is
  shown as the top screen's BG3, scrolled so the 96x96 canvas sits at (136,40).
  The box is palette index 0xE (0x7FFF), which is the (251,251,251) docs/255
  measured.
* **Only a quarter of the rows per update.** The minimap redraws 24 of its 96
  rows each update, rotating, and the box segments in those rows. While the
  camera moves, the box on screen is slices drawn from up to four different
  camera positions.

The position and the size, from the ROM:

    02025D00  ldrsh r2, [r1,#4]      camx (view+0x5E)
    02025D0C  add   r2, r2, #12      + half a metatile, to round
    02025D18  ldrsh r3, [r1,#6]      camy (view+0x60)
    02025D24  add   r3, r3, #8       + half a metatile
                                     -> metatile -> mt2px[] = 95*j/(N-1)
    02025D5C  ldrb  r1, [r6,#0x8CC]  the size, cached at mm+0x8CC/+0x8CD
    02025D68  bne   02025DC0         (the constructor sets 0xFF: compute once)
    02025D6C  mov   ip, #0xF8000     248.0, the view's width:  /24 = 10, +1 = 11
    02025D70  mov   r3, #0xC0000     192.0, the view's height: /16 = 12
                                     -> mt2px[11] x mt2px[12] = 16 x 18 px
    02025DC0..                       clamp into the world rect's minimap extent

N is the map's width in metatiles (`u8 map+0x14`). One table serves both axes,
so the minimap is uniform in metatiles and only looks squashed in pixels.
docs/180's two-scale linear fit is within 3 px of this for a stationary camera.

## Why the port's box flickered

The port found the cartridge's 16x18 box in the top screen's pixels, erased it
in a copy, and drew a wider box with ImGui from docs/180's fit. It accepted a
found box only within 8 px of the live camera's prediction (docs/347 item 2).
A box drawn in slices from up to four updates, a frame behind, plus the fit's
own error, fails that test on moving frames. Then nothing is erased and
nothing is drawn: the vanilla box shows. That is the flicker.

## The fix: four words (`apply_minimap_box`, lb_patches.cpp)

The four words are rewritten for the visible view, `vis x visr`: the layout's
report, clamped to the surface as the HUD anchor clamps it.

| site | stock | patched |
|---|---|---|
| `02025D0C` | `add r2,r2,#12` | `add/sub r2,r2,#(12 - c)`, c = (vis-256)/2 |
| `02025D24` | `add r3,r3,#8` | `add/sub r3,r3,#(8 - ct)`, ct = (visr-192)/2 |
| `02025D6C` | `mov ip,#248.0` | `mov ip,#CW`, CW in [24k, 24k+23], k = (vis-8)/24 |
| `02025D70` | `mov r3,#192.0` | `mov r3,#CH`, CH in [16t, 16t+15], t = visr/16 |

* **The origin.** Mid-map the visible field is centred on the native window.
  The band is centred, and the frontend crops the middle `vis` columns of the
  surface; the HUD anchor uses the same `(vis-256)/2`. So the field's left
  edge is `camx - c`. At a map edge the band slides, and the plotter's own
  clamp stops the box at the minimap's edge.
* **The size.** Any value in a metatile window gives the same count, so one
  whose `<<12` is an ARM immediate is chosen. The counts are capped at `N-2`
  and `N-1`, so `mt2px[]` (N entries) is never read past its end.
* **The cache stays.** The first design also turned the cache test into a
  `nop`, so the size would be recomputed on every call. That would add guest
  instructions to every update, and the cadence reads update length. Instead,
  when a size literal changes, `mm+0x8CC` is set back to the constructor's
  0xFF and the cartridge recomputes once.

Otherwise it follows the existing patch idiom: gated on Enhanced (and
`LB_WIDEMAP`), off for a native-size view, and `LB_MMBOX=0` turns it off
alone. All four words or none, self-healing each VBlank, logged on change.
Going back to stock puts back the stock words and resets the cache.

While it is live (`lb_minimap_box_live()`), the Top Screen panel neither
scrubs nor overdraws the box. The finder would otherwise take a 13-25 px tall
box for the old one and erase it.

## Verification

Held Free Play battle at 1473x830: a 735x414 view, a 64-metatile map.

    [lb_mmbox] the cartridge draws the minimap box for a 735x414 view:
               origin E24220E3 E2433067, extent 720x400

`sub r2,#227` and `sub r3,#103` are exactly 12-239 and 8-111. The cartridge's
cached size read back as **46x37**, from `mt2px[31]` and `mt2px[25]`.

`mmtest.py` captures the raw top screen (`framebuffer engine=A`) stationary and
while the D-pad is held right, left, down and up. It finds the white
components inside the minimap panel:

* **Stationary:** one box, `(136,40) 46x37`, 162 px. That is the outline of
  46x37, at the minimap's corner with the camera at (0,0).
* **Position check:** at camera x 944 the box is at x 179, and the ROM formula
  gives `136 + mt2px[(944-239+12)/24] = 136 + mt2px[29] = 136 + 43`.
* **The flicker:** **0 of 715** moving frames hold a 16-wide box.
* **The shear.** While panning sideways the box is two components: rows 40-63
  at one x, and the rows below about 3 px behind. That is the quarter-row
  redraw above, the same mechanism the stock box has, visible because this box
  spans two quarters. It is the cartridge's drawing, not the port's.
* **The window:** one white box on the Top Screen panel, 59.3 fps.

**Exactness** (the recorded replay `lb-session-1790122638` at 1473x830), as an
A/B inside the new build:

* **exp-gate370a, `LB_MMBOX=0`:** identical to gate369 at all six checkpoints,
  and 8/8 layer maps. With the patch off, nothing else changed.
* **exp-gate370b, the default:**
  * **Counts:** identical at frame 1500 (no minimap yet). From frame 3000,
    ARM9 retires about 57 more instructions per frame: the cartridge drawing
    longer lines. ARM7 is a few dozen off from the new interleaving, and the
    cycle totals are unchanged.
  * **Layer maps:** 8/8 identical to gate-quiet.
  * **Frame rate:** 59.50 fps.
  * **Resize sweep:** the words followed every size, from a 306x216 view to
    1536x626, where the width count is capped at N-2 (1488 = 24*62).
* **exp-van370 (adaptive off):** 240/240 identical; the patch never engaged.

## Not changed

* **The quarter-row shear.** Removing it would mean the cartridge redraws all
  96 rows every update, four times its minimap work, for a vanilla behaviour.
* **The minimap shroud readback** (lb_field, fallback path only). It still
  counts the white outline as explored. A wider box has more outline pixels
  (162 against 64), on the path that is used only when the record is missing.
