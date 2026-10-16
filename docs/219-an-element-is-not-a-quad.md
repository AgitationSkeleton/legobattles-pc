# 219 — An element is not a quad

**Goal within the project:** adaptive window size. Specifically HUD
repositioning, and specifically this defect, seen in a screenshot: with the
base (Eagle Command) selected, the cancel HUD element (bottom-left) is not
visible and the two brick sidebar elements float away from the window edge.

Both halves of that defect are one bug, and the bug is a category error in
`lb_hud_scan`: it treated a QUAD as an ELEMENT.

## What the anchoring does

When the window is wider and taller than the DS, the field grows but the HUD
does not. The in-field HUD is drawn by the 3D engine as textured quads at fixed
native coordinates, so a quad that sat against the left edge of a 256-wide
screen now sits 68 px inside a 392-wide one, stranded in the middle of the
field. `lb_hud_scan` finds those quads, works out which window edge each was
against, and offsets it by the shift so it rides the new edge instead.

The offset is decided per quad: if the quad's native x0 is within a few pixels
of 0 it gets `-shift_x`, if its x1 is near 256 it gets `+shift_x`, and the same
test runs on y against 0 and 192.

## Why that tears an element in half

The sidebar brick is not a quad. It is four quads in a 2×2 block:

```
[lb_hud]   x 71..79  y 26..50   -> -68,-10
[lb_hud]   x 79..87  y 26..50   -> -68,-10
[lb_hud]   x 71..79  y 50..74   -> -68,+0
[lb_hud]   x 79..87  y 50..74   -> -68,+0
```

Read the third column. The top two quads were moved up by 10 and the bottom two
were not moved at all, because the top pair's native y0 of 16 is within the
top-edge slack and the bottom pair's y0 of 40 is not. The brick was pulled apart
down its middle and a 10 px gap opened inside it. That is the "floating away
from the window edge" — the halves float away from each other and from the
edge, at different amounts, so neither half lands anywhere sensible.

The horizontal axis had exactly the same latent bug and never showed it. The two
COLUMNS of the brick share a y range, so the two ROWS share an x range, so the
horizontal test always agreed across the pair by accident. A one-column element
would have torn horizontally the same way.

## The fix: group, then decide once

`lb_hud_scan` is now two passes.

The first pass collects every on-screen HUD quad into a small array. The second
merges them with union-find: any two quads whose rectangles touch or overlap
within 1 px of slack are the same element. Then, once per GROUP, the code takes
the group's bounding box, runs the same edge tests it used to run per quad, and
applies the resulting `dx`/`dy` to every quad in the group.

An element now moves as a unit or not at all. There is no arrangement of quads
that can split one.

## The second half: a cap that was measuring the wrong thing

Grouping alone left the brick untorn but unmoved vertically:

```
[lb_hud]   x 71..79 y 26..50 -> -68,+0
[lb_hud]   x 79..87 y 26..50 -> -68,+0
[lb_hud]   x 71..79 y 50..74 -> -68,+0
[lb_hud]   x 79..87 y 50..74 -> -68,+0
```

Grouped, the brick is 48 native rows tall, and `kCornerMaxHeight` was 32, so the
element was declined for vertical anchoring and kept its native y — still
floating from the top of a taller window, just in one piece.

The comment on that 32 claimed a larger cap "would admit almost every quad in
the list — including the health bars that sit above units". That does not
survive inspection. A health bar is short and WIDE; a HEIGHT cap has never been
what excluded one. The z threshold excludes health bars, and does it exactly:
in-field HUD quads sit at min_z below 4300000 and unit-attached art sits above
it. The height cap was only ever excluding the brick.

Raised to 72, which admits the brick and still refuses the full-height side
panels (those want stretching, not moving).

## Measured, after

Same scene, window 472×256, shift 107,32:

```
[lb_hud] 5 corner element(s) moved, surface=472x256 visible=470x256 shift=107,32
[lb_hud]   x 112..119 y 48..72  -> -107,-32
[lb_hud]   x 119..127 y 48..72  -> -107,-32
[lb_hud]   x 112..119 y 72..96  -> -107,-32
[lb_hud]   x 119..127 y 72..96  -> -107,-32
[lb_hud]   x 112..135 y 200..224 -> -107,+32
```

All four brick quads agree on both axes and the brick lands in the top-left
corner whole. The fifth quad is the bottom-left element that was missing; it moves to the bottom-left corner and the surface dump confirms it is
on screen and inside the window.

## What this does not explain

A separate defect: sometimes the fog does not show, or the sides or even the
original-aspect field may corrupt.

Nothing here touches the fog or the band suppliers, so that defect is still
open and is not answered by this change. Recorded so this document is not
read as covering it.
