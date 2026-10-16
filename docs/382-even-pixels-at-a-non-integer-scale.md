# 382 — Even pixels at a non-integer presentation scale

Status: **fixed** (apply_hooks stanza 0zzzzzzzzzz). This is a candidate for
the "stretched" half of two reported defects: sprites, HUD and UI stretching
on resolution changes, and menu/UI elements stretched at some resolutions.

## What happened

The Enhanced field is presented with `SDL_RenderCopy` at the **cover** scale,
`max(window / surface)` in each axis, **nearest-neighbour**
(`SDL_HINT_RENDER_SCALE_QUALITY` "0").

* **Normally the scale is an integer.** The surface is sized from the window
  at the pixel scale (typically 2.0), so the scale is ~2.00; the gate's
  1473x830 gives 2.005.
* **Unless the map floors it.** "Never show more than the map" (frontend,
  `lb_world_extent`) raises the scale to `max(win_w/world_w, win_h/world_h)`.
  On a 64-cell map (1536x1024 px), any window wider than 3072 takes it: the
  a 3440-wide window gives **2.24x**, in every battle. Smaller maps start lower,
  and so does a pixel scale below 2.
* **What nearest does at 2.24x.** It gives 369 of 1536 columns and 148 of 616
  rows three screen pixels instead of two (a model of GPU nearest sampling
  run on a real 1536x616 composite). Lines and
  circles get uneven thickness and dither turns irregular, and as the camera
  scrolls each pixel changes width at the seams. That applies to the whole
  field, HUD included.

## The fix

When the cover scale is at least 0.05 from an integer, the field is drawn
**sharp**: nearest into a render target at the next integer scale, then one
linear pass down to the same destination rect. Every source pixel gets the
same width; only its edge is blended.

What is untouched:
* near-integer scales, including the usual 2.005;
* a source the player has set to linear filtering (antialiasing);
* the supersampling path;
* the destination rect itself, so the touch mapping, the HUD's anchors and
  the overlay are unchanged.

`LB_PRESENT=nearest` restores the old path. A `[lb_present]` line records
each engagement with its scale.

## Verification

* **A/B:** Free Play at `LB_WINDOW=1900x1000` with `LB_PIXEL_SCALE=1`. The
  floor gives a 1536x768 surface at 1.302x (`[lb_geom] ... scale=1.302`),
  which fits a 1920x1080 display.
  * The default run logs `[lb_present] field 1536x768 at 1.302x: sharp, an
    exact 2x prescale then one linear pass`; `LB_PRESENT=nearest` logs none.
  * The base (`screenshots/exp-shp382/win/sharp-ab.png`): with nearest, the
    radar dish's grid and the flowers are drawn in uneven 1- and 2-pixel
    runs. Sharp draws them even.
  * The window title read 60.3 fps.
* **Gate** (exp-gate382): counts identical to gate370b; all eight pictures
  byte-identical to gate381, since the change is after the composites. The
  sharp path stayed off through the replay at 2.005x and engaged only in the
  harness's closing resize sweep (1536x626 at 2.222x). 59.79 fps.
* **Vanilla** (exp-van382): 240/240 identical.

## Also this pass

* **Live resizes** (run rsz381). A held battle
  with the base's command list open was walked through 2022x900, 1200x1000,
  1900x640, 1000x1000, 2022x1100 and back (surfaces 996x426 ... 486x476).
  The first composite at each new size matched that size's settled frame in
  all four HUD corners (0 px differ). The periodic differences inside a size
  are the shroud's cloud animation. No transient stretch, move or omission.
  MoveWindow keeps windows on a 1920-wide display, so
  surfaces past 1024 need a launch size (docs/381's wrap runs).
* **`LB_CLIPREAD=ab`** alternates docs/381's native and scaled clip-matrix
  answers every 600 composited frames, for a before/after of a skinned model
  in one run. A replay of the model381 recording at 2022x1142 kept the
  transport ship off screen (`[lb_model] sx=-715`), and the harness stopped
  waiting at 900 s of the recording's ~980. So the before picture is still
  owed.
