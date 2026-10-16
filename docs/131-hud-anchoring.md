# HUD anchoring

## The request

The requirement: elements that touch a side or a corner of the screen should snap to the new edge or corner as the window is resized.

Stretching does not do this. It scales the icons and leaves every element at the
same *fractional* position, so a gold counter that was 4% from the left edge is
still 4% from the left edge -- further away in pixels, and blurrier. Anchoring
moves it to the edge.

## The implementation

`lb_hud_anchor` cuts the native 256 columns into three bands:

```
   native            output (448)
   [0 .. band)   ->  [0 .. band)                 anchored to the NEW left edge
   [band .. 256-band) -> centred                 stays centred
   [256-band .. 256) -> [W-band .. W)            anchored to the NEW right edge
```

with `band = (256 - center_width) / 2`, and `center_width = 96` for this HUD --
small enough that the left and right bands still contain the edge-anchored
elements, large enough to hold the population readout, which really is centred.

The two gaps this opens are filled by **replicating the column at the inner edge
of each band**, per row. For a HUD built from horizontal bars and panels -- which
this one is -- replicating a column continues the bar exactly, with no scaling
and no interpolation.

It is reached through `lb_menu_fill`, which dispatches on a flag
`lb_scene_is_hud` that `lb_wide2d_supported` sets when it refuses a scene
specifically for being a HUD over a 3D scene (`docs/130`). Menus keep the
stretch-or-fill behaviour; only the in-game HUD anchors.

## Result

In game at 448:

* **gold `446` sits flush to the new left edge**;
* **`0/0` sits flush to the new right edge**;
* the population readout `1/4` stays centred;
* icons are **unscaled** -- the numerals are as crisp as at 256, because no band
  is resampled;
* the panels and bars run continuously across the full width;
* one minimap blip, no duplication (`docs/130`'s fix still holds).

This is what the request asked for, and it is visible in the capture rather than
inferred.

## Known artefact

The gap fill flattens any **horizontal** gradient in the replicated column: the
bottom bar shows a faint band where the replicated region meets the original
art, because the source column is not a flat colour. Vertical structure is
preserved exactly, since replication is per row.

This is honest -- it continues the bar rather than inventing detail -- but it is
visible on close inspection. A smarter fill would choose the replication column
by looking for the most uniform column in each band rather than always taking
the innermost one. Recorded rather than fixed, because the current result is
already correct in the way the request cares about.

## Not covered

`center_width` is a constant, chosen by looking at this HUD. If another screen
in the game has a different arrangement of edge-anchored elements, 96 may cut
through something. Only the in-game HUD has been checked.
