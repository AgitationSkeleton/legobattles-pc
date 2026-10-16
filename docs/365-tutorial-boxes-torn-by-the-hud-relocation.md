# 365 — Tutorial boxes torn apart by the HUD relocation

Status: **fixed, built, and verified at 21:9 and in the exactness gate;
deployed with 364 and 366** (`8f5a47a4`, sha256 `7787b871...`; Vanilla
240/240 identical).

## 1. What was wrong

At 1720x720 (21:9, magnification 1), the campaign tutorial's text boxes
came apart whenever a button was on screen. Before is exp-keep369, after is
exp-keep370; both follow the same steps.

- **Torn by a drag.** "You can group select up to 9 units at once." had its
  left part and the deselect button slide to the window's left edge. A hole
  opened through "group select", and the box's lower right stayed behind.
- **Flung to a corner.** Once the tutorial opened the whole map, the view
  zoomed out (a 1452x608 surface). The boxes went to the bottom-right
  corner, with half of each left in the middle of the field.
- **The button left behind.** Wherever the scan never reached it, the
  deselect button stayed at the DS position. The tutorial's own text said
  "touch the Deselect Icon in the bottom left corner".

## 2. What a tutorial box is

Read from the live polygon list (`gx_polygons`):

| part | quads | texture, palette | native x |
|---|---|---|---|
| the box | 72..150 tiles of 8x8 | 6DD01000, 46 | 27..227 |
| its text | 1, 256 wide, in front | 6DD01000, 32 | 32..288 |
| deselect button | 1, 24x24 | 6ED00000, 14 | 3..27 |
| combat buttons | 1..2, 24x24 | 6ED00000, 14 | 227..252 |
| sidebar brick | 2..4, 8x24 | 6ED00000, 26 | 4..20 |

In the reference replay, every HUD quad uses 6ED00000: the buttons, the
bricks, and the buildings list (19 quads).

## 3. Three faults, all in `lb_hud`

1. **The scan stopped at 64 quads.** Tutorial frames had 74, 106 and 152.
   The box was grouped from whichever tiles came first. The rest, and the
   deselect button drawn after them, were never seen.
2. **Adjacency merged neighbouring widgets.** The deselect button's right
   edge is the box's left edge (x 27). So button and box made one
   left-anchored element. With the combat button as well, they made one
   right-anchored element.
3. **The move wiped its own work.** The one-pass move vacated each quad's
   source right after writing its destination. A quad processed later then
   wiped anything an earlier quad had written on its source. The code's own
   note assumed "source and destination cannot overlap ... the box is
   capped at 64 px". A 200 px element moving 104 px breaks that.

A fourth weakness was fixed with them. The cached redraw used for vertical
moves matched last frame's capture by slot index and size only. With 150
identical tiles, a quad appearing ahead of them would have drawn each tile
from its neighbour for a frame. It is now matched by source rect.

## 4. The fix (`port/runner/src/lb_hud.cpp`)

- The scan holds 256 quads. The dropped-box counter, declared but never
  incremented, now counts.
- **Grouping:**
  - quads of one texture merge when they touch;
  - quads of different textures merge only when their interiors overlap;
  - `LB_HUDGROUP=0` restores adjacency alone.
- Every source is vacated before any destination is drawn.
- The cache is keyed by source rect.
- `LB_HUDLOG` now prints one line per element, with its textures and its
  move.

The result: the box is one element in the middle of the screen. It stays
put at 458x192 and only moves down at 1452x608. The buttons are their own
elements and go to their corners.

## 5. Verified

- **Exactness gate, under both groupings.** Identical instruction counts,
  8 of 8 layer files identical to gate-quiet, 59.8 fps.
- **The reference replay, 110 sampled element decisions per run.** The same
  elements and the same moves under both groupings. The only differences
  are two samples taken mid-resize, at different surface sizes.
- **The tutorial at 1720x720, the same steps before and after:**
  - every box is whole;
  - the deselect icon is in the bottom-left corner and the combat buttons
    in the bottom-right;
  - `qnmax` is 151, and neither cap was hit.

## 6. Not done

A message box that stretches between the deselect and combat buttons is not
built. That 3-slice stretch exists for a single-quad bar
(`31b95694`, which shipped marked UNVERIFIED). A tutorial box is a grid of
tiles with a text quad on top, so it keeps its native width.
