# 294 — A shift finder cannot measure periodic terrain, and the 6-px gap is withdrawn

Commit `41bcdca8` recorded a discrepancy: a camera nudge verified at **+150
guest px** produced a picture shift measured at **-288 window px**, where 2x
magnification predicts -300. Six guest pixels unaccounted for, with the band
split named as the likely suspect.

**Both halves are now resolved, and neither the way it was going.**

## The band is innocent, and now provably paired

The band could not be blamed or cleared before, because `[lb_band]` prints on
change plus periodically and there was no line for the capture's surface at the
post-nudge camera. `--nudge-cam` now waits for a band line whose `cam` matches
and prints both ends:

```
nudge: camx 648 -> asked 798, now 798 (held)
nudge: band BEFORE at cam 648 -> 786x376 left=265 right=265
nudge: band AFTER  at cam 798 -> 786x376 left=265 right=265
nudge: band delta +0 guest px -> picture should move -300 window px
```

**Band delta +0.** The split did not move, so it cannot explain any gap. That
much is now measured rather than assumed, and the pairing is by construction
rather than by two cadences coinciding.

## But the gap itself was never real

Re-measuring the shift on that pair returns something the previous run's
narrower search hid:

```
measured shift -336 window px (residual 0.0)
five best candidates: -336(0.0), -288(0.0), -335(18.7), -287(18.7), -337(18.8)
```

**Two different shifts both score a residual of 0.0** — a perfect pixel match —
and they are **48 window pixels apart**. At 2x magnification that is 24 guest
pixels: the metatile, and exactly the 3-tile period `LB_TILECHK` reported
independently (`000, 1E0, 012`).

The terrain repeats at that period, so a shift finder run over terrain cannot
distinguish shifts differing by a multiple of it. It locks onto the lattice, not
the true displacement. `-288` was not a measurement of the picture's movement;
it was the lattice point nearest the bottom of the search range the first run
happened to use.

**So the six-pixel discrepancy is withdrawn.** There is no established gap
between the camera delta and the picture's movement.

## Why the instrument passed its control and still failed

This finder *was* validated first, on known offsets -64, -16, 0, +24 and +96,
recovering all five exactly (docs/290's addendum). That control was sound and it
is not the reason this went wrong.

The control used **one image shifted against itself**, which keeps every
non-periodic feature — units, buildings, fog boundaries — present in both
halves, and those anchors pin the lattice. The real pair has a camera move
between the frames, so fog is revealed, units have walked, and the anchors that
made the control work are weaker or gone. What is left is repeating terrain,
and repeating terrain has no unique answer.

**A control has to exercise the property that will differ in the real case.**
Shifting an image against itself does not test robustness to changed content,
which is the whole difficulty of the real measurement.

## What would actually measure it

Anchor on something that does not repeat. The port already logs entity screen
positions (`[lb_builder]`, `[lb_base]`, `[lb_hero]` with `sx=`), and an entity
is unique — so its `sx` before and after a verified camera nudge gives the
picture's displacement directly, with no correlation and no lattice. That is
one number from the log rather than a search over pixels, and it is immune to
everything this document is about.

## Status

* `--nudge-cam` holding a precise, verified camera delta: **established**.
* Band paired to the capture, delta +0 on this pair: **established**.
* A gap between camera delta and picture movement: **withdrawn, not established**.
* The crossing test for the band: **still not run**, and it needs the entity
  anchor above rather than a terrain correlation.
