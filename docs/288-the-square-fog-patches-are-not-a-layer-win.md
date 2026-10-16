# 288 — The square fog patches are not a layer win, and not reproduced here

Item 13: fog and shroud could still glitch, jitter, and follow the camera as it
moved -- most notably a sheet of fog, and square holes or pieces of fog/shroud.
Shroud could even appear over traversed or starting terrain depending on camera
pans.

docs/250 investigated the "follows the camera" half and did not reproduce it —
the record's box stays at metatiles 0..22 while the camera crosses 125 tiles —
but it left the patches open with a stated next step:

> the isolated square patches visible in the play captures are **not**
> explained by this. They remain real and unexplained; the next place to look is
> which LAYER supplies those pixels, since BG1 terrain and BG2 fog are separate
> tables and only BG2 was under test here.

This is that step. It answers the question and the answer is **neither layer**.

## The measurement

`LB_LAYERDUMP` records which layer won each pixel, as `(order + 1) * 40`:
BG0/3D = 80, BG1 = 120, BG2 fog = 160. A pan route at 786x376 produced 25 maps.

Per-frame mix, which is itself worth having:

```
f=90..270     BG0 0.8%   BG1 50.0%   BG2 49.2%
f=360..900               BG1 19.1%   BG2 80.9%
f=1080..2250  BG0 0.5%   BG1  4.3%   BG2 95.3%
```

The frames with a genuine 50/50 boundary are the ones that can show an island,
so f=180 and f=270 were walked with connected components, looking for a region
of one layer fully enclosed by the other and not touching the surface border:

```
frame 180   BG1 terrain island inside fog:  0
            BG2 fog island inside terrain:  0
frame 270   BG1 terrain island inside fog:  0
            BG2 fog island inside terrain:  0
```

**Zero, both directions, both frames.** The fog/terrain boundary is one
connected frontier with no detached pieces on either side.

## What that rules out, and what it leaves

A "square hole in the fog" that were a *layer* fault — BG1 winning a patch it
should not, or BG2 losing one — would appear as an island in this map by
construction. None exists. So the patches are not produced by the layer
priority resolution, and docs/250's proposed next step is closed.

That leaves the remaining candidate: a patch **inside BG2's own content** — fog
cells written as `kClearTile` where the record says hidden. Those still resolve
as BG2 winning, so the layer map cannot see them and this analysis could not
have found them.

## And that mechanism is also measured absent

The port already counts exactly it. At the `g_ext2` assignment:

```c
if (s && !have_fog) ++g_bare_fill;
g_ext2[...] = (s && have_fog) ? fog : kClearTile;
```

`g_bare_fill` is declared once, incremented there and printed once. **It is
never reset** — three references in the whole file and none of them assigns
zero — so the `bare=` field in the periodic `[lb_shroud]` line is a running
total since process start, not a snapshot. A patch lasting three frames between
two prints would still be counted.

Across the whole pan run it read `bare=0` on every report, alongside
`bg3served=0` and a stable `shrouded=750/768`.

That is a stronger statement than it looks: it is not "no bare cells right now",
it is **no bare cell was produced at any point in the run**.

## Status

**Not reproduced, with two mechanisms measured absent rather than untested.**
The layer resolution produces no islands, and the clear-where-hidden fill never
fired once.

So the route does not create the reported condition. That is a fact about the
route, not evidence the patches are unreal — they were seen, and a capture
exists. What differs is unknown: a different map, a longer session, combat
revealing ground, or a state this route never enters.

**No code changed.** An earlier draft of this tick was going to add a
high-water-mark counter so a transient patch could not hide between periodic
prints. That counter already exists in exactly that form, because `g_bare_fill`
is cumulative — adding a second one would have been redundant code justified by
an unchecked assumption about the first.

The useful next input is the runtime log of a real play session rather than another route:
the counter is always on and costs one comparison per cell that is already being
written, so if `bare=` is ever non-zero in play, that number and the
`[lb_bares]` lines beside it name the first offending map cell directly.
