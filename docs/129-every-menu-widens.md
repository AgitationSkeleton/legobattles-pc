# Every menu widens; only the field letterboxes

> **The in-game HUD row is corrected by
> [docs/130](130-the-hud-was-being-tiled.md).** "in-game top screen: widened"
> was measured by margin blackness and was wrong: the HUD was being TILED, so
> the widened columns showed a duplicate gold counter, population readout and
> minimap blip. The menu rows below stand.

Walking the full `tools/lb_warp.py` route headless at `LB_FORCE_WIDTH=448`,
measuring what fraction of each 96-pixel margin is black:

```
step                          bottom          top
boot to title                   0% black        0% black
title: touch to start           0% black        0% black
profile: Create                 2% black        0% black
name: Q / W / E / Ok            1% black        0% black
menu: Single Player             1% black        0% black
single: select FreePlay         1% black        0% black
single: enter FreePlay          1% black        0% black
map select: Continue            2% black        0% black
rules: Continue                 3% black        1% black
army: Continue                  2% black        0% black
opponent: Start               100% black      100% black    <- a fade, not a letterbox
settle in game                100% black        0% black
```

**Every menu screen in the route widens on both screens.** The 1-3% readings are
dark pixels in the artwork itself, not bars: a letterbox reads 100%, as the last
row shows.

`opponent: Start` reads 100% on *both* screens, which is the black transition
between army select and the match, not a letterboxed scene -- the very next
sample has the top screen back at 0%.

**In game**, the split is exactly as `docs/128` found: the **field letterboxes**
and the **top screen still widens**.

## lb_menu_fill is exercised, and works

The widener's own log for this run:

```
[lb_wide2d] engine 0: engine A 3D scene (dispcnt 00010D0B)   <- the field, deferred
[lb_wide2d] engine 1: WIDENING          (dispcnt 00011F10)
[lb_wide2d] engine 1: a non-text BG     (dispcnt 00011C13)   <- refused
[lb_wide2d] engine 1: WIDENING          (dispcnt 00010810)
```

One engine-1 scene is **refused** as "a non-text BG" -- an affine or extended
background, exactly the case `lb_wide2d_supported` cannot extend truthfully and
exactly what `lb_menu_fill` was written for. A refused scene now routes to the
fill rather than to the letterbox, and no top-screen sample in the whole route
reads as letterboxed.

So `lb_menu_fill`, recorded as unexercised in `docs/127`, **is** being used and
does keep the affine scenes filled.

## Standing state of the widescreen work

| | at 448 wide |
| --- | --- |
| all menu screens, both engines | **widened** |
| affine/extended menu scenes | **filled** by `lb_menu_fill` |
| in-game top screen (HUD) | **widened** |
| in-game field | **letterboxed** -- no tile data exists (`docs/117`, `docs/120`, `docs/128`) |

Everything the widescreen goal covers is now working except the game field,
and the field's blocker is measured, specific, and documented rather than open.
