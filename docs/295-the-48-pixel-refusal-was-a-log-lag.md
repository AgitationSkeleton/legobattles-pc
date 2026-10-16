# 295 — The 48-pixel refusal was a log lag, and entity positions are exact

Three commits chased a disagreement in `--nudge-cam`'s safety check. It is
resolved, and resolving it produced the first exact band measurement.

## The refusal

`--nudge-cam` writes the camera through the view pointer at `0x02152034`. Before
writing it compares that pointer's `+0x5E` against the camera the port reports,
and refuses when they differ by more than 32 — a guard against the pointer going
stale, which docs/174 recorded happening (it once read `camx -7889` on a
192-tile map).

It began refusing healthy runs, three in a row, with the two values **exactly
48 apart**: 584/536, 632/584, 616/568.

**A constant difference across independent runs is not staleness.** Staleness
would be arbitrary. Two wrong diagnoses were published before the right one:
first that the pointer was stale (the message's own wording), then that the
camera was still settling after the route's pan — an idea that survived two
samples and died on the third, since a race varies and this did not.

## It is the LOG that lags, not the camera

`port_camx` returns the newest `cam=` in `[lb_band]`, and that line prints **on
change plus periodically**. Its newest value is therefore behind the live camera
by however far the camera has moved since that line was written.

The refused run's log settles the matter: it *does* reach `cam=616`, the very
value the pointer reported, while `port_camx` had returned 568 from an earlier
line.

That also explains why an earlier fix could not work. Waiting for the logged
value to stop changing does not make a periodic log catch up to a live value.

**The check now asks the right question.** Not "does the pointer match the
latest logged camera" but "is the pointer tracking the camera the port has been
reporting at all" — accept if it is within 32 of **any** of the last twelve
logged values. A genuinely stale pointer misses every one of them by thousands
and is still refused.

## And then the measurement came out exact

With the refusal fixed, the displacement measurement runs — and its first result
disagreed by exactly the band delta, which is what identified the convention:

```
camera 536 -> 265 (clamped), delta -271
band   left 265 -> 261,      delta -4
base   sx -277 -> -6   moved +271, predicted +267   DISAGREES by +4
```

The error is `+4` against a band delta of `-4`. **`sx` is a GUEST screen
coordinate**, measured from the guest's own origin, so where the compositor
places those 256 columns inside the surface does not enter into it. Every
earlier run had a band delta of zero and so could not distinguish the two
conventions.

Dropping the band term, on a run where the band does move:

```
camera 616 -> 265 (clamped), delta -351
band   left 265 -> 261,      delta -4
base   sx -357 -> -6   moved +351, predicted +351   AGREES
```

Exact.

## What is established

* The camera can be moved by a precise, verified amount, or cleanly clamped by
  the game and **reported as clamped** rather than as a failed write.
* The band is paired to the capture by construction, not by two cadences
  coinciding.
* **An entity's screen position tracks the camera exactly**, including with the
  base at `sx = -357` — deep in the left widened column, far outside the guest's
  own 256. Whatever else is wrong in the bands, entity positioning out there is
  world-correct to the pixel.

## What is NOT

The crossing test still has not run. The base moves from `-357` to `-6`, which
is band to *the native edge*, never inside `0..255`. A crossing wants an anchor
that ends clearly within the native columns, and the camera clamps at 265 before
that happens on this map and route.

And this measures **where the port says the entity is**, not where its pixels
land. The two agreeing is likely but unproven; proving it needs the entity's
sprite located in the image, which a unique sprite makes tractable in a way
periodic terrain never was (docs/294).

## Addendum — the displacement is sound, the CAPTURE is not pinned to it

An attempt to confirm that the base's pixels land where its `sx` says failed,
and the failure is instructive rather than a defect.

Predicted position for the reported `sx = -6`, with band left 261 and the 14 px
window border at 2x magnification: surface 255, window 524. That spot is solid
shroud, and the frame's explored ground sits at the far left instead.

**The reading and the picture are different moments.** The log around the
capture reads, chronologically:

```
978 / -6      898 / -86      738 / -246      603 / -381
```

`-6` is the OLDEST of those, not the newest. `_base_sx` returned it correctly as
the newest line at the instant it was called — and the entity then kept moving
through the band-pairing polls and the capture that follow, so the frame shows a
later state. Predicting a pixel position from `-6` was asking the image about a
moment it does not depict.

**Also worth recording: there are TWO bases in that log**, one per player, a
constant 984 guest px apart in every sample (978-(-6), 898-(-86), 738-(-246),
603-(-381) are all 984). The `owner=0` filter keeps the readings on one of them
consistently, so the displacement result is unaffected — but anything that reads
`[lb_base]` without that filter will alternate between two entities, which is
docs/238's warning about `[lb_builder]` repeating on a different tag.

### What this does and does not touch

**Unaffected:** the displacement measurement of this document. It compares a log
reading against another log reading, both pinned by the byte offset, and the
camera delta between them is read back from the same source. `moved +351,
predicted +351` stands.

**Blocked:** verifying that the entity's *pixels* are where the port says. That
needs the capture pinned to a log line — the capture taken immediately after a
known `[lb_base]` line with nothing in between, rather than after several
seconds of polling. Until then the image cannot check the report.
