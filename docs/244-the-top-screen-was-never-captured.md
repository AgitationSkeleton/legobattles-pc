# 244 — The top screen was never captured, and the aspect box

Status: **measured.** Four things settled, one of them a retraction.

## The retraction

An earlier investigation compared `adaptive-0-498x360.png` with
`adaptive-1-498x360.png`, found them near-identical, and concluded:

> the top screen mirrors the field, there is no minimap, that idea is dead.

**That is wrong and is retracted.** The dumper's filename was

    "%s/adaptive-%d-%dx%d.ppm", lb_dir, lb_seq++ & 3, bottom_width, lb_dh

and the first `%d` is `lb_seq++ & 3` — a **ring slot**, not a screen index.
Every file that dumper ever wrote is `bottom_pixels`. The two files compared
were two moments of the *same* screen, thirty frames apart, so of course they
matched. The top screen had never been dumped at all.

There is a minimap, on the top screen, with a
white camera box on it.

The lesson is narrow and worth keeping: a ring index and a screen index look
identical in a filename. The dump now writes `screen-top`, `screen-bot`,
`presented-top` and `presented-bot` as words.

## Which engine drives which screen, measured rather than assumed

`POWCNT1` bit 15 routes engine A to the top when set. Read live:

| when | POWCNT1 | engine A |
|------|---------|----------|
| title | `0x820F` | top |
| **gameplay** | `0x020F` | **bottom** |

The routing **flips for gameplay**. A one-shot `static bool` print reported
`0x820F` and was reporting the title screen; that is a real instrumentation
trap, because the value is also the power-on reset value the runtime itself
writes at direct boot, so a stale read is indistinguishable from a plausible one.

Confirmed from the other side by the display registers:

* engine A `DISPCNT = 0x00012F08` — mode 0, **BG0 = 3D**, all four BGs on,
  OBJ **off**. That is the field: BG0 units, BG1 terrain, BG2 fog, BG3 inert.
* engine B `DISPCNT = 0x00011F00` — all four BGs on, OBJ **on**. That is the
  HUD/minimap screen.

So during a battle the **field is engine A on the bottom LCD** and the
**minimap is engine B on the top LCD**. `gpu2d.cpp` already carried a comment
saying exactly this; it is now measured.

Both engines' OAM read `0x000000C0` in every one of the 128 entries — Y = 192,
parked off-screen. Nothing on either screen is a sprite, so the minimap box is
plotted into BG tile data, not OAM.

## The white camera box

Located from the top-screen dump by taking near-white, near-neutral pixels and
looking for the columns and rows carrying a long run of them — an axis-aligned
outline is exactly that:

    columns  x = 202, 217   (18 white px each)
    rows     y =  53,  70   (16 white px each)

A **16 x 18** box. `docs/180` fitted the minimap transform from 18 camera
sweeps and predicted `256 * ax = 15.3` by `192 * ay = 17.5` for the DS view;
this is an independent confirmation from a single frame.

The transform predicts the box's *position* too. At map camera (1048, 144):

    predicted (201, 53)      measured (202, 53)

within a pixel on both axes. `lb_field_map_to_minimap()` is correct.

At a 996-wide window the box *should* be about 60 x 50 rather than 16 x 18.

The minimap canvas itself is engine B BG VRAM around `0x06204640`: a watch over
`0x06200000..0x06220000` fills its ring from one PC, `0x01FF8260` in ITCM,
writing `0x44444444` — a 16-colour clear to palette index 4. That is the canvas
wipe, not the box plotter; the ring overflows on it before the box is drawn.
Finding the plotter needs a narrower watch.

## The debug aspect box

`LB_ASPECTBOX=1` draws an opaque yellow rectangle around the guest's own
256x192 inside the widened surface, using the band **the composite actually
used** (recorded as `g_lb_used_extra` / `g_lb_used_extra_y` at the point of use,
not recomputed at present time, which can have moved on by a frame).

It is drawn in a wrapper around `nds_gpu2d_adaptive_framebuffer` and only when
the returned width exceeds 256 — the native pointer is `g_fb` itself, the
guest's own framebuffer, and drawing into that would corrupt the frame rather
than annotate a copy of it.

Measured on a 996x546 surface: the box is **exactly 256x192 at (114, 72)**.
The band arithmetic is right.

### What it settles

With a hero panned into the left widened column (`sx = -61`, 61 px left of the
guest's viewport), the base and a unit render **completely outside the box**.
Entities are not culled outside the DS aspect in this scene.

This is the measurement four earlier attempts failed to make from polygon
extents and layer dumps, all four retracted. A rectangle drawn in the
presented surface's own coordinates cannot be argued with.

### What it exposes

In the same frame the revealed terrain has **perfectly straight top and bottom
edges lying exactly on the box's top and bottom**, while its right edge is
ragged. Straight edges aligned to the native aspect are the signature of the
conservative window-derived fog cache: the camera panned only horizontally, so
only rows inside the native 192 were ever "seen", and the extended rows above
and below stay shrouded however explored the game itself considers them.

That is the remaining defect behind "spotty fog" and it is a **memory**
problem, not a rendering one — the port is reading the wrong explored record.
Identifying the game's own record from the write side is the open task.
