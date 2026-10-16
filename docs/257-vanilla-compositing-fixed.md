# 257 — Vanilla compositing: two faults, both in the presenter

A deploy gate: with the enhancements set to off, vanilla rendering must still
work (and run well) before a build carrying new features and fixes is deployed.

It did not work. The guest was never at fault — the port's own dumps from the
broken runs are clean 256x192 — so both faults are in the SDL presenter, and
both were invisible in Enhanced, which is why they survived.

## Fault A: the source rect clamp only covered one screen

Every texture is created `screen_widths[screen] x 768` (kMaxAdaptiveHeight),
and `SDL_UpdateTexture` is handed the guest's own pitch, so only the top-left
part of it is ever real. A hook already existed to copy just that part:

```c
if (lb_present_src_w > 0 && screen == 1) { ...source rect... }
```

`screen == 1`. The top screen was copied **whole** — 768 rows of texture
holding 192 rows of picture — so those 192 rows were squeezed into the top
quarter of the destination and the remaining 576 filled with memory that had
never been written. In Enhanced the top screen goes to the ImGui panel and
nobody saw it; with Enhanced off both screens are drawn inline and it *is* the
picture: HUD strip, field strip, black band, and a block of RGB noise, all
inside one 192-row rect.

Fixed by making the extents per-screen (`lb_present_src_w[2]`,
`lb_present_src_h[2]`) and setting both at the upload site, where `top_width`
and `bottom_width` are already in hand. The height rule is not invented here —
the dump code a few lines above states it, having hit the same trap:

> "The height is only the adaptive one when the surface is genuinely wide; a
> native 256 buffer is a 192-row Frame and reading lb_dh rows from it would run
> off the end — the same fault as the top_width lie."

so `width > 256 ? adaptive_height : 192`, per screen.

## Fault B: the vanilla destination rect was sized to the adaptive maximum

`presentation.screen_widths[]` is assigned once, at presentation creation, from
`options.adaptive_max_width[]` — 512 here, the `adaptive_width=512` the banner
reports. It is an **option**, not the Enhanced setting, so with Enhanced off the
bottom screen still got a 512-wide destination rect while the guest drew 256
columns into it: a clean 2:1 horizontal stretch, and the two screens came out
at different widths.

Fixed by sizing those two rects from `lb_present_src_w[screen]`. Only the
non-Enhanced branch uses them — the Enhanced path computes its own a few lines
below — so the widened layout cannot be disturbed by it.

## Measured

Same route, same window, measuring each screen by its distinctive content (the
top screen's red HUD frame, the bottom screen's green field):

```
                       RED (top)              GREEN (bottom)
before both fixes      x 702..1213 (w=512)    x 462..1469 (w=1008)
after fault A fixed    x 702..1213 (w=512)    x 462..1469 (w=1008)
after both fixed       x 702..1213 (w=512)    x 710..1213 (w=504)
```

The 2:1 stretch is gone and the screens line up on the same right edge. The
noise is gone with fault A: counting high-contrast horizontal transitions
across the whole window, **8110 -> 2020 -> 2008**.

Vanilla runs 41.9 and 49.1 fps against Enhanced's ~35.

## Enhanced is not regressed

```
ENH before   field green x 12..1115 (w=1104) y 34..764
ENH after    field green x 12..1115 (w=1104) y 34..769
```

Identical width and left/right extent; the five extra rows are the capture
itself no longer being clipped (docs/256, fault 1). 35.7 fps, in the normal
29-38 band.

## Note on the earlier attribution question

docs/256 left open whether the recent changes caused the vanilla breakage.
They did not, and the question is now moot in a more useful way: both faults
are in hooks that predate them (the `screen == 1` clamp and the
`screen_widths` assignment), and neither is reachable in Enhanced, which is the
only mode that had been exercised. Nothing was measured against an older
build, and nothing needs to be — the defects are visible in the source.
