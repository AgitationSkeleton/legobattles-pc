# 270 — The square fog edge: the mask asked a different authority than the fill

The defect: the horizontal fog in the widened bands had square, flat edges,
where the vanilla renderer's fog edge is not flat.

The comparison with vanilla is available in every frame, because the
cartridge's own 256 columns sit inside the same composited surface as the band.

## Measured against vanilla

Walking the BG1/BG2 layer-win boundary row by row in one composited frame
(`LB_LAYERDUMP` records which layer won each pixel):

```
                boundary rows   mean vertical run   longest   step sizes
CARTRIDGE            280              2.04            32      0:143 1:93 2:27 4:7
PORT (band)          368             28.31           112      0:355 24:8  48:4
```

The cartridge's outline moves 1, 2 or 4 px every couple of rows. That is
sub-TILE detail, and it can only come from a partially transparent edge tile's
art. The band's moves only in whole metatiles -- 24 px -- and holds the same
column for up to 112 rows: whole metatiles flipping between solid fog and clear
with nothing in between. That is the flat edge.

It predates the fog fix of docs/267 and is unchanged by it (`mean run 28.31`
before, `28.31` after on the same frame), so it is a separate defect and not a
side effect of making interior tiles solid.

## The cause

The band answers two related questions from two different sources:

```
fill_terrain's `s`   hidden_metatile()    -> g_vis_bits, the GAME'S OWN fog
                                             record (docs/247), the whole map
metatile_mask()      metatile_shrouded()  -> g_seen, this port's cache of what
                                             the guest's own window has shown
```

In the band those disagree by construction. `g_seen` is initialised to SHROUDED
and is only ever written from the guest's own 32 columns (`scan_shroud`), so for
ground the window has not covered it reports shrouded whatever the truth is.

At the revealed region's edge the record therefore clears a metatile correctly
while every one of its neighbours still reads shrouded -- mask `0xFF`, which is
"cloud on all eight sides", which selects the interior pattern. A clear tile
hard against a solid one is exactly a 24 px step with no raggedness, and no edge
variant is ever consulted.

## The fix

`metatile_mask` asks the record, which is the authority `fill_terrain` already
trusts and the one the cartridge is itself drawing from. It falls back to the
cache when the record has not been located yet, because `hidden_metatile`
answers `false` for an empty `g_vis_bits` and taking that at face value would
report every neighbour REVEALED and make every cell an edge cell.

`learn_fog_edges`' own `metatile_shrouded` test is NOT changed: it runs over the
guest's own columns, where the cache is accurate by construction and is what the
learned variants are graded against.

`LB_FOGMASKREC=0` restores the cache.

## Verified, one variable

Same build, same route, 89 frames matched by frame number with the same band and
surface width in both arms:

```
                       mean vertical run   longest   step sizes
record (LB_FOGMASKREC=1)      5.73            5      0:10420 1:13026 2:4668 4:427
cache  (LB_FOGMASKREC=0)     28.81          112      0:30495 1:523   2:163   24:704
```

Per frame it is `1.56 mean / longest 5` against `21.65 / longest 112`, frame
after frame. The 1-2 px steps now dominate, which is the cartridge's own
signature.

The first attempt at this comparison was NOT controlled -- it picked "the dump
with the most boundary rows" independently in each run, landed on different
cameras (f=4950 against f=6210), and produced a before-figure of 2.81 that
contradicted the 28.31 measured elsewhere. Frame choice dominates this metric.
The knob exists so the comparison can hold everything else still.

Gates: `LB_THREADCHECK` 21 reports over 7 surface sizes, 0 differing pixels.
fps 57.08 / 57.62 / 57.30 against 57.57 / 57.69 / 57.81 before -- within spread.
