# 252 — The band fog measures clean; what is left is the square edges

Status: **four independent checks, all negative.** The remaining visible fog
defect is the edge variants, which is a separate and already-quantified thing.

## The checks

| probe | asks | result |
|---|---|---|
| `[lb_bare]` | band cells drawn `kClearTile` where the record calls the metatile hidden | **0** |
| `declined_out_of_table` | band cells the extension declined, falling through to the hardware wrap | **0** |
| `[lb_lone]` | isolated revealed metatiles in the game's own record | **0** |
| `[lb_vis]` box | does the revealed region move with the camera | fixed at `0..22,0..22` across 125 tiles of pan |

## The BG1-in-band share is geography, and that follows from the above

The layer dump reads, at 1706x696:

    GUEST window   BG2 100.0%
    LEFT band      BG2 69.3%  BG1 30.7%
    TOP band       BG2 72.9%  BG1 27.1%
    RIGHT band     BG2 100.0%
    BOTTOM band    BG2 100.0%

Terrain winning a third of two bands while the guest's own view is solid cloud
looks like a leak. It is not, and it does not need a new measurement to settle:

* BG1 can only win a band pixel where BG2 is transparent there, i.e. where the
  fill wrote `kClearTile`.
* `[lb_bare]` is 0, so **every** band cell written `kClearTile` is one the
  record calls revealed.
* Therefore BG1 wins only over revealed ground.

The premise that `kClearTile` is transparent is the cartridge's own behaviour,
visible in the same dumps: in the GUEST window -- which this port does not
draw -- BG1 wins 54.9% at 786x426, over cells the cartridge itself filled with
`kClearTile`.

And the direction matches the map: the record's revealed region is
`0..22,0..22`, the map's **top-left corner**, so the LEFT and ABOVE bands point
back at explored ground while RIGHT and BELOW point into the dark. The service
rates agree: LEFT 58.9%, ABOVE 75.7%, against RIGHT 89.9%, BELOW 96.2%.

## Which also explains why only six edge masks are ever learned

`docs/249` left 1440 cells per report still drawn with the interior pattern,
because their cardinal neighbourhood had never been observed. Every observed
mask has `E1 S1` -- cloud to the east and south.

That is forced by the geography. Standing inside an explored region in the
map's top-left corner, every fog boundary has revealed ground to its **west or
north**, so only `W0`/`N0` variants can ever be seen. `E0` needs shrouded ground
to the *west* of revealed ground, and on this map from this start there is no
explored ground further west to produce it.

So the missing variants are not a bug in the learner; they are unobservable on
this route. They will be learned on a map or a playthrough whose exploration
runs the other way, and the cardinal fallback already covers 2880 of the 4320
cells that were square before it.

## What is actually left

The square edges, and only those: 1440 cells per report, measured, with the
cause (unobserved cardinal combinations) understood. Everything else about the
band's fog measures correct.

## Tested directly: the missing edge masks cannot be observed on this route

`--fxband` orders a unit out rather than only panning the camera, which is what
actually extends exploration. Two units were ordered east and moved out of the
native columns, and the record grew with them:

    box 0..22,0..22 -> 0..24,0..22 -> 0..27,0..22 -> 0..27,0..23

Two things fall out of that.

**The record is confirmed an EXPLORED record**, not a currently-visible one: it
accumulates as units walk, monotonically, and the port tracks it live. That is
the independent check the monotonicity test in `find_fog_record` was written to
make, arrived at from the other direction.

**And the new ground did not produce a single new edge mask.** It grew the
explored region's EAST boundary, which still has revealed ground to its west --
`W0` again. `E0` needs shrouded ground to the *west* of revealed ground, and
this region starts at map column 0, the map's own west edge, so there is
nothing further west to be shrouded. The prediction in the section above is
confirmed by experiment rather than left as an argument.

### And a second condition, which the same run exposed

That run reported `edgemasks=0` -- worse than the 6 seen elsewhere -- with the
learner's own counters saying why:

    cand=120 notshroud=110 margin=5 notfog=5 took=0

`notshroud` is the dominant rejection: with the camera sitting inside the
explored area after the units moved out, there are almost no shrouded metatiles
in the guest's window to sample a boundary from. The learner needs cloud IN
VIEW, and a well-explored view has none. (The `notfog` count of 5 is the range
and uniformity guards from docs/249 doing their job, not the cause.)

So the edge table fills only while the player is looking at a fog boundary of
the right orientation, and one orientation is unreachable on this map. Waiting
for observation is not a route to filling it.

**The way in is the tile graphics.** The shroud tileset occupies 0x0B1..0x0C4,
twenty tiles, of which the observed variants use eleven: 0B1 0B2 0B3 0BB 0BC
0BD 0BF 0C0 0C1 0C2 0C4. The unobserved nine are the candidates for the east
and south variants, and which is which can be *measured* rather than guessed --
an east edge is the horizontal mirror of a west edge, and the tiles are in VRAM
to compare pixel by pixel. That keeps the fix on measurement, like kShroudTiles.

## RULED OUT: the edge variants are not mirrors of each other

The plan above was to identify the nine unobserved tiles by mirror-matching:
an east edge should be the horizontal mirror of a west edge, comparable pixel
by pixel rather than guessed.

The tileset dumps cleanly once the probe waits for the battle to load it --
a one-shot on the first `scan_shroud` call fired before the tiles were resident
and printed twenty rows of zeros twice. Gated on `fog_learned()` and 300 scan
frames it reads real data, 256-colour, 64 bytes a tile (BG2CNT bit 7 is set;
a first attempt assumed 4bpp and read half strides).

Matching every observed tile's H- and V-mirror against all twenty:

    0B1 H-mirror -> 0B9  diff 22   <- the best result anywhere
    0C2 V-mirror -> 0B7  diff 38
    0B2 H-mirror -> 0B9  diff 39
    ...            ...   diff 41..52

**Nothing matches.** An exact mirror is diff 0 and the best result differs on
22 of 64 pixels. The cloud edges are irregular organic shapes drawn
independently per orientation, not reflections, so there is no mapping to
recover this way. Forcing the closest one would put visibly wrong art on the
boundary, which is worse than the straight edge it replaces.

So both routes to the missing variants are now closed by measurement:
observation cannot reach them on this map (tested), and the tileset does not
encode them as mirrors (tested).

### What is left, and it is the same shape as the fog record

The cartridge draws these variants, so it owns a mapping from neighbour
configuration to tile, and that mapping is in the ROM. The fog record was
solved exactly this way -- four correlation searches failed and the decompiled
reader gave the address outright. The shroud stamp is the function to read:
`func_0203A4B4`, which `docs/248` identified as writing the clear tile where
the visibility test passes, and whose neighbouring code must choose the edge
tile where it does not.

Until then the cardinal fallback stands: it covers 2880 of the 4320 cells that
were square before it, measured, and the remaining 1440 are limited by which
masks this map can produce rather than by the fallback.
