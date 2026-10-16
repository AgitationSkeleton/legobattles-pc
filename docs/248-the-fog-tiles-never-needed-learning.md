# 248 — The fog tiles never needed learning

Status: **fixed, 0 frames instead of never.** Answers "the slow load time for
the area outside the original aspect ratio upon loading into a new match".

## The cold start

A widened cell the map-space record calls shrouded is drawn **blank** until the
six tiles of the fog metatile are known. Those were learned by watching the
guest draw them: a per-phase vote accumulated over many frames, committed only
once all six phases have a candidate and the six are distinct. Until it
commits, the band outside the native aspect is empty and fills in as the votes
arrive — which is the slow load.

## RETRACTED: the shroud is not a metatile in the map's tables

The first fix here seeded the six tiles by expanding **metatile 100**, on the
strength of the note at `kShroudMetatile`:

> the shroud is drawn through the same machinery as the terrain, from one fixed
> metatile -- func_0203A4B4 writes index 100 for every tile the visibility test
> flags

**That note is wrong and so was the seed.** A disagreement check added at the
learner's commit — the map's own tables against what the guest is observed
drawing — reported six of six phases wrong on two maps out of three:

    map 0  seeded 11E 19F 1A0 121 1A1 1A2   observed 0C2 0C0 0C1 0BD 0BB 0BC
    map 2  seeded 196 197 198 199 19A 19B   observed 0C2 0C0 0C1 0BD 0BB 0BC

The seeded values differ per map while the observed ones do not, which is the
shape of reading the wrong index out of a per-map table.

Expanding **every** metatile in the tables and comparing against the learned
set then settled it:

    [lb_fogidx] shroud tiles 0C2 0C0 0C1 0BD 0BB 0BC == metatile -1 (split 440)

on both maps. No metatile expands to the shroud. It is not in the map's tables
at all.

`kClearTile` is `0x064` — also 100 — and it is the tile BG2 holds where ground
is **revealed**. So what `func_0203A4B4` writes is the CLEAR tile where the
visibility test passes. A tile index and a metatile index, conflated because
they are the same number.

## What the tiles actually are

Constant, and measured three ways that agree: the histogram recorded in the
`kShroudMetatile` comment ("even rows C2 C0 C1 / odd rows BD BB BC"), and the
learner's committed values on map-index 0 and map-index 2, identical to each
other and to it. A fixed shroud tileset, not map data — so `kShroudTiles` is a
constant, which is the honest way to write down something that was measured
rather than derived.

Verified on three freeplay maps, every one seeding immediately with no
disagreement reported:

    [lb_fog] seeded 0C2 0C0 0C1 0BD 0BB 0BC (no learning needed)
    learned=1 ... ext2 fog=73041 (band 73041/73041)

## The cold start it fixes

Measured on one route with one variable, from the `[lb_shroud]` reports:

| | first report | `learned` |
|---|---|---|
| seeded | band fog **73041/73041** | `1` on all 38 reports |
| learner (`LB_FOGSEED=0`) | band fog **0/73041** | `0` for the first 16 of 38 |

Reports come every 120 scanned frames, so without a seed the band is entirely
empty for roughly 1900 frames. That is the slow fill.

(An earlier draft of this document claimed the learner "never committed in
15 s". That was wrong: it rested on a log line that had never been added to the
learner's path, so its absence measured nothing. The `learned=` flag in
`[lb_shroud]`, which was present all along, gives the real figures above.)

## The check that caught it

The observational learner still runs after the seed and its values still win.
When it commits, it compares against what was seeded and reports any
disagreement in one line. That is what turned a wrong seed into a measurement
instead of a silent regression, and it generalises: any map or tileset where
the constants are wrong says so, in the log, wherever the player finds it.

## Also recorded: a freeplay match with no fog at all

The route reached a battle with no shroud anywhere -- full terrain to every
edge of the widened surface on the first frame of gameplay, with entities drawn
well outside the DS aspect box. That is the "without fog" half of the standing
test, and it passes: 342 distinct colours in the band outside the box, flat
across the first eight seconds, with no fill-in.
