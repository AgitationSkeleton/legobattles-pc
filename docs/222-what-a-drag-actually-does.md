# 222 — What a drag actually does

**Goal within the project:** the expanded field of view on the touch screen
must be just as interactable as the original aspect: tapping or commanding a
unit, click-dragging to select multiple units, and so on.

Note the standard: *just as interactable as the original aspect*. The test is
therefore a comparison, and this document is mostly about how easy it is to
skip the control and how much that costs.

## The probe that could not fail correctly

`--drag-select` draws its box around the player's **base**. A base is a
structure, and a drag box in an RTS selects units — so "caught nothing" is what
a perfectly correct game would do, and the probe could not tell that apart from
a broken one. That is "a control that can fail for want of a target", the same
mistake docs/217 and docs/218 each turned on.

`--drag-unit` replaces it. It drags around the player's **hero**, twice at one
camera height in one session: once with the hero parked inside the native 256
columns, once with it parked in the widened band. The control decides whether
the test is allowed to mean anything.

## Three things the control found, in order

**1. The reported position is not the clickable one.** A click at the hero's
`[lb_hero] sx, sy` selected nothing. docs/218 had already seen this on the base,
where the clicks that worked were at sx+12 and sx+36.

**2. `tap` selects and `frontend_input` does not.** One target, one frame:

```
drag-unit: control  TAP       at (147,95)  hud 0 -> 5  SELECTED
drag-unit: control  FRONTEND  at (147,95)  hud 0 -> 0  nothing
```

`tap` is the debug server's `touch` command, which takes guest coordinates.
`frontend_input` takes window pixels and the frontend converts them back. The
window mapping here is the exact inverse of the frontend's, the command returns
`ok: True`, and the gate (`lb_overlay_mouse_in_touch_area`) accepts the point —
and still nothing selects. **Twelve calibrated offsets were tried, including the
nine that `--click-sweep` uses, and none of them selected the hero in band.**

This matters beyond this probe: `frontend_input` is the delivery docs/218 used
to conclude that out-of-band clicking works, and the comment in `lb_patches.cpp`
cites that result to justify defaulting the touch extension on. That conclusion
now needs re-checking with a control that is known to be able to succeed.
**Recorded here rather than acted on, because acting on it would mean changing a
default on the strength of one probe, and the last two times this project did
that it was the probe that was wrong.**

**3. The drag reaches the guest, and does nothing.** With `LB_TOUCHLOG=1` the
port prints what it publishes while the pen is down, and during the control drag
it published a sweep across the box:

```
[lb_touch] published 133 -> slot=133  struct +0A=128 ... native=133
[lb_touch] published 164 -> slot=164  struct +0A=160 ... native=164
[lb_touch] published 186 -> slot=186  struct +0A=186 ... native=186
```

The box was native x 107..187, so those are the drag's own coordinates arriving
at the cartridge's touch struct. The hero's `sx` also held steady at 155 right
through the drag, so the drag did not pan the camera either.

## What that adds up to

**In the native aspect, a delivered stylus drag around a unit selects nothing
and pans nothing.** That is the control, inside the original 256 columns, with
no part of this port's widening involved.

So the widened band is **not implicated** by the band trial failing — it failed
identically to the control. The goal is that the band be *just as
interactable as the original aspect*, and on this operation the two are
currently equal.

What is NOT established, and must not be written down as if it were:

- whether this game has box-selection at all on the field;
- whether it needs a modifier, a different start (on a unit rather than on empty
  ground), or a slower or faster sweep;
- whether `frontend_input` is a sound delivery for any click conclusion.

docs/214 read `func_020aa8e4` as building a selection rectangle at `obj+0x10` /
`obj+0x18`, which is evidence that rectangle logic exists somewhere; it is not
evidence that a field drag reaches it.

## How this gets settled

By a human player, in one recording. The demo recorder round-trips now (docs/221), and
the one thing every probe in this project has had to guess — where a player
actually puts the stylus and what they expect to happen — is the one thing a
recording does not guess.

**What to record:** a drag around two or more of your own units inside the normal
screen area, then the same drag with units out in the widened band. That single
demo answers whether box-select exists, what a correct one looks like, and
whether the band matches — and it replays against every future build.
