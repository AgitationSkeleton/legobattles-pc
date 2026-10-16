# 180 — The minimap mapping, measured from the game's own camera box

Status: **measured and wired.** Settles the requirement that the top-screen
minimap square representing where the camera is matches the window aspect
and what it covers, which docs/164 listed as untested.

## Why the cartridge's box is wrong once the field is wide

The game draws its minimap indicator for the view it can show: 256x192. That is
correct on a DS. It is not correct once the wide field is covering a 3440x1440
window with most of the battlefield in it — the box then marks a small square
inside ground the player is already looking at.

The port cannot fix this by widening the game's own box. The viewport object
docs/163 found is sized to the DS view, and the wide field is a **host-side**
renderer that the game knows nothing about, so nothing in the guest can be told
how much the window covers. The box has to be drawn by the port.

Which needs the mapping from map pixels to minimap pixels, and that was not
known.

## The measurement

Rather than assume the minimap is the map scaled to fit its panel, the mapping
was measured against **the game's own indicator**, which is the one thing on
screen guaranteed to be drawn by the cartridge's real transform.

The camera was swept across the level with the d-pad, and in each frame the
white indicator was located inside the minimap panel, giving 18 (camera, box)
pairs. Least squares:

    minimap_x = 0.059882 * camera_x + 135.269
    minimap_y = 0.091210 * camera_y +  39.683

Two results that could not have been guessed:

**The scales differ.** `1536 * ax = 92.0` and `1008 * ay = 91.9`: the minimap
squashes a 1536x1008 battlefield into a **92x92 square panel**. It is anamorphic,
not a uniform fit. Assuming a uniform scale would have put the box in the wrong
place everywhere except along one axis.

**The fit predicts the cartridge's own box.** `256 * ax = 15.3` and
`192 * ay = 17.5`, against **16.0 x 17.7 measured** across the sweep. That is the
independent check: the transform reproduces the thing it was fitted from, at a
size that was never an input to the fit.

### A contaminated first attempt, recorded because the failure was diagnostic

The first pass took the bounding box of every near-white pixel on the top screen.
It returned 187x131 and two mutually inconsistent scales (0.0556 and 0.0957). The
inconsistency is what exposed it: the HUD numerals along the bottom (`500`,
`1/4`, `0/0`) and the panel trim are white too. Restricting the search to the
minimap panel gave the clean 18-point sweep above. A single scale would have
looked plausible and been wrong.

## Wired

`lb_field_map_to_minimap()` carries the constants, next to the camera offsets it
depends on. `draw_wide_field()` publishes what the window covers in map pixels,
and `draw_top_screen()` draws that rectangle over the minimap panel whenever the
wide field is on — over the panel rather than into the guest's framebuffer,
because the top screen is the game's own output and the port does not repaint it.

Computed coverage, camera at the player base:

    window          map px covered        minimap box
    3440x1440       1536 x  643           92.0 x 58.6
    1920x1080       1536 x  864           92.0 x 78.8
    1280x720        1536 x  864           92.0 x 78.8
    stock 256x192    256 x  192           15.3 x 17.5      <- the cartridge's own

`screenshots/20260831-153206-minimap-box/box_3440x1440.png` shows the yellow box
spanning the whole minimap width with the game's small white indicator inside it.

Note the box's *on-screen* aspect is not the window's aspect, and should not be:
the region covered (1536x643) does have the window's 2.389 aspect, but the
minimap it is drawn on is anamorphic. Correcting for that would mean fighting the
cartridge's own minimap rather than reporting against it.

## Not done here

This says nothing about the shroud. The minimap still shows only explored ground
while the wide field shows everything (docs/179), so the box is now correct over
a map that is still wrongly revealed.
