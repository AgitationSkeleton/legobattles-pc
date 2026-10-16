# 223 — The buildings list follows the edge

**Goal within the project:** HUD repositioning and scaling, and a quick way to
reach the HUD that matters: tap the builder (the grey robot minifig in Free
Play, the blue figure with black hair in the King story) or the hero (the king
in the story, the red space criminal in Free Play) to show those HUD elements;
then tap the red brick to view buildings as the builder, click an icon, place
it in the world on a green space, and tap the checkmark to command the builder
to build.

That sequence puts up the richest in-field HUD the game has, which makes it the
thing anchoring should be tested against — not the two or three quads a bare
selection shows.

## Getting there automatically

`--build-flow` follows that sequence. Three things make it work where earlier probes
did not:

**It taps rather than clicking through the frontend.** docs/222 measured the two
against one target in one frame: `tap` selects, `frontend_input` does not.

**It aims at the builder**, which needed the builder's position to be printed at
all. `[lb_ent]` only reported category-1 units when they happened to be in a
widened column — the one case where a harness does not need telling. `[lb_builder]`
now reports the player's builder wherever it is, like `[lb_hero]` and `[lb_base]`.

**It reads the buttons instead of guessing them.** The port prints every
on-screen HUD quad's rect with `LB_HUDLOG=1`, and those quads *are* the buttons.

Two bugs in that reading, both worth recording because both produced confident
nonsense:

- The port prints the summary line FIRST and the per-quad lines after it. The
  parser collected the lines *before* the summary and found nothing.
- The rects are in SURFACE coordinates — the widened 392×212 — while the
  cartridge's hit test works in the 256×192 it still believes it has. Tapping the
  surface number lands 68 px into the field: tapping `136,52` selected the
  **base**, and the capture shows it ringed with the HUD untouched. They differ by
  the `shift=` the summary carries.

With both fixed the sequence runs: builder → 6 quads, blue brick → 13, red brick
→ **18** (the buildings list), bottom-left → 0 (cancel).

## What it found

Every one of the 18 quads reported `-> +0,-10`. Anchored to the top, moved not at
all sideways — so opening the list made the two sidebar bricks **jump 68 px to
the right**, out of the corner they had been sitting in, and the whole bar floated
in the middle of the field with green either side.

The cause is the interaction between two correct-looking rules. Grouping
(docs/219) merges quads that touch, so the bar and the bricks are one element,
232 px wide. And the anchoring decision required an element to be narrower than
`kCornerMaxWidth = 64`.

## The cap was a proxy; the edges are the thing

The width cap's stated reason was real:

> The same frame that contains the 24-px button also contains a 255-px-wide quad
> at HUD depth whose right edge is well past the native screen, and an edge test
> alone would have picked that up and thrown it 96 px sideways.

But size was standing in for something the edges state exactly. An element
touching **both** sides SPANS the screen: it cannot be put against both window
sides by moving it, and it wants stretching, which is a separate job. An element
touching **one** side is against that side whatever its width, and belongs against
the corresponding window side.

The 255-px quad that motivated the cap touches both, so the edge test excludes it
for the right reason rather than by being big. The buildings bar spans native
3..235 — hard against the left edge, 21 px clear of the right — so it is
left-anchored and there is nothing ambiguous about it.

Both caps are gone, replaced by:

```cpp
const bool at_left  = nx0 <= kEdgeSlack;
const bool at_right = nx1 >= 256 - kEdgeSlack;
if (moving && shift   > 0 && w > 0 && at_left != at_right)
    dx = at_right  ? shift   : -shift;
if (moving && shift_y > 0 && h > 0 && at_top  != at_bottom)
    dy = at_bottom ? shift_y : -shift_y;
```

The height cap goes the same way and for the same reason — docs/219 had already
had to raise it from 32 to 72 to admit the sidebar brick, which was the same
proxy failing the same way one size earlier.

## Measured, after

All 18 quads report `-68,-10`, and the capture shows the buildings bar starting
at the window's left edge with the blue brick above it, exactly where the bricks
sit when nothing is open. No jump on opening the list, nothing floating.

## Still open

An element against **both** edges is still declined, which is correct as far as
moving goes but means a genuinely full-width bar would sit in the native columns
with field either side. Nothing in the field HUD does that today — the buildings
bar stops 21 px short — so this is recorded as the known limit of the approach
rather than a defect anyone has seen.
