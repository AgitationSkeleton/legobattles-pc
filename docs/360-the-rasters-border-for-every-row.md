# 360 — The 3D raster's border, cleared for every row it has

Written for: whoever works on the 3D renderer next.

Status: **built, verified exact, deployed** with docs/359 (`5a0569ab`,
sha256 `92793cca...`). **Latent for the reference replay** (see s3).

## 1. The defect

melonDS's software rasteriser keeps a one-pixel border around the picture.
`SoftRenderer::ClearBuffers` fills it each frame with the clear depth and
the clear polygon ID. Edge marking (DISP3DCNT bit 5) compares every
polygon-edge pixel with its four neighbours, so at the picture's edges it
compares with that border. That is how a polygon clipped at the screen
edge gets its outline there.

The loops that fill the border still said 192 rows:

    for (int x = ScanlineWidth; x < ScanlineWidth*193; x+=ScanlineWidth)  // sides, rows 1..192
    for (int x = ScanlineWidth*193; x < ScanlineWidth*194; x++)           // bottom, row 193

This port renders up to 768 rows. On a taller surface, the side borders
below row 192 and the true bottom border (row `RenderHeight + 1`) were never
filled. They kept depth 0 and polygon ID 0 from `Reset`, or a taller frame's
pixels after the window shrank. Edge-marked polygons meeting the left or
right edge below row 192, or the bottom edge, then got no outline, or one
from stale geometry. Row 193 itself sits inside the picture now; the clear
loop repaints it, so it was harmless.

## 2. The fix

The same loops, over the rows the raster has:

    for (int x = ScanlineWidth; x < ScanlineWidth*(RenderHeight+1); x+=ScanlineWidth)
    for (int x = ScanlineWidth*(RenderHeight+1); x < ScanlineWidth*(RenderHeight+2); x++)

At `RenderHeight` 192 they are the original loops, so the DS's own picture
cannot move.

## 3. Does LEGO Battles edge-mark? Not in the reference replay

> **Correction (docs/388):** it does in any full match. `Building_Draw3D`
> draws Transport and Siege units (category 5 and 6, the `FUN_02036CD0`
> gate below), not buildings. The first ship, catapult or dragon on screen
> leaves DISP3DCNT at 0029 (edge marking on, AA off) for the rest of the
> session, because only G3X_Init at boot turns them back. The replay measured
> here has no such unit, so its 0019 throughout stands for that replay only.

The decomp says it can. The ARM9 image holds the literal 0x04000060 five
times. One is `Building_Draw3D`'s (0203B3AC, literal at 0203B764), which does:

- `DISP3DCNT &= 0xCFEF`: anti-aliasing off, and the two acknowledge bits;
- then `DISP3DCNT = (DISP3DCNT & 0xCFFF) | 0x20`: edge marking on.

It is called from the building pass of `View_DrawEntities` (0203A9E8),
behind `FUN_02036cd0()`, and it returns early before those writes when a
flag in its first call's result is set.

But the renderer draws with the value latched at the geometry flush, not
with the register. `LB_3DCNT=1`, added here, prints that latched value
whenever it changes. The whole reference replay shows:

| | DISP3DCNT | |
|---|---|---|
| boot | 0000 | |
| a 3D menu scene | 0018 | alpha blending, anti-aliasing |
| from there on, battle included | 0019 | texture mapping, alpha blending, anti-aliasing |

So **no frame of the replay is edge-marked**, and **anti-aliasing is on in
battle**. Anti-aliasing blends a pixel with the one below it in the same
column and does not read the border. The fix therefore changes nothing in
this replay:

- 108 of 108 dumps identical around frame 5000;
- 700 of 700 at 140 frames spread across the replay, including the 3D layer
  itself;

against the deployed build (docs/358, old borders), run as a copy in the
harness's build folder. It is kept because it is exact at 192 rows and
right for any scene whose frame does latch edge marking. What sets that
flag, and when `FUN_02036cd0` lets the building draw through, is not
established.

## 4. Verified

- **The exactness gate:** identical at all six frames.
- **Layer maps and composites** inside the replay: 8 of 8 against
  `gate-quiet`.
- **Vanilla:** 240 of 240.

A correction to an earlier reading: after reading `Building_Draw3D`, the first
conclusion was that LEGO Battles draws battles with edge marking on. The latched
value says otherwise. The register write is real, but it does not reach a
rendered frame in the replay.
