# 373 — The camera stops where the picture stops

Status: **fixed, measured live.** The defect: the camera can be slow to start
moving, as if it carried momentum from the original aspect's location.

That suspicion is right. At a map edge the camera kept travelling to the
stock limit while the picture stood still, so panning back out first had to
cross a dead zone. At a 736-wide surface that zone is 240 px of camera travel.

## The mechanism

* **The stock clamp** (since docs/345). The cartridge's `func_0203906C`
  lets the camera (the top-left of the guest's own 256x192) reach its
  own limits:

      max_x = (max_tile_x + 1) * 24 - 248      max_y = (max_tile_y + 1) * 16 - 192
      min   = min_tile * (24 | 16)

* **The band slides instead.** At a map edge the band slides so the picture
  stays inside the world (`band_split`, lb_widemap). The band is centred only
  while `camx - 4 >= span/2` and `map - (camx + 256) + 4 >= span - span/2`
  (the 4 is the BGxOFS bias).
* **The dead zone.** Between the camera position where the band starts to
  slide and the stock limit, the camera moves and the band slides by the same
  amount, so the picture does not move.

## The fix (lb_patches.cpp, `apply_camera_edge`)

The clamp now stops the camera where the band would start to slide, by the
cartridge's own arithmetic.

* **The max side** is a view-size subtraction, so it takes a larger view
  size.
* **The min side** has no constant at all (`min_tile * 24`). So each axis's
  two helper calls, both the cartridge's own fx32 add/subtract, are
  rearranged:

      stock    FUN_0200E634(.., &max, 24)       FUN_02039134(.., &max, 248)
      patched  FUN_02039134(.., &max, V - 24)   FUN_0200E634(.., &min, c)

  * x: V = 252 + (span - span/2), c = span/2 + 4
  * y: V = 192 + (span_y - span_y/2), c = span_y/2

  That is ten words at `020390A4..020390D8`, all or none, self-healing.
  Immediates that cannot be encoded round toward stopping sooner. An axis with
  no overhang keeps the stock range exactly.

  > **Corrected by docs/393: these bounds left the cartridge's camera grid.**
  > The cartridge keeps its camera on multiples of 8: the d-pad glides 32 px
  > from rest to rest, a minimap tap masks with 0xFFF8, and every stock bound
  > is a multiple of 8. BG3 (the placement footprint) is scrolled once to
  > (4, 0) and lines up with the terrain only on that grid. span/2 + 4 put
  > the minimum at camx 244 (scale 2.0) and 203 (2.25), where a battle that
  > starts against the map's edge rests. The checkmark then sat off the
  > footprint, and the cell mask cut a hole beside it. All four immediates
  > are now rounded up to multiples of 8 (`camgrid=1` in recordings).
* **The world rect** (`+0x150/+0x158`), which the simulation reads, is never
  written (the docs/281 lesson). `func_0203CED4`, the level-start clamp, is
  left stock: every later move goes through `func_0203906C`.
* **Gating.** Enhanced only. `LB_CAMEDGE=0` turns it off alone. It is off
  under `LB_CAMVIEW=1`, docs/345's older patch, which shares 020390C4.
* **Recordings.** A recording made before this replays with the stock range
  (`camedge=` in the demo header): its taps were aimed at the camera it had.

A conflict found on the way: with `LB_CAMVIEW` off, `apply_camera_view_size`
"restored" 020390C4 and 020390D4 to stock whenever they held a non-stock
`mov r2`. That is this patch's min margins, so the two functions rewrote the
clamp against each other every frame (4,898 log lines in one run). It now
leaves those two words alone while this patch is live.

## Measured

Held Free Play battle, 1473x830 (a 736x414 surface), `LB_CAMEDGELOG=1`,
which logs camera, band and picture (camera minus band) on every change. The
D-pad was held into the west edge and away, then into the north edge and
away. `camseries.py` sums each press:

| press | stock range (`LB_CAMEDGE=0`) | this fix |
|---|---|---|
| away from the west edge | camera 1 -> 1168, picture 1 -> 928: **240 px of camera travel without the picture** | camera 245 -> 1044, picture 5 -> 804: **1:1** |
| into the north edge | camera to 0, band stays 111, picture to **-111**: 111 rows above the map | camera stops at 111, picture at **0** |
| away from the north edge | 1:1 | 1:1 |

The applied values were view size 492x304 and min + 244,111. 303 is not an
ARM immediate once 16 is subtracted, so it rounded to 304: the picture stops
1 px short of the south edge rather than 1 px past it.

A second finding came out of the same table. Vertically the band never slid
in the stock range: at the north edge the picture overhung the map by 111
rows. With the camera stopping at the band's own margin, that overhang is
gone too.

**Exactness** (exp-gate373, the reference replay at 1473x830):
* **Counts:** identical to gate370b at all six checkpoints. The recording
  predates `camedge=`, so it replays with the stock range.
* **After the replay:** once playback ended the patch engaged, and it followed
  the harness's resize sweep, from 306x216 (min + 29,12) to 786x426
  (min + 272,117).
* **Layer maps:** 8/8 identical. **Frame rate:** 59.22 fps.
* **Vanilla** (exp-van373): 240/240 identical.
