# The widener runs; the width is still 256

> **The central claim here is WRONG, corrected by
> [docs/127](127-widescreen-menus-verified.md).** The buffer was never 256 wide:
> the debug server returns the width as `w`, not `width`, and this session's
> capture helper defaulted to 256. The 448x192 image was reshaped as 256x336 --
> and 448*192 == 256*336 was visible in the output. adaptive_width = 448 worked
> from the moment it was added, and menus widen correctly.

An attempt to verify `lb_menu_fill` on screen, which turned into finding why
nothing is widened at all.

## What is now known about the verification path

* **`lb_setting_enhanced()` defaults to true**, so Enhanced is not the blocker.
* **The adaptive path only exists in the interactive frontend.**
  `nds_gpu3d_set_output_width` is called from `frontend.cpp` and nowhere else, so
  under `--serve` alone `nds_gpu3d_output_width()` stays 256 and
  `nds_gpu2d_adaptive_framebuffer` returns the native buffer before reaching any
  widening code. Every headless capture in this session was therefore incapable
  of showing widening, whatever the code did.
* **The title's declared capability is `bottom`.** Passing
  `--adaptive-widescreen both` is refused with *"unsupported by this title
  (supported: bottom); refusing to stretch the native framebuffer"*.

## The widener does run

From an interactive launch:

```
[sdl] layout=stacked adaptive=bottom supersampling=1x aa=0x internal=1x
[lb_wide2d] engine 1: WIDENING            (dispcnt 00010100)
[lb_wide2d] engine 0: engine A 3D scene   (dispcnt 00010D0B)
```

Both lines are correct and worth keeping:

* engine 1 -- the top screen -- is accepted and widened by `lb_wide2d`;
* engine 0 -- **the field**, which `docs/114` established is engine A -- is
  refused with "engine A 3D scene" and left to the upstream 3D-aware path, which
  is right: its `dispcnt 00010D0B` has bit 3 set and mode 1, exactly the
  `mode == 1 && bg0_3d` case that path was written for.

So the compositor selection is behaving as designed.

## But the buffer is still 256 wide

Captured through the debug server with `adaptive: true`, both screens come back
**256** wide. The chain is:

```
frontend.cpp    screen_widths[s] = options.adaptive_max_width[s]
                   ... only when (adaptive_screens & bit) && (adaptive_supported & bit)
frontend.h      adaptive_max_width[2] = {256, 256}      <- the default
frontend_config adaptive_max_width set from display.adaptive_width,
                   or 448 when the key is absent
```

`adaptive_widescreen` and `adaptive_capability` are both read from
`port/game.toml` and both take effect -- the banner says `adaptive=bottom` and
the CLI rejects `both` against the capability. Adding
**`display.adaptive_width = 448`** to the same table changed nothing, and the
width stayed 256.

That is the contradiction to resolve next: two keys from that table are honoured
while a third in the same table appears not to be. The likeliest explanation is
that the frontend config is loaded from a different file or a different table
than the title config `--config` points at, and that the two adaptive keys are
reaching the frontend by another route.

## Status of lb_menu_fill

Still **unverified**. It is built and wired into both letterbox tails, but with
`output_width == 256` the adaptive function returns before any of that code is
reachable, so nothing has exercised it. No claim is made about how it looks.

The `display.adaptive_width = 448` line added to `port/game.toml` is kept: it is
correct, it is the runner's own maximum, and it costs nothing while the plumbing
question is settled.
