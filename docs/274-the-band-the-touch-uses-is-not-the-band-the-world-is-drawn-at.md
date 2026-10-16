# 274 — The touch band and the render band disagree, and the probe cannot see it

The defect: clicking and dragging the selection box, and clicking outside the
original aspect, did not work reliably. This is what was found, what was ruled out, and why the
fix is not in yet.

## The finding

frontend.cpp maps a window pixel to a guest coordinate by subtracting a band:

```c
// The guest still believes its screen is 256x192: extra
// adaptive columns and rows are extra VIEW, not extra touch,
// so the touchable band is 256x192 centred in the surface.
const float band  = (float)(gw - kScreenWidth) / 2.0f;
const float bandy = (float)(gh - kScreenHeight) / 2.0f;
```

That is the CENTRED split. The compositor does not use it: `band_compute`
CLAMPS at a world edge to keep the extension on-map, and the guest's own 256
columns are drawn at `lb_band_left()`. Measured in one frame, surface 1712x730:

```
  render band   [lb_band] left=540   (and 588, 234 in other frames -- it moves)
  touch band    (1712-256)/2 = 728
```

So what the player SEES and what the player CLICKS are measured from different
origins whenever the clamp is engaged. The error is zero when it is not, and
grows with it -- worst at a world edge on a wide surface, which is why it reads
as intermittent.

## The evidence that is not circular

`--dragbox` drags between two chosen native points and captures DURING the
drag, which nothing here could do before: a marquee exists only while the button
is held and every other capture is taken after the release.

With the port and the harness both on the centred band, a drag aimed at native
(20,30)->(240,170):

```
  cartridge received   (19,29) -> (239,169)      -- the round trip is exact
  marquee drawn at     native -475 .. -378       -- 494 px to the LEFT
  and it TRACKS: right edge -451, -426, -402, -378 against touch 44, 69, 93, 118
                 slope 1.0, constant offset -494
```

494 is the gap between the centred split and the clamped band in that frame. The
game draws the box at `touch + band_render`, which is correct for the game; it
is the touch that was interpreted against the wrong origin.

## Why --bandselect reported the opposite, and cannot settle it

docs/273 reported band selection working 4 of 4 on the direct detector. That
result CANNOT distinguish a right band from a wrong one, and the reason is
structural: the harness maps native->window with a band, the port maps
window->native back with the same band, and the cartridge receives the intended
coordinate. Selection then succeeds whenever the two sides AGREE, whatever they
agree on. The probe never touches the picture.

That is why every one-sided change broke its in-band control:

```
  port centred, harness centred   in-band 3 of 4     (agree)
  port centred, harness render    in-band 0 of 4     (disagree)
  port render,  harness render    in-band 0 of 4     (should agree -- it does not)
```

The third row is the open question. If the two sides were truly aligned it
should pass, and it does not -- most likely because the harness reads the band
from the last `[lb_band]` line while the port uses the live value and the camera
moves between them. That is a timing gap in the INSTRUMENT, not evidence about
the port.

## State

The port change is REVERTED and the deployed build is back on committed source.
Shipping a touch change whose only control is circular would be worse than the
defect: the error is invisible to the probe either way, so "it passed" would
mean nothing.

## What the fix needs

A control that involves the screen. The measurement above is one: put a unit at
a known place, capture the frame, find the unit IN THE PICTURE, click that window
pixel, and ask whether the cartridge received the unit's own coordinate. That is
the only form of the question that a shared wrong assumption cannot satisfy.

`--dragbox` plus the marquee already provides most of it, because the marquee is
drawn by the GAME from the coordinate it received, and is therefore outside the
round trip. The remaining work is to make the harness read the band at the
instant it clicks rather than from the tail of a log.
