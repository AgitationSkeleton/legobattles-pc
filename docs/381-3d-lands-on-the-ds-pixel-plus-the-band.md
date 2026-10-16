# 381 — 3D lands on the DS's own pixel, plus the band

Status: **three faults fixed in the widened viewport transform** (apply_hooks
stanza 0zzzzzzzzz). Found while finishing docs/374's open half, "Y needs the
exact form".

| fault | who saw it | effect before this doc |
|---|---|---|
| 1. The product wrapped at surface column 1024 | every surface 1025-1728 wide: the 1360 and 1720 of a 09-22 log | 3D right of column 1024 was drawn 1024 columns to the LEFT |
| 2. Rows only approximately the DS's | every widened surface | about half the 3D vertices sat one row off the DS's row + band |
| 3. The guest could read the widened clip matrix | skinned models (dragons, giants, transport ships...) in any widened view | their skinned parts were scaled vertically a second time |

## 1. The product wrapped at column 1024

`G3_SetupScreenOrtho` (020FA694) loads `G3_OrthoW(0, 192.0, 0, 256.0, n,
1024.0, scaleW 0x400000)`, so every sprite vertex has **w = 2^22**. The DS's
viewport divider halves the numerator and the denominator when w > 0xFFFF,
leaving posX up to 2^22. The widened branch multiplied that by RenderWidth
**in u32**:

    s32 lb_px = (s32)((posX * viewport_width + lb_round) / den)

posX·RenderWidth is column·2^22, which passes 2^32 at **column 1024** on any
surface. A vertex at column c > 1024 landed at c − 1024. The soft renderer
caps the surface at 1728x768 (`MaxRenderWidth`/`MaxRenderHeight`), so every
width from 1025 to 1728 was exposed. Rows would wrap the same way past row
1024, but the 768-row cap keeps them clear; the row product is widened too,
defensively.

**Why nothing caught it.** docs/374's exact Python model of this line checked
2.64M cases up to RW 1728, but in unbounded integers, so the wrap could not
exist in it. And 1.9M logged polygons from 1096- to 1570-wide surfaces
(a one-off `polyxmax.py` over every saved `[lb_polylist]`) never started
past column 1024. That read as "no problem", but the harness camera never put
content there: the furthest right any polygon reached was 943.

**Measured** on a 1200x500 surface (`LB_WINDOW=2400x1000`, Free Play on the
default map with `LB_CHEATS=0`, the D-pad panned to the map's east edge;
a one-off `wraptest.py`), same frame, same camera (812,154):

* The new `LB_VTXLOG=1` counted 2,048+ vertices whose old product would
  have wrapped, for example `native 18219008 band 472 rounded 132 exact 1156`.
* The deployed docs/380 binary's own render list has the brick pickup at
  **surface x 132..148**; this build has it at **1156..1172**.
* The picture (`screenshots/exp-wrap381b/win/wrap-ab.png`, from the two runs'
  `lb_comp-1200x500-1800.png`): before, the pickup sits among the trees at
  the far left and its clearing at the right is empty. Now it is in the
  clearing and nothing is drawn at the left. The terrain is identical. The
  other four pickups differ only by their animation frame (the runs are not
  in phase) and by part 2's one-row moves (the render lists: 16→15,
  335→336, 495→496).

The widened branch now multiplies in 64 bits. The native branch keeps the
DS's 32-bit product, unchanged.

**What this meant in practice.** The 09-22 log ran surfaces of
1360x592 and 1720x690. docs/373 keeps the guest's own 256 columns centred
(472..728 at 1200; 732..988 at 1720), so the HUD and the camera's
surroundings stayed below 1024. **Everything in the right band past column
1024** was drawn over the left band instead: at 1720, the right 40% of the
picture's units, buildings and pickups. That explains sprites moving or being
omitted on resolution changes, and is a cause of the overlap differing at
some resolutions. Both defects appeared only at some sizes; the steady
sweeps that found nothing (docs/376-380) ran surfaces narrower than 1024.

## 2. Every vertex on the DS's own row

The widened transform used to reach a pixel in three steps, and each one
truncated:

* the clip matrix's Y column is scaled by `k = (192<<12)/RenderHeight`
  (12 fractional bits);
* the band shift is subtracted in clip space (`2·w·s/RH`);
* the viewport floors `(posY·RH)/den`.

A bias fixes X (docs/374) but not Y (`sprites_biasfix_y2.py`: 27% of rows
still off).

**The exact form.**
* **Carry the DS's own coordinate.** Each vertex carries its **unscaled**
  clip X and Y (`Vertex::lb_Native`). X is `Position[0]` before SubmitVertex
  scales it. Y comes from `lb_ClipNative`, the matrix `UpdateClipMatrix`
  builds before the widened Y scaling. The clipper interpolates both into the
  vertices it makes.
* **Run the native arithmetic on it.** The transform applies the native
  viewport's own arithmetic (the same `>>1` when w > 0xFFFF, the same floor),
  in signed 64 bits so it continues past the native frustum.
* **Add the integer band.** The band is `lb_band` / `lb_band_y`, the same
  latched `g_list_left`/`g_list_top` the clip-space shift used.
* **Keep the edge.** A clipped vertex on the surface's edge keeps the edge,
  or the edge would gain a one-pixel gap.

This is exact by construction wherever the DS draws the vertex. The vanilla
path is the same arithmetic without the band. `LB_VTXEXACT=0` turns it off
(docs/374's rounding, still 64-bit).

**Measured** (gate replay, 736x414, `LB_VTXLOG=1`):

* **Y:** about half of the widened vertices move one row onto the DS's row;
  `exact differs from rounded` passed 65,536 at 129,294 vertices.
* **X:** 0 differ. docs/374's bias was already exact for the whole and half
  pixels the cartridge uses.
* **Layer maps** against exp-gate377 (a one-off `shiftcheck2.py`): every
  changed pixel is 3D content moved exactly one row.

| frame | differing px | what they are |
|---|---|---|
| 1285 | 56 | all 3D moved one row down |
| 2570 | 404 | all 3D moved one row down |
| 3855 | 40 | 39 up, 1 down |
| 5140 | 243 | 238 up or down; 5 on one builder's diagonal and dithered outline, the same move |

The old rounding erred in **both** directions, depending on where content sat
against the band, not just "1 px up" as the model had it.

**Against vanilla itself** (a one-off `hudrows.py`). The reference replay was
held open at its end three times: vanilla (adaptive none), widened exact, and
widened with `LB_VTXEXACT=0`. Each time the live render list was read with
the debug server's `gx_polygons`. HUD quads sit at fixed guest positions, so
they pair across the runs by texture, palette and guest x.

| widened transform | HUD quads paired | on vanilla's rows |
|---|---|---|
| exact (this doc) | 5 | **5** |
| old rounding | 5 | 4; tex 6ED00000 at x 4..28 read y 167..191 against vanilla's 168..192 |

The sample is small, and the world quads did not pair: the three held
moments differ. But this is the DS's arithmetic run in vanilla mode, and it
agrees with the construction.

`LB_POLYLIST` cannot do this comparison: its frame number is ticked by the
widened compositor, so a vanilla run prints one frame only.

**A rarer case the exact form also removes.** In the gate, about 20 vertices
around one camera jump had a clip-space shift from band 265 while their
polygon's transform saw band 185. The polygon's last vertex and its transform
always share one latch, so such a polygon had vertices from **two** bands and
was drawn stretched between them for a frame. That needs a SwapBuffers to
execute between two vertices of one polygon. The swap is a task at priority
0x25 after every GX writer (`Task_SwapBuffersAndOam`, 02024D08), so the CPU's
own writes cannot split a polygon. The cartridge does stream lists by DMA:
the SDK's asynchronous GX send is at 021101CC (it waits for the FIFO to be
under half full, then starts a GX-FIFO DMA with IRQ 21) and its completion
handler at 02110370. A SWAP_BUFFERS written while such a transfer is still
feeding the FIFO would land mid-list. Whether that also happens on hardware,
or only under the port's DMA timing, is not verified. The exact form places
every vertex of a polygon with the one band its transform sees. Where that
puts a vertex off the surface's edge, the vertex is clamped, as before.

## 3. The guest reads the DS's clip matrix

melonDS answers the clip-matrix result registers (0x04000640..0x0400067F)
and PosTest from `ClipMatrix`, and the widened frustum scales that matrix's
Y column. The cartridge links `G3X_GetClipMtx` (0210C350). Its callers are
NitroSystem's SBC handlers; the table is at **02150100**: NOP, RET, NODE, MTX,
MAT, SHP, NODEDESC, BB 020FF6A4, BBY 020FF9AC, **NODEMIX 020FFD10**,
CALLDL, POSSCALE, ENVMAP, PRJMAP 021008FC.

* **Which models run them.** The ROM's 39 `.nsbmd` models were walked command
  by command (a one-off `nsbmd_sbc.py`; every stream ends in RET). None uses
  BB, BBY, ENVMAP or PRJMAP. **12 use NODEMIX**, 60 commands between them:
  W_Giant (the Troll Giant), K_Dragon, W_Dragon, the King and Pirate transport
  ships, the Imperial and Pirate clippers and flagships, P_Shark and
  E_ClawTank.
  These are the model-backed categories 5 and 6 (docs/242).
* **What NODEMIX does.** NODEMIX is skinning. At entry it sets MTX_MODE =
  projection, MTX_STORE and MTX_IDENTITY (0x04000440/44C/454, from the
  handler's literal pool). Then, for each weighted node, it restores the
  stored matrix, multiplies in the inverse bind matrix, reads it back through
  CLIPMTX (the projection is identity, so that is the position matrix),
  blends, loads the result and restores the projection.
* **What the widened frustum did to that.** Every readback came back with
  its Y column already scaled by k = 192/RH, and the blend was scaled again
  at draw time. A skinned vertex at guest row y therefore landed at
  `centre + k·y` instead of `centre + y`, while the unskinned parts of the
  same model were right. At 736x414 (k = 0.46), a transport ship at guest row
  150 would have had its skinned parts about 80 rows too high.

The guest now reads `lb_ClipNative`, the DS's own matrix, for CLIPMTX_RESULT
and PosTest. In vanilla it equals ClipMatrix. `LB_CLIPREAD=scaled` restores
the old answer. BoxTest still uses the widened matrix and is only counted.
`LB_GXREADLOG=1` logs each kind at the 1st, 2nd, 4th ... occurrence, with
whether the projection is identity.

**Live.** The gate replay draws no model: `LB_GXREADLOG` logged no reads of
any kind, and the counts are identical to gate370b. So the test used docs/242's
recipe: Free Play on River Crossing (`--map-index 3`), 14 minutes
(a one-off `modelrun.ps1`, run `model381`).

* **The reads.** From about 3.8 minutes in, the cartridge read CLIPMTX_RESULT
  **more than 262,144 times, every time with the projection = identity**,
  and made no PosTest, BoxTest or POS_RESULT access. Only BB, BBY, NODEMIX,
  PRJMAP and the ENVMAP/PRJMAP helper call `G3X_GetClipMtx`, and no model
  uses the other four, so these are NODEMIX.
* **The model.** For most of the run it was out of view. The harness's closing
  resize put a transport ship on screen (996x546, dump frame 57000, 129
  polygons, the top band at guest y -129..-122). It is drawn whole: hull,
  masts, crow's nest and health bar together
  (`screenshots/exp-model381/win/ship-after.png`).
* **Owed.** With the old answer (k = 192/546), its blended-matrix vertices
  would have landed about 80 rows below the hull. A picture of that needs a
  second run with `LB_CLIPREAD=scaled` and the ship on screen again. Not
  taken this pass.
* **Found on the way, not investigated.** With `LB_CHEATS=0` (Reveal Map)
  held, River Crossing was still shrouded outside the base at dump frames
  7200 and 14400, and open by 57000. On the default map (the wrap runs) the
  east side was open within seconds. So the cheat's effect differs by map or
  by time, and cannot be relied on to put a far unit on screen.

## Verification

* **Counts:** exp-gate381 is identical to gate370b at all six checkpoints.
  The replay reads no guest-visible 3D result, so the change is host-only
  there.
* **Vanilla** (exp-van381): 240/240 identical.
* **Frame rate:** 59.70 fps (gate, 736x414); 59.94 fps (the replay at
  1200x500); 59.86 fps (the replay at `LB_WINDOW=3440x1380`, which the
  test display gave as a 1536x616 surface; emulation 11.15 ms a frame).
* **Pictures:** the eight gate frames differ from gate377 only by the
  one-row moves above.
