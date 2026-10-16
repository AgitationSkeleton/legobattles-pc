# 168 — The whole battlefield renders host-side, from live guest memory

Status: **working**, with one characterised gap. Tool: `tools/lb_field.py`.

## Why

docs/167 measured that the emulated 2D path can never show more field than it
already does — BG1/BG2 are a scroll double-buffer, so one screen's worth of
terrain exists in VRAM at a time and a wider canvas is a wider black bar. The
goal, a large window (1920x1080, or an ultrawide 3440x1440) showing the whole
battlefield, is unreachable that way.

It is reachable the other way. The level is stored as a grid of metatile indices
in main RAM; that grid describes the ENTIRE map, not the visible window. Reading
it directly and drawing it on the host has no DS hardware limit at all.

## The structures, and how each was located

Nothing here is a guess; each was pinned by matching against what the guest was
displaying at the time, and `tools/lb_field.py` re-derives all of it at runtime
rather than hardcoding the addresses below.

**Metatile geometry.** Self-similarity of the live BG1 map is 82% at horizontal
shift 3 and 82% at vertical shift 2, and near zero at every other small shift.
A metatile is **3 tiles wide by 2 tall = 24x16 px**.

**Metatile table — `0x022BC0CA`, 12 bytes per entry (6 tile indices).**
Two weaker methods found the wrong thing first and both failures are worth
keeping:

- taking the first RAM occurrence of a displayed 3x2 block landed *inside the
  level grid*, because a wrong-phase block occurs somewhere by chance;
- bounding a candidate by "all six values look like tile indices" also accepts
  the level grid, whose metatile indices are small numbers too.

What works is a signature the grid cannot imitate: the table is generated, so
its first entries reference tiles 0..5 then 6..11. Seven addresses in main RAM
carry that signature; exactly one of them (`0x022BC0CA`) is both *below* a
uniquely-located on-screen block and 12-byte aligned with it. That block — the
common grass metatile at `0x022BC63A` — lands at index 116, and 116 is a value
that actually appears in the grid. Two independent facts, one answer.

**Window phase.** Of the six possible (x,y) phases, only x=1,y=0 resolves the
displayed blocks against the table (73% versus 0% for four of the others).

**Level grid — `0x0227F738`, u16 per cell, 64 cells wide.** Found by extracting
the visible 10x12 metatile window and searching RAM for it in two dimensions:
the most varied row is the probe (a row of pure grass matches everywhere), and
every other row must agree at the candidate stride. Across 14 candidate widths
and the whole 4 MB of main RAM this yields **exactly one** surviving candidate:
probe row at `0x02280138`, width 64, 88 cells verified. Walking out from there
while cells stay valid gives 63 rows.

    map = 64 x 63 metatiles = 1536 x 1008 px

**Tiles** at `0x06004000` (BG1 char base) and **palette** at `0x05000000`, both
straight from the registers.

## Result

`python tools/lb_field.py --out field.png` writes the whole battlefield in about
1.7 s: the central lake, the sand-and-cliff ridges, the forest belts, all of it,
1536x1008. A 1920x1080 window holds the entire map at 1:1 with room to spare;
3440x1440 holds it at 1:1 with a wide margin.

That is the view the goal calls for, produced without touching the DS 2D
limits at all.

## The second table: what the 15.8% was

Colour index 0 is transparent on the DS and tile `0x016` is the blank tile.
A first render left 15.8% of pixels as backdrop. Four explanations were tested
and all four are wrong, which is what pointed at the real one:

- *Not* a streamed tileset. Of the 432 distinct tile indices the whole map
  references, **430 are already resident**; only tiles 22 (`0x016`, the blank)
  and 965 are empty.
- *Not* distance-dependent. Transparency by distance from the camera is flat at
  13-18% in every band, and the visible window is *more* transparent (26.4%)
  than the map average. Region streaming would show the opposite.
- *Not* a per-tile composite of two tables. Compositing either way round scores
  exactly as well as the better table alone.
- *Not* a second grid. The array after the grid is 98% `0xFFFF`.

What it is: **a second metatile table, indexed by the same cell value.**

For the 32 of 120 visible cells the primary table cannot explain, the displayed
block does occur in RAM -- all of them inside `0x02283862..0x02283C6A`, and all
congruent to 10 mod 12. A clean 12-byte lattice, and a different one from the
primary table's. Solving `base = address - cell_value * 12` over those cells
gives a single consistent answer:

    primary   metatile table   0x022BC0CA
    secondary metatile table   0x022822AE
    level grid                 0x0227F738   (indexes BOTH)

The two tables are disjoint and complementary on screen: the primary explains
88 cells, the secondary the other 32, 88 + 32 = 120 exactly. Each cell uses one
table or the other, never a blend.

Selection needs no magic constant. A threshold anywhere in 133..450 explains
120/120, which is a wide enough range to show that a threshold is not the real
rule. This is, and it is self-validating:

    use the primary table, UNLESS its entry references a tile that is blank
    in VRAM -- then the cell is a secondary-table cell.

That also scores 120/120 on the live screen, and applied to the whole map it
selects 644 of 4032 cells (16.0%), matching the 15.8% of pixels that were
missing.

With both tables wired the render leaves **0 backdrop pixels out of 1,548,288**.
The whole battlefield is complete and correct.

## Screenshots

`screenshots/20260831-095058/battlefield_full.png` — the whole map.
`screenshots/20260831-095058/hostrender_bg1.png` — the same renderer restricted
to the visible window, which reproduces the on-screen terrain (grass, the
tree cluster, the sand patch) and validated the decode before it was scaled up.
