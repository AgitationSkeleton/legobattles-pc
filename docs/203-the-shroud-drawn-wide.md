# 203 — The shroud, found and drawn wide

Status: **working and verified in a live battle.** The widened columns now show
fog over unrevealed ground instead of terrain, seamlessly with the native
columns, at every window size tried.

This supersedes docs/202, whose central claim was wrong. See the retraction
there.

## The wrong turn, and what it cost

docs/202 concluded "nothing is shrouded in these matches" from two all-zero
probes of the mask object at `0x02151F28`. That was wrong: Free Play always has
fog unless the no-fog cheat is enabled, and a
screenshot of the very frame those probes read is wall-to-wall grey cloud.

The mask is real, and reading it was not a bug in the probe. It is simply not
the record that drives the picture: `func_0203420C` fills it from entity extents
every frame, clamped to `0x20 x 0x18`, and it is empty when nothing is filling
it. **A symbol that plausibly means "fog" is not evidence about what is drawn.**

The fix was to stop reading symbols and read the picture's own source. That
needed a new `vram` debug command, because `read_mem` cannot see `0x06000000`
at all — VRAM belongs to the video module, not the CPU bus, exactly as OAM does.

## What is actually there

Histogramming engine A's three tilemaps in a fully fogged view:

    BG2 @0x0000:  31 distinct, top 00BB x133 00C2 x132 00C0 x132 00BD x132
    BG1 @0x0800: 250 distinct, top 0000 x126 01E1 x66 01E2 x64 01E0 x59
    BG3 @0x1000:  68 distinct, top 001F x768   <- 768 = 32 x 24, the whole field

BG3's 768 looked like the answer, and `func_020396E0`'s third pass backed it up:

    if (*func_0203D8E8(tx, ty) != 0x1f && func_020EEBF4(fog, tx, ty))
        *func_0203D8E8(tx, ty) = 0x1f;

It was half right. Filling the widened columns of BG3 with `0x1F` changed
nothing on screen. `LB_BGMASK` settled it in two runs:

| masked            | result                                              |
|-------------------|-----------------------------------------------------|
| bit 11 (BG3 off)  | no change — fog still there                          |
| bit 10 (BG2 off)  | **fog gone**, clean terrain across the whole window   |

So **BG3's `0x1F` is a transparent marker** — an overlay cleared under fog —
and **BG2 draws the cloud**. Both are used here: BG3 says *where* the fog is,
BG2 says *what it looks like*.

## The fog is a metatile

BG2's entries in a fogged view are period 3 across and period 2 down:

    even rows   C2 C0 C1 C2 C0 C1 ...
    odd rows    BD BB BC BD BB BC ...

which is `func_020A3478`'s 3x2 expansion — the same machinery the terrain uses.
One frame of the vote even caught BG2 holding `064` in every slot: `0x64` is
100, the index `func_0203A4B4` writes. So docs/202's "100" was right; it is a
staging value, caught before expansion.

**But no table expands it.** Metatile 100 through the terrain's own tables gives

    via map+0x338:  11E 19F 1A0 / 121 1A1 1A2
    via map+0x33C:  243 1E0 25F / 243 1E2 1E3

neither of which is the fog. A search of all 4 MB of main RAM for the tile run
`C0 C1 C2` finds hits only inside the shroud staging buffer at `0x02152A20` —
the already-expanded output. There is no stored 12-byte fog record to read.

So the six tiles are **learned from the guest's own BG2**, at positions BG3
marks as shrouded, one per phase, rebuilt every frame. This needs no index and
no table, and it cannot drift from what is on screen. Note the tiles are two
rows of a sheet five wide: `BB BC BD` and `C0 C1 C2`, five apart.

## Three guards, each paid for by a wrong answer

* **Rebuild every frame, never latch.** The first version learned on first
  sight and was wrong for the whole battle — it committed
  `079 07A 07B / 089 08A 08B` from a frame where BG3 already said "shrouded"
  but BG2 still held the previous scene. The giveaway was `agree=0` on a screen
  reading `0C1 0C2 0C0 0C1 0C2 0C0`.
* **Require an 80% majority per phase.** BG2 is not always the finished picture
  when this runs; a half-raw, half-expanded frame scored 389/768 and must
  commit nothing.
* **Commit only six DISTINCT tiles, together.** Rejecting just "all six
  identical" was not enough — a frame committed `064 064 064 / 064 0EA 064`,
  five copies of the raw index and one stray, because one slot differed.

With all three, a run's committed tables are only ever the unlearned zero state
and the correct `0C2 0C0 0C1 / 0BD 0BB 0BC`, at `agree=768/768`.

## Phase: per frame, per hardware column

The table is indexed by hardware-column phase and rebuilt each frame, so it is
correct by construction regardless of whether the fill is anchored to the map or
to the screen. The table does differ between runs at different camera positions,
and that is not a bug — it is the camera, and the per-frame rebuild absorbs it.
`agree=768/768` is the proof that the table matches the frame it was built from.

Only one layer is widened by the compositor — `wide_bg_slot`, the furthest back,
which is BG1 here. BG2 (priority 1) and BG3 (priority 0) would never be chosen,
so the port names the layers it can supply entries for and upstream's rule still
governs everything else.

## The one honest limitation

The fog tiles can only be learned from a frame that has fog. At the very start of
the first battle after launch there has been no such frame, so:

* until the fog is known, the terrain is **not** widened either — empty columns
  are honest, revealed ground is not;
* after 120 frames with nothing shrouded anywhere, the widening proceeds anyway,
  so a match with Reveal Map on, or a fully explored map, does not sit behind
  black bars forever;
* the learned tiles are **kept across maps**. Only the first battle of a session
  has a cold start at all.

The residual window is a few seconds at the start of the first battle, if that
battle's opening view happens to contain no fog at its edges.

## Measured

    fog tiles 0C2 0C0 0C1 / 0BD 0BB 0BC  learned=1
    shrouded=768/768  agree=768/768
    map 23898/24576 shrouded
    ~28 fps at 448 columns

Captures in `screenshots/exp-final/`: fog fills the window seamlessly at
640x480 through 1920x1080, with no seam at the native boundary and no terrain
leaking into the widened columns.

## Buildings: a measured negative result

The widened terrain is built from the metatile grid alone, leaving out
`func_02034B44`'s building stamp, and this file's own comments warned that a
building standing on widened ground would therefore show as bare terrain.

That is not true, and the way to find out was not to pan a building across the
boundary and squint at it (one pan step moves 27 tiles). The guest's own 32
columns are ground where BOTH answers exist: recompute them from the metatile
grid and diff against BG1.

    [lb_terrain] mine vs guest BG1: 0/768 differ

Nine report frames across a full run, including the ones with the player's base
on screen: **zero differences, every time**. Nothing is stamped into BG1 that
the reconstruction does not already reproduce exactly, so buildings are 3D
geometry on BG0 — which already reaches the widened columns (docs/197).

There is no building gap to close. The diff stays in as a standing check: if
some map or unit type ever does stamp the tilemap, it will say so.

## The fog is MAP-anchored. Measured, not argued.

This had been guessed both ways. Counting both conventions every frame settles
it:

    abs=533 screen=533 of 768   cam_tx 57 (%3=0)
    abs=602 screen=602 of 768   cam_tx 60 (%3=0)
    abs=624 screen=0   of 768   cam_tx 61 (%3=1)

When the camera happens to sit on a multiple of three the two conventions are
**identical**, which is exactly why screen phase ever looked right. When it does
not, absolute matches 624 tiles and screen matches **none at all**.

So the expected fog tile for a map tile is
`g_fog_tiles[(ar % 2) * 3 + (ac % 3)]` — a function of absolute position only.
Both the shroud test and the learning tally now use that.

## The remaining stripe, and what it is NOT

A vertical stripe still appears through the left widened band after the camera
pans. Two candidates have been eliminated by direct measurement rather than
reasoning:

* **Not the shroud test.** Absolute phase is proven correct above.
* **Not the visibility cache.** Dumping `g_seen` across the widened columns
  shows clean contiguous runs, no alternation:

        [lb_seen] row 30: ###########.....|..........####################|########

* **Not the terrain.** The reconstruction matches the guest's own BG1 exactly,
  `0/768 differ`, on every report frame.

What is left is the fog TILE PATTERN itself: the six-tile metatile being emitted
at a phase that scrambles the cloud texture, which would read as fine vertical
banding rather than as blocks of fog and terrain — and blocks are what a cache
error would produce, since the cache is contiguous.

Next measurement: dump the emitted `g_ext2` entries for one row beside the
guest's own BG2 entries for the same absolute columns, and compare the sequences
directly. If the port's sequence is a rotation of the guest's, the phase used
when EMITTING differs from the phase used when TESTING, and the two must be
brought into agreement.

## The fog has EDGE TILES, and this port models only the interior

The seam probe prints the guest's own BG2 beside this port's emitted entries for
the same hardware columns:

    [lb_seam] h=-6..6 row 12: p000 p000 p000 p000 p000 p000
                              g064 g064 g064 g0B1 g0B2 g0B3 g0C2

Columns 3..5 hold `0B1 0B2 0B3` — **tiles that are not among the six this port
learned** (`0C0 0C1 0C2` over `0BB 0BC 0BD`). And the histogram in this very
document already recorded the answer without it being read properly:

    BG2 @0x0000: 31 distinct, top 00BB x133 00C2 x132 00C0 x132 00BD x132

**Thirty-one distinct tiles.** The six that were learned are just the four most
common plus their pair; the other twenty-five were dismissed as noise. They are
not noise — they are the fog's edge and transition tiles, which is why the top
four each appear ~132 times (the interior, tiled) while the rest appear a
handful of times each (the boundary).

So the model here is right for fog INTERIOR and wrong at every fog EDGE. Where
the guest draws a transition, this port draws the interior pattern hard against
terrain. That is the visual difference between the widened bands and the native
one, and it is not a phase bug — the phase work above was necessary and correct,
but it was never the whole story.

`g064` at columns 0..2 is the raw pre-expansion index again, so the expansion is
also progressive across a frame rather than atomic.

### What closing this needs

The rule that selects an edge tile. It is presumably autotiling on the
visibility of the neighbouring cells, which is exactly the kind of thing to
establish by measurement rather than assumption: for each observed BG2 tile,
record the 8-neighbour shroud pattern from `g_seen` and look for the mapping.
The port already has everything needed to collect that table.

Until then the widened fog is honest but coarse: correct where fog is solid,
abrupt where the guest would blend.

## The fog tileset is compact — and a warning about read timing

Listing every distinct BG2 value in a gameplay frame:

    [lb_bg2set] 13 distinct: 064 0B1 0B2 0B3 0B6 0BB 0BC 0BD 0BF 0C0 0C1 0C2 0C4

Every fog tile lies in **0x0B1..0x0C4**, plus `064` (the raw pre-expansion
index). So the fog tileset is one compact range, which suggests a much simpler
shroud test than the six-tile equality currently used: "BG2 within the fog
range" would recognise EDGE tiles as well as interior ones, with no
per-neighbour-pattern table.

Two things must be settled before relying on that:

* **`nonzero` is useless as a test.** Measured at render time, `BG2 != 0` reads
  768/768 -- every visible tile carries an entry. The earlier rejection of this
  test was right, and now for a measured reason rather than a suspected one.
* **Which value means "revealed" is not yet known.** In gameplay frames BG2
  holds only fog-range values, so the clear tile is presumably inside the range
  too. Identifying it needs a histogram taken in a genuinely MIXED frame.

## Read timing, and two invalid measurements

Two runs reported `[lb_bg2set] 1 distinct: 064:768` and were taken as evidence
that BG2 holds the raw index at read time. **Both were invalid: the route never
reached the battle.** It ended on the army-select menu, with the field visible
only in the Top Screen panel, and a menu's BG2 says nothing about fog.

The harness's `in_gameplay` check passed on that menu, so the run reported
success. That is a measurement-harness fault and it belongs on the same list as
the tick-units bug in docs/204: **a check that can pass on the wrong screen is
worse than no check.**

Moving the scan from `y == 0` to `y == 191` was done on the strength of those
invalid readings. It is harmless -- end of frame is a defensible place to read
what was drawn -- but it was not justified by the evidence claimed for it, and
the read-timing question remains open.

## 0x064 IS THE CLEAR TILE. The shroud test is one comparison.

With the harness no longer able to measure a menu by mistake, a full histogram
of BG2 in a real battle:

    064:248
    0B1:3 0B2:4 0B3:4 0B4:1 0B5:1 0B6:1   0BF:8   0C4:11      (33 edge tiles)
    0BB:87 0BC:87 0BD:75 0C0:82 0C1:82 0C2:74                (487 interior)

487 + 33 + 248 = 768, and the capture of that frame is about one third revealed.

So **`0x064` is the shroud layer's CLEAR tile.** It was recorded here as "the
raw pre-expansion index func_0203A4B4 writes" -- an interpretation that made a
menu reading `064` on all 768 tiles look like a bizarre timing artefact, when it
was simply a fully revealed screen.

That makes the shroud test one comparison:

    shrouded  <=>  (BG2 & 0x3FF) != 0x064

and it is **exact**: the port now reports `shrouded=520/768`, matching
487 + 33 precisely.

### Why this matters more than the tile values

Equality against the six INTERIOR tiles could only ever call an edge tile
REVEALED. Every fog boundary was therefore misplaced by up to a tile, in the
one place the eye is drawn to. The clear-tile test gets all 25 edge variants
right without enumerating any of them, and it needs no learned table, so
visibility is recorded from the first frame rather than after a fogged view has
been found to learn from.

Emitting the correct edge TILE in the widened columns is still not done -- the
interior pattern is used everywhere shrouded -- but that is now a texture
detail at the boundary rather than the boundary being in the wrong place.
