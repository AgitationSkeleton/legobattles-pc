> **RETRACTED, 2026-09-04.** The central claim of this document -- that an
> out-of-band click selects -- is WRONG, and docs/224 records the controlled
> measurement that replaces it.
>
> The target here was the player's BASE, and this document's own warning
> about clamped clicks applies to it: the base's hit area covers native
> x 0, so a click that clamps to 0 hits it wherever it was aimed. The
> "4/12 select" is that artifact.
>
> Redone against the BUILDER -- which stands still, and whose hit area a
> clamp cannot reach -- at one camera position with the same delivery and
> offset either side of the boundary: in band `hud 0 -> 6 SELECTED`, out of
> band `hud 0 -> 0 nothing` with the slot holding -38. The coordinate
> arrives; the cartridge does not act on it.
>
> The measurements below are left as they were taken. Only the conclusion
> is withdrawn.

# 218 — clicking out of band works, and two features were off by default

**Goal this serves.** Touch input has to work beyond the original aspect:
clicking, selecting units, commanding units, clicking and dragging.

## The result

A click on the player's **base** at native screen x **−49** and **−25** — well
outside the cartridge's own 256 columns — selects it. The top screen names what
was selected: **"Eagle Command 1500/1500"** with its build panel.

```
LB_TOUCHEXT=1 :  OUT band 4/12 selected  at native x [-49, -25]
LB_TOUCHEXT=0 :  OUT band 0/12 selected
```

Same camera position, same target, same twelve clicks, the base parked at screen
x −61. The extension is what makes it work.

docs/214 and docs/217 both concluded it did not work. **Both were wrong**, and
each for its own reason.

## Why it took three attempts to see

**1. The probe could not click.** docs/217: `nds_frontend_debug_touch` refuses
any x ≥ 256 and remaps through the vanilla layout, so the harness's clicks never
reached the frontend — not the out-of-band ones, and not its own control.

**2. The reported position is not the clickable one.** The port reports an
entity's own position; a sprite is drawn at an offset from it, and a building's
origin is nowhere near its middle. Measured: clicks at the base's reported
position at screen x **123 and 43 — both inside the native band —** selected
nothing. A probe that cannot select its target *in* band says nothing about out
of band. The fix is not to guess the offset but to sweep a grid across the
sprite: if **any** in-band point selects and **no** out-of-band point does, the
boundary is the cause and nothing else can be.

**3. A clamped click lands at native x 0.** This is the subtle one. Without the
extension the frontend clamps the guest touch into 0..255, so an out-of-band
click arrives at x 0 — and a target whose hit area *covers* x 0 is then hit
either way. Measured, and it is exactly what happened: with the base at screen x
−21 and the extension **off**, clicks at −9 and −1 "selected". That is not
out-of-band clicking working; it is a clamp getting lucky.

So the target has to be parked where a clamp cannot reach it. At screen x −61
the base's whole hit area is far left of 0, and the 4/12 versus 0/12 above is
what that gives.

## Panning between rows is not a control

An earlier version panned the camera between clicks and clicked the base at each
step. It reported the same screen x as both SELECTED and refused, because
everything moves when the camera does: the calibrated offset drifts off the
sprite and the deselect misses. One camera position and one target, with only
the click position varying, is the design that answers the question.

## And two features were off by default

Chasing this turned up something worse than the bug.

**`LB_WIDEMAP` was not a diagnostic switch. It was the widescreen feature.**
Everything in `lb_widemap.cpp` was written and measured with `LB_WIDEMAP=1` set
by hand, so the DEFAULT experience was never the one being looked at. Measured
without it at 1920×1080 (`screenshots/exp-nowm/game-1920x1080.png`): the field
is **pillarboxed to roughly the native width with black bars either side**, and
a **band of stale tiles runs across the top** — the exact two things that file
exists to prevent.

The design is explicit that adaptive window size is ONE toggle whose off
position is pure-vanilla rendering. A second, hidden, default-off switch in
front of half the feature is precisely what that rules out.

Both now default ON and follow **Enhanced**, which is the one toggle:

* `lb_widemap.cpp`'s `stage()` defaults to 1, and `lb_widemap_fill` returns
  early unless `lb_setting_enhanced()`.
* `apply_touch_extension` no longer requires `LB_TOUCHEXT` or `LB_WIDEMAP`, only
  Enhanced.
* `LB_WIDEMAP=0` and `LB_TOUCHEXT=0` still turn each off, which is how the
  control above was run.

Verified with **no environment variables at all**: the field fills the window
with no bars and no stale band, `[lb_terrain] 0/768 differ`, the five touch
patches report applied, 42 fps — and an out-of-band click selects the base
(3/12 at native x −57, −33, −25).

## The lesson, which is the same one as docs/217

A default that no measurement has ever exercised is not a default anyone has
tested. Every good capture in this project set `LB_WIDEMAP=1` explicitly, so the
picture a player would actually get had gone unlooked-at for the whole feature's
life.


---

# Drag-select does NOT cross the boundary, and clicks do

Both are required: tapping or commanding a unit, and clicking-to-drag to
select multiple units. They are different code paths in the cartridge and they
behave differently.

`--drag-select` synthesises a real drag — a button-down, a path of absolute
motion events, a button-up — because the frontend only updates the guest touch
on motion **while the button is held**, so two clicks at the ends are not a drag.
`frontend_input action="moveto"` was added for the path.

The measurement uses the same design as the clicks: park the base, draw a box
around it, and change only which side of the boundary the box is on.

| box | base parked at | result |
|---|---|---|
| native (53,9)–(111,73), wholly IN band | screen x 59 | **3/3 SELECTED** |
| native (−67,9)–(−9,73), wholly OUT of band | screen x −61 | **0/3** |

And the drag genuinely reaches the guest with the sign intact — the touch X path
for the out-of-band box:

```
touch X saw 19 distinct: [-65, -61, -58, -55, -52, -48, -45, -42, -39, -36, -32, ...]
```

So: **box-select exists, works in band, receives correctly-signed out-of-band
coordinates, and still selects nothing out there.** That is a much sharper
statement than "dragging does not work", and it rules out everything upstream.

## Where that leaves it

`func_020a8794` builds two things from the touch and they are not the same:

* the CLICK point, `[obj+0x20]` / `[obj+0x22]`, written from `ldrh` — and
  clicking works;
* the RECTANGLE, `[obj+0x10]`/`[obj+0x18]` (x) and `[obj+0x12]`/`[obj+0x1a]`
  (y), normalised by `func_020aa8e4` from the **signed** reads at `020a87fc`
  and `020a8800` — and dragging does not.

So the rect is stored correctly and something that CONSUMES it is not. That is
the next measurement, and it is a narrow one: watch reads of `[obj+0x10]` and
`[obj+0x18]`.

Two false starts worth keeping, because both produced confident wrong readings:

* A full-screen in-band drag caught **nothing**, which looked like "box-select
  does not exist". The touch path showed 19 distinct X values sweeping across
  it, so the drag was real — there was simply none of the player's own on the
  native screen at that camera position. A control that can fail for want of a
  target is not a control, which is the third time this project has met that.
* The first out-of-band box started at native −88, whose window x is **negative**
  on a 392-wide surface — outside the field rect, so the frontend rejected the
  button-down and the drag began wherever the pointer re-entered. The band is
  only as wide as the surface is.
