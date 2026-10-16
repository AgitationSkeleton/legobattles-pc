# 267 — The transparent fog was a west-edge tile, for the first 24 seconds

The defect: fog sometimes rendered transparent, where it is supposed to be
opaque, and not consistently -- it varied from run to run for no evident
reason.

Four maps, two magnifications, four surface sizes and 45-second lingers had all
failed to reproduce it. The reason is in that inconsistency: it is not a
property of the map or the window. It is the first twenty-odd seconds of every
battle, and then it heals itself.

## Reproduced, then measured

`LB_LAYERDUMP=N` writes, every N composited frames, a greyscale map of which
layer won each pixel — `(Pixel::order + 1) * 40`, so BG1 terrain is 120 and BG2
fog is 160 — beside the composited picture for the same frame. The fog lives on
BG2, and the interior shroud set `0BB 0BC 0BD 0C0 0C1 0C2` is 64/64 opaque
(docs/reference/shroud-tile-borders.txt), so over unexplored ground BG1 must win
nothing at all.

```
frame          240    480    720    960   1200   1440   1680   1920
BG1 wins     12.46  12.46  12.46  12.46  12.46  12.46   0.00   0.00   %
```

Uniformly across the whole fogged area, not at an edge or a seam. And
`lb_comp-1712x730-240.png` shows what 12.46% looks like: the lake and the paths
legible through the cloud. `-1920.png` is flat grey.

## The counters name the branch

```
[lb_fogedge] band edge cells: exact 0, cardinal 73809, interior(square) 0   masks=1
[lb_fogedge] band edge cells: exact 0, cardinal 0,     interior(square) 0   masks=6
```

EVERY cell in the extension table — all 73,809, interior ones included — took
the cardinal-substitution tier while only one edge mask had been learned. What
it served was not the interior set:

```
early  ext2 (fog): 00BA 00BB 00BA 00BA 00BB 00BA ...
late   ext2 (fog): 00C2 00C0 00C1 00C2 00C0 00C1 ...
```

`0BA` is **54/64** opaque, `lft 3` — a WEST EDGE tile, which fades on its left
border because that is its job. Two of every three columns get it, so about 12%
of the cloud is holes. That is the 12.46%.

## The guard that was on the third tier and not the first two

```c
if (g_fog_edge_seen[mask][pos])            { fog = ...; }
else if (const uint16_t* c = fog_edge_cardinal(mask, pos)) { fog = *c; }
else if (mask != 0xFFu)                    { ++g_edge_interior; }
```

The last branch already refuses to call mask `0xFF` a square edge, because a
metatile with cloud on all eight sides is not an edge at all. The two branches
in front of it never got the same test, so the substitution machinery was free
to replace the correct interior pattern with an edge variant fitted to a
DIFFERENT neighbourhood. With one mask learned, `fog_edge_build_cardinal`
derives entries for all sixteen cardinal classes from that single observation,
and every interior cell in the band took one.

The comment on the cardinal tier says it "can improve the picture and cannot
make it worse -- the same argument the exact table was added under". That is
true for a BOUNDARY cell, where the alternative is the square interior pattern.
For an interior cell there is nothing to improve, and "no worse" means untouched.

```c
if (mask == 0xFFu) {
    /* interior: keep g_fog_tiles[pos] */
} else if (g_fog_edge_seen[mask][pos]) {
```

## Verified

```
BG1 wins in the fogged area
 before   240:12.46%  480:12.46%  720:12.46%  960:12.46%  1200:12.46%  1440:12.46%  1680:0.00%
 after    240: 0.00%  480: 0.00%  720: 0.00%  960: 0.00%  1200: 0.00%  1440: 0.00%  1680:0.00%
```

The band's first fog row is now `00C0 00C1 00C2 ...`, and the boundary variants
still arrive where they belong — `0064 ... 00B1 00B2 00B3 00C2 00C0 00C1 ...`
— so the rounded edge the substitution exists for is intact and only the
interior is protected.

`LB_THREADCHECK`: 21 reports over 7 surface sizes, 2520 frames, 0 differing
pixels. Frame time is unchanged to within run-to-run spread (57.9 / 58.2 / 54.7
fps, emu 11.73 / 11.69 / 12.35, against 56.0 / 57.0 / 56.1 and 12.15 / 11.94 /
12.05 before).

## What this says about the next fog question

The other fog defect, square-line edges on the horizontal fog, is the
thing the cardinal tier was built to fix, and it is still only partly fixed: the
learner sees six of 256 masks because every observable one has `E1 S1` (docs/259),
so a boundary whose neighbourhood was never observed still falls back. That
fallback is now confined to boundary cells, where it belongs. Sizing what is
left needs the same instrument used here -- count the cells that reach the third
tier, rather than looking at the picture.
