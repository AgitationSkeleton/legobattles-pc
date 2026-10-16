# 259 — The sixth route to the fog edges, and it validates

docs/253 stopped on this, listing five routes closed by measurement, and that
was the right call at the time. The defect is still present — fog in the
expanded screen space is square-edged and possibly inaccurate to the revealed
terrain — so it is reopened, with a route none of the five covered.

## What is actually missing

`LB_FOGEDGE=1` prints the learned table. One Free Play run, 40 s of gameplay:

```
mask D4 (N0 NE1 E1 SE1 S1 SW0 W0 NW0) -> 0B1 0B2 0B3 0C4 0BB 0BC
mask D6 (N1 NE1 E1 SE1 S1 SW0 W0 NW0) -> 0BF 0C0 0C1 0C4 0BB 0BC
mask F4 (N0 NE1 E1 SE1 S1 SW1 W0 NW0) -> 0B1 0B2 0B3 0C4 0BB 0BC
mask F6 (N1 NE1 E1 SE1 S1 SW1 W0 NW0) -> 0BF 0C0 0C1 0C4 0BB 0BC
mask FE (N1 NE1 E1 SE1 S1 SW1 W1 NW0) -> 0C2 0C0 0C1 0BD 0BB 0BC
mask FF (N1 NE1 E1 SE1 S1 SW1 W1 NW1) -> 0C2 0C0 0C1 0BD 0BB 0BC
```

Six masks, and every one has `E1 S1` — exactly as docs/253 predicted, because
the explored region starts at the map's west edge. Everything else falls back to
the interior pattern, which is the square edge seen in play.

## The polarity, read rather than assumed

`metatile_mask()` (lb_widemap.cpp:2814) sets a bit when the neighbour
**is shrouded**:

```c
if (metatile_shrouded(mx + dx, my + dy))
    m = uint8_t(m | (1u << neighbour_bit(dy, dx)));
```

This matters and nearly went the other way: docs/247 records the fog record's
own bits as "set = revealed", so the mask and the record have OPPOSITE
polarity. A tile therefore fades out toward a **0** bit.

## The route: classify the tiles by their pixels

`LB_FOGTILES=1` dumps all twenty tiles of the shroud set (0x0B1..0x0C4) as 8x8
palette indices. Counting non-transparent pixels on each border gives each tile
an orientation without assuming anything about symmetry — which matters,
because docs/253 route 3 disproved the mirror assumption the code comment beside
that dump still carries ("an east edge is the horizontal mirror of a west edge"
is **false**; best match differs on 22 of 64 pixels).

Full table: `docs/reference/shroud-tile-borders.txt`. Coverage 8 = solid cloud
on that border, 0 = fully faded.

```
idx  opaque  top bot lft rgt        idx  opaque  top bot lft rgt
0B1   17/64   0   6   0   4         0BB   64/64   8   8   8   8
0B2   46/64   0   8   4   7         0BC   64/64   8   8   8   8
0B3   59/64   4   8   7   8         0BD   64/64   8   8   8   8
0B4   53/64   2   8   8   6         0BE   58/64   6   8   8   5
0B5   53/64   0   8   6   6         0BF   53/64   8   5   3   8
0B6   50/64   2   8   5   8         0C0   64/64   8   8   8   8
0B7   57/64   4   8   8   5         0C1   64/64   8   8   8   8
0B8   35/64   0   8   5   5         0C2   64/64   8   8   8   8
0B9   23/64   0   6   5   0         0C3   50/64   8   6   8   1
0BA   54/64   6   8   3   8         0C4   43/64   5   8   1   8
```

## It reproduces every observed mask

Two independent checks before using it for anything unobserved.

**The interior set.** The six tiles at 64/64 with all four borders solid are
`0BB 0BC 0BD 0C0 0C1 0C2` — exactly `kShroudTiles`, which docs/248 established
by a completely different method. The classification did not know that.

**Every learned mask.** The metatile is 3x2, so position 0 touches the N and W
edges, 1 touches N, 2 touches N and E, 3 touches W and S, 4 touches S, 5
touches S and E. Against the table:

```
D4 (N0 W0 E1 S1)   pos0 0B1  top 0 open <- N0    lft 0 open <- W0
                   pos1 0B2  top 0 open <- N0
                   pos3 0C4  lft 1 open <- W0    bot 8 solid <- S1
D6 (N1 W0 E1 S1)   pos0 0BF  top 8 solid <- N1   lft 3 open <- W0
                   pos2 0C1  solid            <- N1, E1
                   pos3 0C4  lft 1 open <- W0    bot 8 solid <- S1
FE (all cardinals shrouded)  all six solid -> the interior set
```

Six for six, including the two that differ only in N and the two that differ
only in W. The model is that **each tile's open borders are exactly the mask's
0 bits for the edges its position touches**.

## CORRECTION: the twenty-tile range was fitted to the same biased sample

Everything above is measured and stands. The premise it was about to be used
under — that the shroud tileset IS 0x0B1..0x0C4, twenty tiles — does not.

Two things in the table should have raised it immediately. Only **two** tiles
are corner-sparse (0B1 at 17/64 and 0B9 at 23/64) though a metatile has four
corners; and **not one tile fades at its bottom edge** — the lowest bottom
coverage is 5 of 8, on 0BF, which was observed at a TOP-left position, so that
is internal shape rather than an edge. A south-facing cloud edge cannot be
drawn with these twenty tiles at all.

The range came from a histogram of **one frame** ("487 interior tiles against
33 edge tiles, all within 0x0B1..0x0C4"), and that frame is from the same
sample as everything else here: every observable mask has E1 S1. It could only
ever contain north and west variants.

`LB_FOGTILES_LO` / `LB_FOGTILES_HI` now widen the dump. Over 0x090..0x0E8
there are **62 non-empty tiles**, not 20, and the south-fading ones sit just
above the old bound:

```
0C8  23/64  top6 bot0 lft0 rgt5     open bottom and left   (SW corner)
0C9  39/64  top8 bot0 lft5 rgt7     open bottom            (S edge)
0CC  53/64  top8 bot0 lft6 rgt6     open bottom            (S edge)
0CF  42/64  top8 bot0 lft6 rgt3     open bottom and right  (SE corner)
0D0  12/64  top5 bot0 lft3 rgt0     open bottom and right  (SE corner)
```

**They are not identified as shroud graphics.** A palette test cannot do it:
the known cloud tiles use indices {2,3,4,5,6,8..15}, and so do controls 0E2 and
0E6 elsewhere in the range. Identification still needs one real sighting.

### The filter was discarding the answer

Worse than a gap. The learner rejects a whole metatile when any of its six
tiles falls outside `kShroudTileLo..kShroudTileHi`:

```c
if (idx < kShroudTileLo || idx > kShroudTileHi) ok = false;
```

So on a map that *does* draw a south edge, the observation would be thrown away
as "not fog". The bound fitted to the biased sample was enforcing that bias.

Raised to 0x0D0. The justification is the weaker, safer one: the fog **record**
is the authority on whether a metatile is shrouded (100% agreement, docs/247),
and this range is only a secondary guard against a contaminated read. The
record gate, `mask_is_observed` and the uniform-metatile test all still apply,
so widening it cannot admit a cell the record calls revealed.

Verified to be a **no-op on this map**, exactly as predicted: the same six
masks (D4 D6 F4 F6 FE FF), `edgemasks=6`, 35.1 fps. It makes the learner
capable of an observation it was built to discard; it cannot be shown to help
until a map draws one.

## What this unlocks, and what is left

Nine tiles have never been placed: `0B4 0B5 0B6 0B7 0B8 0B9 0BA 0BE 0C3`. Their
signatures already suggest where they go — `0B9` (top 0, rgt 0) is the only
tile open to both north and east, so it is position 2 of an `N0 E0` mask; `0C3`
(rgt 1, everything else solid) is an east-only edge; `0BA` and `0C4` are the
west-only pair. That is the derivation to finish, and it must be finished the
same way it was validated: by the table, not by eye.

Not yet done, and not to be skipped: the derived assignments have to be checked
against the cartridge somewhere it actually draws an east or south edge before
they are trusted. The five closed routes say that will not happen on this map by
observation — but a derived table only needs ONE confirming sighting to stop
being a guess, and a play test on a different map is a cheaper oracle than
another instrumentation round.

## The square edges are real, and now measured

`[lb_fogedge] band edge cells: exact N, cardinal N, interior(square) N` counts
which tier served each band cell. Across seven existing runs the square tier is
never idle — peaks of 1161, 1027, 681, 640, 558, 252 cells per report. In one
of them, 1161 of 2365 band edge cells (49%) drew the interior pattern where the
cartridge would have drawn an edge. So the square-edge defect is not hypothetical.

The reason is arithmetic: the six learnable masks collapse to only **three**
distinct cardinal combinations — `(N0,E1,S1,W0)`, `(N1,E1,S1,W0)` and
all-shrouded. Thirteen of sixteen have no entry at all.

And they cannot be composed from what was observed. A mask with `E0` needs a
tile open on its right at position 2; every observed mask has `E1`, so no
observed tile is right-open. Composition is a dead end, which is what makes the
derived tier necessary rather than merely convenient.

## The derived tier

`fog_edge_derive()` scores every tile in the measured border table against the
openness the position requires — open where the mask has a 0 bit on an edge
that position touches, solid everywhere else — and takes the best fit. Scored
rather than matched exactly because corner tiles fade diagonally: `0B1` is the
north-west corner and reads `top 0, lft 0` but also `rgt 4`, which no exact
rule would accept.

It fills only entries observation left empty, and a mask the cartridge is later
seen drawing replaces the derived tile on the next rebuild. `LB_FOGDERIVE=0`
turns it off.

### Measured, same route

```
LB_FOGDERIVE=0    exact 594  cardinal  450  interior(square) 240   (18.7% square)
LB_FOGDERIVE=1    exact   0  cardinal 73809  interior(square)   0   (0%)
```

**The square tier is eliminated.** The `exact` column going to zero is
bookkeeping, not a regression: with all sixteen cardinal entries filled, the
cardinal tier now answers first for every cell, and the all-shrouded entry it
answers interior cells with is the one the observation loop filled from the
real `FF` mask.

Safety check — fog **coverage** is byte-identical between the two runs:

```
shrouded=282/768 (bg3marks 768)      both
ext2 fog=71331 (band 71049/73041)    both
```

so this changes *which tile* is drawn at an edge and never *whether* fog is
there. No holes were opened and nothing was over-covered.

### The risk, stated plainly

The tiles that satisfy a south- or east-facing edge are `0C8 0C9 0CC 0CF 0D0`,
and those are **not confirmed to be shroud graphics** — a palette test cannot
separate them from unrelated tiles, because the controls share the same ramp.
They are used only where the confirmed range `0x0B1..0x0C4` offers nothing,
which is exactly the bottom-open case, so the exposure is the minimum the
problem allows.

**Not visually verified**, and honestly so: the A/B scene has no visible band
fog edge, so the two captures are indistinguishable. If those five tiles turn
out to be the wrong graphics, an east or south fog boundary will look wrong
rather than square — a different defect, not a worse coverage bug. That is the
thing to look for in a playtest, and `LB_FOGDERIVE=0` restores the old
behaviour immediately if so.

## A second fog fault reclassifies this work

A separate fog fault is wrong-looking textures: the fog tiles themselves look
wrong, corrupted or mismatched, *not* merely square-edged.

So the square-edge work in this document addresses the square-edged fog
defect, and the measurement stands — `interior(square)` really was 240 of 1284
band edge cells and is now 0. But it is **not** the cause of the wrong-looking
textures, which is a separate and un-investigated defect.

**And the two interact badly.** The derived tier ships tiles `0C8 0C9 0CC 0CF
0D0` that are *not confirmed to be shroud graphics*, and could not be visually
verified because the A/B scene contained no visible band fog edge. If the fog
textures already look wrong, an unverified tile substitution is exactly the
change most likely to be blamed for it — or to make it worse.

`LB_FOGDERIVE=0` reverts the derived tier without touching anything else. That
is the first thing to try if the fog looks worse after this build, and the
distinction to establish next is whether the wrong-looking textures appear:

- only in the widened band, or in the guest's own 32x24 window too (the latter
  would rule out everything this port does to the band);
- only after this build, or in earlier ones as well;
- as the wrong *tile* in the right place, or the right tile drawn with the wrong
  *palette*.

Those three answers separate a tile-index fault from a palette fault from a
band-only fault, and each has a different cause.
