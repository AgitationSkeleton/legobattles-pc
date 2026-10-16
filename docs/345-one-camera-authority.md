# 345 — One camera authority: both port clamps off by default

Two camera defects (docs/343):

* **The clamp:** depending on aspect ratio, the initial starting position or
  the camera might not position accurately. At a 2749x1234 window the camera
  could not move far enough down to see the very bottom of the map, where the
  player's units and base are.
* **The flicker:** the original-aspect region flickers a bit once moved from
  the initial starting position, most noticeably when holding an input
  against the world border or while the camera is in motion.

Status: the clamp **fixed and measured**; the flicker **not reproduced** --
one of its plausible sources is removed, which is not the same as fixed.

## Three conventions for one camera

The camera is `view+0x5E/+0x60`, one object (decomp report). Three pieces of
code disagreed about what that point means in a widened window:

| code | assumes the camera is | effect |
|---|---|---|
| the band (`band_compute`, `band_split`) | top-left of the guest's own 256x192; the overhang slides to whichever side has map at an edge | the window ends exactly at the map edge |
| `apply_camera_view_size` (patched the cartridge's clamp constants) | top-left of the WHOLE surface | camera stops at `world - surface`, short by the overhang |
| `apply_camera_clamp` (rewrote the camera every frame it disagreed) | centre of a centred band | fights the cartridge's own clamp |

The cartridge's two clamps are `View_ClampCamera` (0x0203906C: x <= (max+1)*24
- 248, y <= (max+1)*16 - 192) and `View_ClampCameraToBounds` (0x0203CED4:
x <= max*24 - 232, y <= max*16 - 176). Stock, they keep the guest's 256x192
inside the map -- which is exactly what the band's convention needs.

## Measured at a 2749x1234 window

Surface 1570x704, world 1536x1024, panning down against the map's edge:

| configuration | camera stops at y | window bottom | port clamp acting |
|---|---:|---:|---:|
| as shipped (both on) | 336 | 776 -- **248 px short** | 841 / 8,113 frames |
| port clamp off | 336 | 776 | -- |
| view-size patch off | 576 | 1024 | **2,761 / 9,340 (30%)** |
| **both off** | **832** | **1024 -- exact** | **never** |

(A first reading of the last row put the window 256 px past the map. That was
the first frame at the edge, before the 3D's band caught up; at steady state
both bands read 512 on 4,488 of 4,491 frames.)

## The change

* `apply_camera_clamp` -- off unless `LB_CAMCLAMP=1`.
* `apply_camera_view_size` -- the constant patch is off unless `LB_CAMVIEW=1`,
  and while off it restores the cartridge's stock encodings. The world-extent
  publication it also does is **kept**: the frontend caps the surface at the
  world's size with it, which is what keeps black bars off a window wider than
  the map (measured: the 2749x1234 surface is capped to exactly 1536 wide).

## Verified edges

Default settings, holding each direction for 16 long presses:

| window | up | down | left | right | frames past the world |
|---|---|---|---|---|---|
| 2749x1234 | top 0 | bottom 1024 | left 0 | camera 1288, the cartridge's own maximum | see below |
| 1920x1080 | top 0 | bottom 1024 | (in progress) | (in progress) | 0 |

Held at every edge, the camera, the band and the game's own scroll registers
did not change on a single frame after arrival.

**Known, not new:** when the surface is exactly the world's width, the window
sits 4 px right of the map mid-map and 8 px at the right edge. That is the
cartridge's own background bias (`4 - camx mod 8`, docs/339) plus the 8 px its
stock clamp allows past 256 (248, not 256). On screen at the right edge it is
continuous shroud -- no black bar, no seam.

## The flicker, honestly

The fight is gone: with the new defaults nothing outside the game writes the
camera. But the flicker did not reproduce in the harness with either
configuration -- held against every border, the game's own scroll agreed with
the camera on every frame in the old configuration as well as the new. The
defect also involves camera motion, which matches the sprite-vs-ground
one-frame drift measured in docs/342 and still open. So: one plausible source
removed, flicker not proven fixed.
