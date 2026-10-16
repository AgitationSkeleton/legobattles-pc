# 246 — The corrected minimap box was unreachable, and the panel was a blank strip

Status: **fixed and photographed.** Follows docs/180, docs/244.

## The box existed and nothing could show it

`docs/180` measured the map→minimap transform, `lb_field_map_to_minimap()`
carries it, and `draw_top_screen()` draws a corrected camera rectangle with it.
That is exactly the feature wanted — the white square outline that represents
the field of view on the top-screen minimap, sized and shaped to what the
player is actually seeing — yet in play it never appeared.

It was gated on `g_wide_field && g_view_valid`. **`g_wide_field` is the
whole-battlefield panel**, a separate host-side renderer that is off in
ordinary play. The adaptive surface — the mode the widening actually runs in —
never sets it, so the condition was false every frame and the only indicator on
screen was the cartridge's fixed 16x18 one. The corrected box was measured,
documented, and unreachable.

The adaptive surface knows what it covers without that panel being up: the
guest's view starts at the camera, `lb_band_left`/`lb_band_top` say how far
left and up of it the surface reaches, and the surface's own size says how much
ground that is.

    x0 = cam_px_x - lb_band_left(output_width)
    y0 = cam_px_y - lb_band_top(output_height)
    x1 = x0 + output_width,   y1 = y0 + output_height

Same rectangle, taken from the renderer that is running. The wide-field path is
kept ahead of it, so nothing about that panel changes.

### Linkage, which is not cosmetic here

`gpu3d.h` carries no `extern "C"` wrapper, so `nds_gpu3d_output_width` and
`nds_gpu3d_output_height` are C++ symbols, while `lb_band_left`, `lb_band_top`
and `lb_widemap_cam_px` are defined `extern "C"` in `lb_widemap.cpp`. Declaring
either group the other way links against a name that does not exist.

## The panel was a blank strip, permanently

Every capture showed "Top Screen" as a ~4 px sliver. Not
collapsed — this port's own `imgui.ini` held

    [Window][Top Screen]
    Size=272,32

32 px is less than the title bar plus padding, so the content region came out
about zero tall, the fit scale went to zero, and the image drew at nothing.

The clamp in `draw_top_screen` only ever **shrinks** the panel — it exists to
rescue one that hangs off the edge of a shrunken window — so nothing could
recover a size saved too small, and imgui writes the bad size straight back out
each session. A player who once dragged it shut would have a dead panel for
good, which matters more than it sounds: in enhanced mode the window draws only
the bottom screen, so this panel is the **only** place the minimap appears.

`SetNextWindowSizeConstraints` now floors it at quarter scale. Quarter rather
than the default, so a player who deliberately made it small keeps a small
panel and simply cannot make it one that shows nothing; imgui applies
constraints to the restored size as well as to dragging, so an already-broken
ini self-heals on load.

## Measured

At a 996x720 window (magnification 2, so a 498x360 surface) the corrected box
should be `498 * 0.059882 = 29.8` by `360 * 0.091210 = 32.8` minimap pixels,
against the cartridge's `15.3 x 17.5`. The capture shows the yellow rectangle
at about twice the white one on each axis, with the white box sitting inside
it — which is the whole point: the player is seeing more ground than the
cartridge's indicator claims.

The cartridge still draws its own 16x18 box underneath. Suppressing it needs
the plotter, which is not OAM — every entry in both engines' OAM reads
`0x000000C0`, parked — so it is plotted into engine B BG tile data. The canvas
is around `0x06204640`; a watch over engine B's BG VRAM fills its ring from the
canvas clear at `0x01FF8260` in ITCM before the box is drawn, so finding the
plotter needs a narrower watch than that.
