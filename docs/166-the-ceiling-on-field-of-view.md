# The ceiling on field of view

The goal stated is a 1920x1080 or 3440x1440 window showing the battlefield, or
as much of it as fits. This records what the architecture can and cannot give,
because the gap is large and it is better known now than after more iterations.

## Two hard ceilings, one in the runner and one in the hardware model

**The runner caps at 448 guest pixels.** Three independent places:

```
gpu2d.cpp:53    constexpr int kMaxAdaptiveWidth = 448;
                using AdaptiveFrame = std::array<uint32_t, 448 * 192>;
gpu3d.cpp:796   if (width < 256 || width > 448 || (width & 1)) return false;
frontend_config if (*value < 256 || *value > 448 ...) "must be 256 through 448"
```

The adaptive framebuffers are **fixed-size arrays**, so 448 is not a policy knob
-- it is the allocation.

**The DS caps at 512.** A text background is at most 512 x 256 pixels
(`docs/113`'s BGCNT size field). The terrain reaches the screen through a text
BG, so **512 pixels of terrain is the ceiling of the hardware model itself**,
whatever the runner allows.

## What that means in numbers

| | width | vs native | aspect |
| --- | --- | --- | --- |
| native DS | 256 x 192 | 1.00x | 1.33:1 |
| runner ceiling today | 448 x 192 | 1.75x | 2.33:1 |
| DS hardware ceiling | 512 x 256 | 2.00x wide, 1.33x tall | 2.67:1 |
| a 3440x1440 window | -- | -- | 2.39:1 |

So the **most** the field of view can grow, staying inside the DS rendering
model, is **twice as wide and a third taller**. Presented in a 3440x1440 window
that is 512x214 guest pixels upscaled about 6.7x -- sharp, correctly shaped, and
showing double the battlefield width. It is **not** "the whole battlefield".

## The honest options

1. **Stay in the model, raise the ceilings to 512.** Needs the runner's three
   448s raised (they are ndsrecomp's, not this port's) and the map packing from
   `docs/117` solved. Ceiling: 2x wider, 1.33x taller. Everything else -- the
   game's own culling, fog, minimap -- follows from the viewport descriptor
   (`docs/164`) because the game does the work.

2. **Render the field host-side.** Read the level map and entity list directly
   and draw the battlefield at any resolution, bypassing the DS BG and 3D paths
   entirely. This is the only route to "as much as fits on screen", and it is a
   renderer, not a tweak: terrain, units, fog, particles, selection and the
   minimap all have to be reimplemented, and every one of them then risks
   disagreeing with the simulation.

3. **Upscale what exists.** 448x192 filling the window. More pixels, same field
   of view. Available today.

## Where this leaves the current work

`docs/164` reported that widening the viewport descriptor made the game fill a
wider area. **That does not reproduce.** Measured across 288, 352 and 448 with a
forced rebuild each time, fully-black columns are unchanged by the widen:

```
W=288  native=29   widened=30
W=352  native=93   widened=94
W=448  native=189  widened=190
```

189 black columns at 448 is 96 per side -- the letterbox, untouched. The single
capture `docs/164` rested on is not reproducible, and that claim is withdrawn.

The widener also refuses the scene outright: `[lb_wide2d] engine 1: HUD over a
3D scene`, which is `docs/130`'s guard doing its job on the top screen while the
field goes through the upstream path and letterboxes.
