# 192 — The black bands are the 3D layer, and the bars are a fixed 448

Status: **measured**, on the real window and on the exact surface the frontend
uploads. Supersedes the guesses in earlier notes about banding
being a texture-upload artefact.

## First: two harnesses were lying

**The window was never being looked at.** Every measurement before this point
came either from a headless run (`SDL_VIDEODRIVER=dummy`, which skips rendering
outright) or from the debug server's `framebuffer` command, which returns a
GUEST SCREEN — 256×192, before the adaptive compositor runs. Neither can show
letterboxing, banding, or a misplaced HUD, which is precisely the list of
reported faults. `tools/lb_capture.ps1` captures the actual window with
`PrintWindow(PW_RENDERFULLCONTENT)`, at a requested size.

**The debug port was answering for a different game.** Port 19842 is the shared
ndsrecomp default on this machine, and a leftover `lsw2_bountyhunter.exe` — the
LEGO Star Wars II port — was holding it. `tools/lb_exp.py` connected to that,
drove it, and reported "route did not reach gameplay" about a game it had never
launched. It now uses a private port (19857) and fails loudly if its own child
dies. See `verify-the-capture-harness`: a known-broken measurement tool is a
prerequisite, not a lower priority.

Two smaller harness faults found the same way, both of which had been silently
discarding good runs:

* `in_gameplay()` asked for `engine="A"`. The debug server's `engine` argument
  selects a **screen**, and in this title engine A drives the BOTTOM screen — so
  it was censusing the HUD for terrain green and always failing.
* Its 30% green threshold rejected a real battle that measured 19%, because a
  fresh Free Play map is mostly unexplored fog. Now 15%.

## The black bands are the 3D layer

Measured on the dumped presentation surface itself (`LB_DUMP_ADAPTIVE`, hook
0m), not on a window screenshot — a screenshot has already been scaled by a
non-integer factor, and three different explanations fitted that blur equally
well. At 448 wide, identical across four frames:

    lit    cols   0..95    (96 wide)     <- left extra, and `extra` is 96
    BLACK  cols  96..191   (96 wide)
    lit    cols 192..266   (75 wide)
    BLACK  cols 267..351   (85 wide)
    lit    cols 352..447   (96 wide)     <- right extra

The lit outer bands are exactly `extra` wide, and 96/352 are the native region's
own edges. So the **native 256 columns are mostly black and the widened extras
are fully lit** — the opposite of every "the extras have nothing to draw"
explanation, and not compatible with an edge-carry fill either, since the native
edge columns are black and a carry would make the extras black too.

`LB_BGMASK=0x0100` clears DISPCNT bit 8 — BG0, the 3D layer — at render time:

    stock            lit 96 | BLACK 96 | lit 75 | BLACK 85 | lit 96
    BG0 (3D) off     lit 0..447, unbroken

**The black is the 3D layer.** With it off the frame is continuous terrain
across all 448 columns, and the castle and units are gone too. The 2D terrain
background widens correctly and always did.

> ## RETRACTED — the fog reading below was wrong
>
> This document originally concluded that the bands were **the fog of war**:
> BG0 drawing black over unexplored ground in the native view, with the widened
> extras unfogged because the guest submits 3D geometry only for its own 256
> columns.
>
> **That is not what they are.** Two later measurements killed it:
>
> * At `adaptive_width = 256` the widener is bypassed entirely
>   (`output_width <= 256` returns the native framebuffer early) and the field
>   has **no black bands at all** — it is continuous terrain. Fog covering the
>   native view would still be there.
> * With `adaptive_hud_anchor = false` at the same 448 the bands collapse to
>   `BLACK 0..95 | lit 96..351 | BLACK 352..447` — the native 256 intact and
>   unbroken, black only outside it.
>
> The real cause is in docs/193: **OBJ/BG anchoring was slicing the field
> itself**. 96 + 75 + 85 = 256 — the "lit extras" were the native view's own
> left and right slices, moved out to the edges, and the black was the hole
> they left behind. The ground texture gives it away before the
> measurement did: the flowers in the widened columns are the same flowers.
>
> `LB_BGMASK=0x0100` clearing the bands is still a true observation. It just
> does not mean what it was read to mean: BG0 is the layer the anchor was
> moving, so masking it removed the moved copies and the holes together.
>
> The rest of this document — the harness faults, the fixed-448 bars, OBJ being
> off in gameplay, the LB_VIEW_WIDTH negative result — was checked again after
> the anchor fix and still holds.

Gameplay DISPCNT is `00010F08`: **BG mode 0, BG0 = 3D, BG1/BG2/BG3 all text,
OBJ off**. The `00010D0B` line in the log (mode 3, BG3 extended) is a transient
load screen, and reading it as the gameplay scene is what made the widener look
like it was refusing in battle. It is not: the last state before gameplay is
`[lb_wideA] screen 1: WIDENING (dispcnt 00010F08, w=448)`.

**OBJ is off in gameplay.** So the in-field HUD is not sprites, and
`adaptive_hud_anchor` — which only moves OBJ — cannot place it. That is a
retraction of the assumption behind the HUD-anchor configuration in
`port/game.toml`.

## The left-edge corruption is the 2D path, not the 3D one

The striped column at the far left survives with BG0 masked off, so it is in the
text-BG widening — the case `lb_wide2d.inc`'s own header warns about, where the
decode walks past the columns the guest actually fills.

## The black bars are a fixed 448, by construction

`frontend.cpp` fits the guest surface into the window with

    lb_gw = presentation.screen_widths[1]        // 448, from game.toml
    lb_s  = min(win_w / lb_gw, win_h / 192)

`screen_widths[]` comes from `display.adaptive_width` and never changes, so the
source is **always 448×192, an aspect of 2.33:1**, and `min()` letterboxes every
window that is not that shape. Measured on the real window: content 908×390 in a
908×790 client — 2.33:1 exactly.

This is not tunable at runtime as things stand:
`nds_gpu3d_set_output_width()` returns false for any change once the accelerated
renderer is running, so the width is fixed at startup.

Two consequences worth stating separately:

* For windows **wider** than 4:3 the width must FALL, not rise: a 16:9 window
  needs `192 × 16/9 = 341` columns, and 448 is already too wide, which is why
  the bars appear top and bottom rather than at the sides.
* For windows **taller** than 4:3 no width helps at all. The surface is 192 rows
  and nothing in the adaptive path varies height (`AdaptiveFrame` is
  `kMaxAdaptiveWidth * 192`).

## LB_VIEW_WIDTH reaches the guest, and is not enough

`apply_view_width()` rewrites the guest's own `mov r1, #32` at `020333A4`. It
works, and the guest's structures prove it — the fog bitmask object at
`02151F28`, read in gameplay:

    field          stock       LB_VIEW_WIDTH=448
    +0x04 stride   4           7            (56 bits = 7 bytes)
    +0x08 width    0x20 = 32   0x38 = 56
    +0x0C height   0x18 = 24   0x18 = 24
    +0x14 rows     02245258    022452a0     (reallocated)

**And the rendered output is byte-identical** — the same five bands, to the
column. So this constant sizes the fog mask and nothing else that matters here;
the 3D submission and the tile fill are governed elsewhere and have not been
found yet. `LB_VIEW_WIDTH` is a negative result for the striping, recorded so it
is not re-tried as though it were untested.

## Frame rate

In gameplay, on the real window, state confirmed by capture at measurement time:

    42.5 fps   emu 0.217  present 0.017  adaptive 0.011  upload 0.000
               draw 0.001  swap 0.004    (ms/frame)

Menus sit at 59–60 fps; the title screen measured 59.3. So the deficit is
gameplay-specific. The instrumented phases sum to **0.25 ms of a 23.5 ms
frame** — over 99% of the frame is outside everything the frontend measures,
including `emu_ticks`, so the counter does not cover whatever the recompiled
code is actually doing. Where the time goes is not yet known, and the phase
breakdown cannot answer it.

One run of the same route with widening left stock measured 31.5 fps against
42.6 with `LB_VIEW_WIDTH=448`. Single samples of different runs vary far too
much to support any conclusion (docs/191 made exactly this mistake), so no cost
is claimed for either setting here.
