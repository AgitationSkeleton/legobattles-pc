# 249 — Square fog edges in the band

Status: **67% of them removed, measured; the rest need the table seeded.**

## The cause

The cartridge draws a transition variant where a shrouded metatile borders
revealed ground — the rounded, bumpy cloud edge. The port learns those variants
from the guest's own 32 columns, keyed by the **eight-neighbour mask**, and
"a mask never seen in the native columns falls back to the interior pattern".

The interior pattern is cloud-with-cloud-on-all-sides. Drawn at a boundary it
is a **straight edge**, which is why fog edges in the extended view were
perfectly square.

Measured on three maps: `edgemasks=6`. Six of 256 masks are ever observed, so
almost every boundary cell in the band took the fallback.

## What the dump showed

`LB_FOGEDGE=1` prints the learned table. Six masks, and they have structure:

    mask FF (N1 NE1 E1 SE1 S1 SW1 W1 NW1) -> 0C2 0C0 0C1 0BD 0BB 0BC
    mask FE (N1 NE1 E1 SE1 S1 SW1 W1 NW0) -> 0C2 0C0 0C1 0BD 0BB 0BC
    mask D6 (N1 NE1 E1 SE1 S1 SW0 W0 NW0) -> 0BF 0C0 0C1 0C4 0BB 0BC
    mask F6 (N1 NE1 E1 SE1 S1 SW1 W0 NW0) -> 0BF 0C0 0C1 0C4 0BB 0BC
    mask D4 (N0 NE1 E1 SE1 S1 SW0 W0 NW0) -> 0B1 0B2 0B3 0C4 0BB 0BC
    mask F4 (N0 NE1 E1 SE1 S1 SW1 W0 NW0) -> 0B1 0B2 0B3 0C4 0BB 0BC

Masks differing **only in a diagonal** give identical tiles — FF/FE differ in
NW, D6/F6 and D4/F4 in SW — while the cardinals each change a specific edge of
the metatile: W swaps positions 0 and 3 (the left column), N swaps 0,1,2 (the
top row). FF reproduces `kShroudTiles`, which is the cross-check that this is
the same cloud.

So the variant is selected by the four **cardinal** neighbours. The useful key
space is 16, not 256, which is why only six masks were ever filled.

## The fix

A tier between the exact mask and the interior pattern: any observed mask
sharing this one's four cardinal bits. It is reached **only** when the exact
mask is absent, where the alternative was the interior pattern anyway, so it
can improve the picture and cannot make it worse — the same argument the exact
table was added under. It does not assume the diagonals never matter; an exact
match still wins wherever one exists.

## Measured

Band boundary cells per report, same route, one variable:

| | exact | cardinal | interior (square) |
|---|---|---|---|
| `LB_FOGCARD=1` | 10800 | 2880 | **1440** |
| `LB_FOGCARD=0` | 10800 | 0 | **4320** |

**4320 square cells down to 1440, a 67% reduction.**

## What is left

1440 cells per report still take the interior pattern: cardinal combinations
never observed at all in the native window. The interior tiles were fixed by
writing down measured constants once it was clear they do not come from the
map (docs/248); the edge table wants the same treatment, and now has a dump to
collect all sixteen cardinal combinations from.

## The contaminated entries, and what actually stopped them

A second entry for mask FF reading `0EA 0EA 0EA 0EA 0EA 0EA` was on record.
The edge learner's only content test was `!= kClearTile`, so **any** BG2 tile
that was not the clear marker counted as fog -- whatever a menu, a transition
or a mid-update frame happened to hold -- and the write is last-wins, so one
bad frame replaces a good entry for the rest of the session.

First guard: the shroud tileset's own index range. Measured with the tiles,
from the histogram at the edge table -- "all within 0x0B1..0x0C4". `0x0EA` is
outside it.

That was not enough. The next run produced `0BA 0BA 0BA 0BA 0BA 0BA` instead:
**inside** the range, so indistinguishable by index, and still not a metatile.

Second guard: a metatile's six tiles are not all the same one. That is the
property the interior learner already leans on for exactly this case -- a frame
caught mid-update. Only the all-identical case is rejected, not partial
repeats, because an edge variant legitimately reuses a tile across positions
(`0C4` is at position 3 of both D4 and D6) and requiring full distinctness
would throw away real observations.

After both, the table is clean over a six-step pan: six masks, every entry a
real metatile, and FF reproducing `kShroudTiles` exactly.

Worth stating as a lesson: the first guard looked like it worked, because the
specific bad value it was written against stopped appearing. It took re-running
the dump to find that a different bad value had taken its place.
