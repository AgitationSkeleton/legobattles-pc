# 292 — RETRACTED IN PART: the audit runs, but it does not yet measure anything

> **RETRACTION, same session, one tick later.** The title of this document was
> "…and on valid frames the bands are exact". **That claim is withdrawn.** The
> lifting of the dump out of the ImGui panel stands and is verified; everything
> below about what the audit *says* was read too quickly. The retraction is at
> the end, and the reasoning that produced the wrong answer is kept above it
> deliberately, because it looked sound.


docs/291 established that `tools/lb_bandaudit.py` could not run: its `lb_map`
reference dump lived inside the Battlefield ImGui panel, an open panel takes the
mouse, and the harness route then never reaches a battle. This lifts the dump
out and reports what the audit actually says.

## The change

`lb_map_dump_tick()` is now a standalone function called once per frame from
`lb_overlay_draw`, before any panel logic and independent of every panel's
visibility. Nothing in it wanted ImGui: `mw`/`mh` come from `lb_field_size`, the
view rect from `lb_field_camera_rect` and `lb_overlay_field_rect`, the pixels
from `lb_field_render`. The in-panel copy is removed, so there is exactly one.

Cost when off is one cached `getenv` and one integer compare. The expensive
parts — `lb_field_locate` scanning main RAM, `lb_field_render` decoding the
level — run only on a frame that is actually dumping, one in `LB_MAPDUMP`.

With the panel shut and the route intact:

```
lb_comp=40   lb_map=80 (40 ppm + 40 txt)   lb_layers=40
```

## THE AUDIT CARRIES ITS OWN CONTROL, AND IT HAD NOT BEEN READ

Every row reports a `guest` percentage alongside the bands. That column is the
cartridge's **own** 256 columns compared against the map, so it must be 100% by
construction — the guest drew it from that very data. Anything less means the
reference crop and the composited frame are not aligned on that frame, and every
number on the row is then meaningless, bands included.

Across 40 rows:

```
rows total 40 | usable (guest >= 99%) 7
guest control on the 33 unusable rows: 11.6% .. 54.3%
```

**Only seven of forty frames are a valid comparison.** On all seven:

```
LEFT 100.0% (8847)   TOP 100.0% (8623)   BOTTOM 100.0% (1092)   guest 100.0% (743)
```

Exact, on every band the frame has pixels for.

## What this does to the standing figure

`gpu2d.cpp`'s comments carry:

> the RIGHT band matches the map on 12-23% of its detailed pixels and the
> BOTTOM band on 36-55%, while the guest window is 100.0% and the LEFT band
> 99.9%

The BOTTOM figure of 36-55% sits inside the range the unusable rows produce here
(11.6-54.3%), and this run's own misaligned rows show BOTTOM at 20.4% and 58.3%
on frames whose guest control reads 30.5% and 54.3%. So the most likely reading
is that those numbers were taken from frames where the comparison was not
aligned, and the control that would have said so was printed but not used.

**Retracted as unsound rather than disproved.** The measurement may have been
made on a build or route where those rows were valid; what is established is
that the figure cannot be trusted without its control, and that on this build,
on every frame where the control holds, the bands are exact.

## NOT ESTABLISHED

**The RIGHT band is still unmeasured on a valid frame.** None of the seven
usable rows lists a RIGHT percentage — the tool prints a band only where it
finds detailed map pixels to score, and on those frames the right band has none.
So "LEFT, TOP and BOTTOM are exact" is the claim; RIGHT is untested, which
matters because RIGHT is the band the retracted figure was worst about.

**Why only 7 of 40 align** is not established either. The dump records its own
`cx, cy` at write time and the compositor's frame counter is shared, so the two
should pair; that 33 rows do not is a fact about the pairing or the camera rect
that has not been chased. A run that aligned on every frame would give forty
times the evidence.

## Relationship to docs/290

Not the same thing, and neither explains the other. docs/290's seams are a
tiling phase discontinuity measured directly from composited frames and
confirmed screen-locked by a camera pan. This is a content comparison against
the level data. A band can draw the *right ground* — scoring 100% here — while
its tiles are offset by a few pixels from the neighbouring region, because a
phase shift of less than a tile moves no pixel far enough to change which map
pixel it matches at this tolerance.

---

# THE RETRACTION

Two faults, either of which is enough on its own.

## 1. The seven "valid" frames are ONE frame, counted seven times

Every one of them reports identical pixel counts:

```
LEFT 100.0% (8847)  TOP 100.0% (8623)  BOTTOM 100.0% (1092)  guest 100.0% (743)
```

8847, 8623, 1092, 743 — the same four numbers on all seven rows. The camera was
still through the linger and the same dump pair recurred, so this is a single
measurement reported seven times, not seven independent confirmations. Counting
it as seven is what made the evidence look strong.

## 2. `guest = 100%` is an OUTLIER, not the aligned case

The control was checked against its own detail-pixel count across every row of
two runs, 28 distinct results:

```
detail=   409   guest= 17.4%       detail=  5407   guest=  0.0%
detail=   416   guest= 17.1%       detail=  6431   guest= 40.0%
detail=   732   guest= 28.7%       detail= 10810   guest= 54.3%
detail=   743   guest=100.0%   <-- the only one
detail=   849   guest= 11.7%       detail= 18845   guest= 17.4%
detail=  1022   guest= 49.9%       detail= 22868   guest= 12.3%
```

**One row in 28 reaches 100%.** And low detail does not explain it: 409 pixels
scores 17.4% and 849 scores 11.7%, while 743 scores 100%.

So "guest ≥ 99% means the comparison is aligned" was an assumption, and the data
does not support it. The honest reading of a control that sits at 12-54% on
essentially every real frame is that **the comparison itself does not reproduce
the guest's own output** — most likely because `lb_field_render`'s decode of the
level grid differs from what the game actually draws (lighting, tile variants,
palette) by more than the tool's detail threshold tolerates.

## What is actually established

**ESTABLISHED:** the `lb_map` dump now works with the panel shut and the route
intact — `lb_comp=40, lb_map=40, lb_layers=40` in one run, where before it was
0. That part of the previous commit is real and is verified.

**NOT ESTABLISHED, withdrawn:** that the bands match the map. One frame at one
camera position is not a measurement of that.

**ALSO WITHDRAWN:** the retraction of the 12-23% figure. docs/291 called it
undated and that was right; this document then called it "unsound", on the
strength of an instrument that has now failed its own control. The correct
status is what docs/291 said: **undated, and currently unmeasurable.**

## The rule this cost, again

`tools/lb_bandaudit.py` prints a control and I used it as a filter without first
asking what the control does on a frame that is known good. Had I plotted it
against detail before drawing a conclusion — thirty seconds of work, and the
data was already on disk — the single 100% among 28 would have been obvious.

This is the same shape as docs/250's stale probe, docs/288's periodic counter,
and the fps band of docs/289: **the instrument has to be established before the
reading means anything.** Three times in one session. The next number this
project acts on should come with its control's behaviour on a known-good case
stated alongside it.
