# 312 — Drag-select: the drag control was not a control, and neither was mine

Two defects are open: drag-select across the boundary is buggy (the boundary
defect), and drag-selection may not select the right units (the wrong-units
defect). Both sit OPEN and unverified. This tick tried to answer the boundary
defect and did not, which is worth
recording precisely because the failure is instructive.

## `--drag-unit`'s control is not a control on this route

Its design is right: park the target inside the native columns, drag, then park
it in the band and drag again, so the boundary is the only difference. Run:

```
drag-unit: control  hero at (-6,63)  box (-46,23)->(34,103)  hud 0 -> 0  nothing
drag-unit: NO frontend offset selects the hero in band --
           the band result below cannot mean anything
drag-unit: band     hero at (-53,63) box (-93,23)->(-13,103) hud 1 -> 7  SELECTED
```

The "control" hero is at **sx = -6** — already outside the native window. It
needs the target in x 70..170 so its ±40 box stays native, and `aim_tag` spends
26 pan attempts failing to get there. **Both arms are in the band**, so the
comparison the probe exists to make was never made — and it prints a band
result immediately after saying that result cannot mean anything.

The camera is not clamped, incidentally: it ranged x 0..616 in the same run.
The hero simply does not end up in 70..170.

## My replacement had the same disease

If the target cannot be moved, move the **box**: one centred on the target so
it spans x=0, one shifted right so it does not, both the same width and both
containing the target. Sound in principle.

The shift was hard-coded at +24, on the strength of an `sx=18` reading — **from
a different run**. The hero moves. In this run it was at -6 and -14, so:

```
rep0 cross  target sx= -6  box x -46..34  CROSSES x=0  nothing
rep0 native target sx= -6  box x -22..58  CROSSES x=0  SELECTED
rep1 native target sx=-14  box x -30..50  CROSSES x=0  nothing
rep1 cross  target sx=-14  box x -54..26  CROSSES x=0  nothing
rep2 cross  target sx=-14  box x -54..26  CROSSES x=0  nothing
rep2 native target sx=-14  box x -30..50  CROSSES x=0  nothing
RESULT  crossing 0/3 selected, entirely-native 1/3 selected
```

**Every box crossed the boundary, including all three labelled "native".** The
RESULT line compares two crossing boxes at slightly different offsets and means
nothing. Its own per-trial output says `CROSSES x=0` on every line — the probe
printed the evidence against its own summary and I nearly quoted the summary.

## The fix: check the geometry, and admit when the pairing is impossible

Three changes, all about not asserting what has not been checked:

* the non-crossing box is computed from the target's position **at trial
  time** (`x0 = max(0, sx - 40)`), not from a constant;
* **each trial is labelled from its actual geometry**, and a trial whose box
  does not match what its arm intends is discarded rather than counted;
* if either arm ends with no valid trials, the probe prints **NO COMPARISON
  POSSIBLE** and says the boundary question is unanswered, instead of printing a ratio.

A box of half-width 40 can only avoid the boundary if the target is at
`sx >= 40`, and the crossing box only crosses if `sx < 40`. So **both arms are
possible only for a target at roughly `sx` 0..40**:

```
sx= -14   cross -54..26 crosses  | native 0..80 does NOT contain the target
sx=  18   cross -22..58 crosses  | native 0..80 contains it, no crossing   <- works
sx=  45   cross   5..85 does not cross -- discarded
```

`sx=18` was the hero's most common position in the earlier run (89 of its
samples), so the window is reachable; it just cannot be assumed.

## Status

**Boundary defect: UNANSWERED.** Not "no bug found" — the experiment did not run. The
one suggestive observation (a crossing box selecting nothing while an in-band
box selected) comes from two single trials with a failed calibration guard, and
is not evidence.

**Wrong-units defect: untouched.** It needs several units of known identity in the box and
a readback of which were selected, which is a different instrument again.

**Fixed:** the probe now refuses to report a comparison it did not make. That
is the part worth keeping from this tick.

## The experiment, run properly

Two changes made it work. First, aim at the window where **both arms are
valid** — `0 <= sx < 40` — instead of the 70..170 `--drag-unit` wants and
cannot reach. Second, use the **hero** rather than the builder: the builder
reads `sx=-6` at every trial (121 of its samples in one run), while the hero
parks at `sx=18`.

The camera is not the obstacle, which is worth stating because it was the first
suspicion: it took values 0, 88, 136, 217 and 265 in the same run. It simply
keeps returning to 217, where the builder sits at -6.

```
run 1 (5 reps)   parked [lb_hero] at (18,95)
  crossing        -22..58   2/5 selected
  entirely-native   0..80   2/5 selected      0 skipped

run 2 (9 reps)   parked [lb_hero] at (18,95)
  crossing        -22..58   5/5 selected
  entirely-native   0..80   7/9 selected      4 skipped
```

Pooled: crossing **7/10**, entirely-native **9/14**. Within each run the arms
are alternated and each trial's geometry is verified before it counts, so drift
during a run cannot favour one arm.

**No boundary effect.** A drag box that spans the native edge selects at least
as often as one that does not — in run 2 it selected every time. Nothing here
supports "drag-select across the boundary is buggy" as a selection failure.

## What the numbers do show, and it is not the boundary

The rate swings hard between runs — 2/5 then 5/5 in the same arm — so the
per-run conditions dominate. That is consistent with this file's own
calibration note that an in-band click lands "about one time in three", and it
points at delivery or at the sprite's clickable band rather than at geometry.

## Status

**Boundary defect: no boundary effect detected**, on one target, one map, one box size,
n=10 and n=14. That is a real measurement where the previous section had none,
but it is not a proof over every case, and it does not address a drag that
*starts* outside the drawn surface (docs' earlier note that a target at native
-69 maps to window x -5 and becomes a RELEASE).

**The wrong-units defect remains untouched.** Checking that a drag selects the right units needs several
units of known identity inside one box and a readback of which were selected —
`lb_field_entity_at` can supply the identities, so the instrument is buildable,
but it is not this one.
