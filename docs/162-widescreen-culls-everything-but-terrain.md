# Widescreen currently widens terrain only: everything else is culled

The requirement is that a widened camera must not cull units, buildings,
fog of war, shroud or particles, that the minimap's camera box must match the
window, and that edge-anchored UI must follow the edges. Checking the first of
those against the working widescreen from `docs/157` finds it **not satisfied**,
and the reason is specific.

## Units are 3D, not sprites

```
engine A DISPCNT = 00012F08   ->  OBJ enabled = 0
engine A OAM     = every entry attr0=00C0, y=192   (parked off-screen)
engine B OAM     = the same
```

**Object rendering is switched off on both engines**, and all 128 OAM slots are
parked at y=192. Nothing in this game is a hardware sprite.

That retires a line of reasoning carried in `lb_wide2d.inc`, which explains at
length that objects cannot extend because "the DS submits at most 128 sprites
and LEGO Battles fills OAM for a 256-wide viewport". OAM is not filled at all.
The units are the **3D layer**, BG0, which `docs/156` measured at 5.7% and whose
DISPCNT bit 3 is set.

## And the 3D layer is culled by the game, not the compositor

The compositor is not the constraint. `nds_gpu2d_adaptive_framebuffer` calls
`render_obj_line(0, y, obj, output_width, ...)` -- **full output width** -- and
`nds_gpu3d_set_output_width(448)` widens the 3D render surface itself.

The constraint is upstream of both: `gx_polygons` reports **3** and stays at 3
across 20 million cycles of scrolling. The game submits geometry only for what
its own view rectangle contains, so the widened columns receive terrain from the
BG layer and **nothing else** -- no units, no buildings, and by the same
mechanism no particles.

Scrolling until the castle sits at the native right edge shows exactly this: the
terrain reaches the left edge of the 448 frame, the castle renders at its native
position, and the area beyond it is **black**.

## So the honest status of `docs/157`

`docs/157` said widescreen gameplay works, and what it demonstrated was real:
terrain in the widened columns, verified on screen. But it is **terrain only**,
and calling that "widescreen gameplay" overstates it. A player would see ground
extending into empty margins where units ought to be.

The three requirements now stand as:

| requirement | status |
| --- | --- |
| no culling of units, buildings, particles | **not met** -- the game culls to its own view rect before submitting geometry |
| fog of war / shroud across the widened area | **unknown, untested** -- likely the same mechanism |
| minimap camera box matches the window | **unknown, untested** |
| edge-anchored UI follows the new edges | partially -- `docs/131` anchors the top-screen HUD; the in-field sidebar buttons are untested |

## What would actually satisfy them

All of it reduces to **one** thing: the game's view rectangle. Widen that and
the game submits geometry, fog and particles for the wider area itself, the
minimap box follows because it is drawn from the same rectangle, and the tile
streamer fills the extra columns without any of `docs/159`-`161`'s buffer
surgery.

`func_020398B8` computes a rectangle from the camera at `+0x5E`/`+0x60` plus
`+0x62`/`+0x64`, and both of those read **0** live -- so they are a delta, not
the size. The size lives elsewhere and finding it is now the single highest-value
target in this thread, because every one of the widescreen requirements falls out of
it at once.
