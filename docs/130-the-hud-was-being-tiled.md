# The HUD was being tiled, not widened

## The defect

`docs/129` reported the in-game top screen as "widened" on the strength of its
margins not being black. Looking at the picture instead of the metric shows what
was actually happening: **the HUD was duplicated**. A second gold counter, a
second population readout, a second `0/0`, and the minimap blip appearing twice.

That is the in-game corruption beyond the native boundary seen earlier, still
present and now measured as a success by a metric that only asked "is the
margin black".

## Why

Extending a 256-wide text BG to 448 **is tiling it**. The map is 32 columns, so
the extra columns necessarily repeat what is already on screen. For a menu
backdrop that is correct and invisible -- a starfield simply continues. For a
HUD it duplicates content, because the content is not periodic.

Upstream says exactly this, in the header comment on
`nds_gpu2d_set_adaptive_bg_extend`:

> this should be on only while the 2D layers ARE the picture, and off while they
> are an overlay on a 3D scene, where extending them tiles the HUD across the
> frame.

The guidance was in the tree the whole time, attached to the sibling of the
function this port wrote.

## The fix

`lb_wide2d_supported` now refuses a 2D scene that is a HUD overlay on a 3D one:

```c
else if (engine != 0 &&
         (g_unit[0].dispcnt & 0x8u) && (g_unit[0].dispcnt & 0x100u))
    why = "HUD over a 3D scene";
```

The test is whether the **other** engine is running 3D, which is what separates
gameplay from a menu: in gameplay engine A draws the field with BG0 in 3D mode
(`docs/114`); in the menus nothing does. Confirmed live:

```
[lb_wide2d] engine 1: HUD over a 3D scene (dispcnt 00011F00)
```

Refused scenes fall through to `lb_menu_fill`, so the HUD is now filled rather
than tiled. In game at 448 it shows **one** gold counter (446), **one**
population readout (1/4), **one** `0/0`, with the panels widened and no
duplication.

## What this is and is not

It is a fix for a real visual defect: duplicated HUD elements are gone.

It is **not** HUD anchoring -- elements that stick to the sides or corners of
the screen snapping to the new leftmost, rightmost or corner positions. The HUD is currently **stretched** to the new width, which keeps one
of everything and looks reasonable for a HUD made mostly of panels and bars, but
it scales the icons rather than moving them to the edges.

Anchoring proper means splitting the native image into edge-anchored bands and
placing them at the new edges, which is what
`nds_gpu2d_set_adaptive_hud_anchor(enabled, center_width)` does for the upstream
engine-A path. The equivalent for this port's engine-1 HUD is the next step.

## Method note

The metric said "filled, not letterboxed" and that was true. It was not the
question. **A margin that is not black is not the same as a margin that is
right**, and only looking at the image distinguishes them. This is the same
lesson as the earlier histogram-versus-screenshot error, and it
recurred because the check was automated and the looking was not.
