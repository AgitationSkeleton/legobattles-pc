# 359 — The 3D snapshot keeps pointers instead of copying the raster

Written for: whoever works on the 3D renderer or the frame's timing next.

Status: **built, verified exact, deployed** 2026-09-24 04:50 (`5a0569ab`,
runner built 04:30, `build/LegoBattles.exe` sha256 `92793cca...`, settings
kept; with docs/360).

docs/358 s6 found the emulation thread copying the whole wide 3D raster
every frame: 2.3% of the thread at the widest surface. This removes the
copy.

## 1. Why the copy existed

The widened compositor runs at the frame boundary (the 263->0 wrap). By
then the software rasteriser has already started the NEXT 3D frame, at
VCount 215, in the only colour buffer it had. So `render_engine_line`
copied each wide 3D row into `g_wide_3d_frame` as its scanline passed, and
the rows below the guest's 192 at line 191, while they were still this
frame's. At 1720x642 that is about 5 MB a frame.

## 2. Two colour buffers

The software renderer now has two colour buffers (`ColorStore[2]`, applied by
`apply_hooks.py`), and each render draws into the one that is not on
display. A frame's rows then stay whole until the frame after next begins to
render. That is after this frame's composite, so the snapshot can simply
point at them.

- **Not a toggle.** `RenderFrame`, at VCount 215, records the buffer on
  display as `DisplayedBuffer`, and every render draws into the other one.
  That matters on the one path that renders a frame twice. `RestartFrame`,
  which `GPU::StartFrame` calls for an aborted 3D frame at line 0, runs in
  the same tick as the flip and before the composite reads the snapshot. A
  toggle there would clear the snapshot's buffer; this re-renders into the
  aborted attempt's own target. (`AbortFrame` is never set true in this
  build, only reset or loaded from a savestate; the rule costs nothing.)
- A frame skipped as identical draws nothing and leaves the shown buffer as
  it is, which already holds that picture.
- `GetLine` hands out `ShownColor`, which the switch publishes with release
  order. A `GetLine` that waited for a row of the new frame has acquired
  `RenderedScanlines`, which is stored after the switch. `VCount144` has
  already waited out the previous render before VCount 215, and the debug
  server runs its commands at the between-frames point. So no reader holds
  a snapshot row past the next VCount 215.

The runner keeps one row pointer per snapshot row (`g_wide_3d_rows`). A row
still has to be copied when `GetLine` returns the shared `ScrolledLine`
(the 3D layer's X scroll is non-zero, or the frame was aborted), or when the
compute renderer is active. `nds_gpu3d_line_persists` answers that per row,
and those cases copy exactly as before. A row the frame never recorded reads
the copy buffer, as before. `LB_SNAPCOPY=1` forces the copy.

The cost is 10.7 MB more for the second buffer.

## 3. Verified

- **The exactness gate:** identical at all six frames.
- **Layer maps and composites** inside the replay, byte for byte: 8 of 8 at
  1473x830 and 8 of 8 at 1280x720, against the same references as
  docs/358. `lb_thinholes.py`: 0.
- **Vanilla:** 240 of 240 presented and native frames identical to
  `exp-van-hudmask`.

## 4. Measured

`LB_SNAPAB=1` switches between copying and pointers every 300 frames in one
run and times the snapshot's work per frame, including any `GetLine` wait,
which is the same either way. The reference replay at the widest surface
(1720x720 at pixel scale 1 = 3440x1440), quiet machine, 16 pairs inside the
replay:

| | per frame |
|---|---:|
| copied | 0.39-0.49 ms |
| pointers | 0.033-0.050 ms |

About **0.36 ms a frame** off the emulation thread at the widest window, in
every pair. It is less at smaller surfaces, since the copy scaled with the
surface: about 2 MB a frame at 946x516.

## 5. Found on the way: the raster's borders

`SoftRenderer::ClearBuffers` still cleared the one-pixel edge-marking border
for 192 rows only, and LEGO Battles does draw with edge marking on. That is
docs/360, fixed separately.
