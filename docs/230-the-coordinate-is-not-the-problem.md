# 230 — The coordinate is not the problem

**Goal within the project:** adaptive widescreen's interaction requirement —
the expanded field of view on the touch screen must be just as interactable as
the original aspect: tapping or commanding a unit, drag-selecting multiple
units, and so on.

docs/228 ended by saying the automated probe could not settle this, because
every attempt went through SELECTION and the synthetic click path was never a
reliable enough control: a band failure and an instrument failure look
identical. That conclusion stands for selection. It does not have to stand for
the whole clause, because the clause has a half that can be measured cleanly.

## Splitting the question

"Can you interact with the band" is two questions stacked:

1. Does a click at a window position **arrive** at the cartridge as the right
   guest coordinate?
2. Does the game **act** on a coordinate outside its own 256-column view?

(1) is arithmetic with a definite answer. It needs no unit to be present, no
camera to settle, no HUD to appear and no selection to succeed — so none of the
four instrument faults catalogued in docs/228 can reach it. It also covers the
whole surface rather than one target.

And it forks the search. If the coordinate is wrong, selection cannot work and
there is a concrete bug to fix. If it is right, the coordinate is excluded and
everything left is downstream — which is the fork docs/228 could not resolve.

## The measurement

`lb_exp.py --touchmap` sweeps eleven window columns across the full field rect
at each of six window sizes, clicks each one through the frontend (window
coordinates, the same path a player's mouse takes), and reads the cartridge's
own touch X at `0215376A` — the field docs/224 traced the port's write to and
docs/225 enumerated every reader of.

Expected value is a plain scale about the field rect, minus the band:

```
guest_x = (window_x - field_x) * guest_w / field_w
native_x = guest_x - (guest_w - 256) / 2
```

**66 of 66 columns, at every window size, land within two pixels of that**, from
native −345 on the far left to +506 on the far right:

```
touchmap: 640x480   guest 306x216 band  25 -- 11 of 11 columns map correctly
touchmap: 800x600   guest 386x276 band  65 -- 11 of 11 columns map correctly
touchmap: 1024x768  guest 498x360 band 121 -- 11 of 11 columns map correctly
touchmap: 1280x720  guest 626x336 band 185 -- 11 of 11 columns map correctly
touchmap: 1600x900  guest 786x426 band 265 -- 11 of 11 columns map correctly
touchmap: 1920x1080 guest 946x512 band 345 --  9 of  9 columns map correctly
```

(Nine at the largest size because the surface is marginally wider than the
window there — the cover crop — so the two outermost sample points fall outside
the window and are skipped rather than clicked at a coordinate no mouse could
reach.)

## The instrument fault this run found, and how it was excluded

The first version of the probe slept 120 ms after the click and read once. It
reported three MISMATCH rows at each of the two smallest window sizes — and
every reported value was **exactly the previous row's**: `5, 5, 5` and
`205, 205, 205`, with one row at 800×600 returning `280`, which was the last
value of the 640×480 sweep before it.

That is a stale read, not a mapping fault, and reporting it as a mapping fault
would have been the fifth time this instrument produced a confident wrong answer
in this project.

The probe now polls until the value moves off the previous reading rather than
sleeping a fixed time. This is self-correcting in both directions: it cannot
return a stale value, and where two adjacent columns genuinely map to the same
coordinate it times out and returns that same value — which is the right answer
anyway. Every row now reports its poll count, so a run where the game is slow
says so instead of quietly degrading.

## What this settles, and what it does not

**Settled: the coordinate is not the problem.** A click anywhere in the widened
field, at any window size, reaches the cartridge's touch struct as the correct
signed guest coordinate. Combined with docs/225 (every reader sign-extends) and
docs/226 (the entity gather rect is widened, so the entity is in the list), the
delivery path is measured end to end and is correct.

**Not settled: whether the game acts on it.** docs/228 traced the selection
chain to `FUN_020e2a04`, which filters by ownership and is not a coordinate
test, and the decision is upstream of whatever fills that routine's list.

The next hypothesis is now a specific one rather than a search, and it comes
from docs/197's table of the three mechanisms that clamp to the 256-column view.
Two of them have been moved — the tilemap, by this port's own fill, and the
entity gather rect, by `apply_cull_width`. **The third has not.**
`Set_View_Dimensions` at `020333A4` still says `mov r1, #0x20`: thirty-two
tiles, 256 pixels. `apply_view_width` can rewrite it, but it is gated on
`lb_setting_view_width()`, which returns 0 unless `LB_VIEW_WIDTH` is set in the
environment — so on every ordinary run, and on every probe run in this project's
history, **the game's own view has stayed 256 pixels wide while everything
around it grew.**

If the picking test is against the view rectangle, that is the refusal, and it
has been sitting behind a defaulted-off environment variable the whole time.
That is a testable claim and it is the next thing to test.
