# 194 — The in-field HUD is 3D geometry, and clicks work

Status: **measured.** Settles bug 4's mechanism, which every previous attempt
had wrong, and clears bug 2.

## The HUD did not exist in any frame measured before this

The unit-action, build and cancel buttons only appear **when something is
selected**. Every capture taken in this project until now was of an idle field
with nothing selected, so "the HUD elements don't move to the corners" was being
investigated against frames that had no HUD drawn in them at all.

`tools/lb_exp.py --select X Y` taps a guest coordinate first. With the king at
guest (149, 105) selected, the buttons appear: two at the upper left, a red
cancel at the lower left.

## Clicks work

The tap selected the unit — it gains a highlight and a health bar. So the
simulation is receiving and acting on touches. Bug 2's "clicks unresponsive" is
not a dead input path; what remains is latency, which at 40-43 fps is a frame
rate problem (bug 1), not an input one.

## Which layer draws it: BG0, the 3D layer

`LB_BGMASK` clears DISPCNT bits inside `render_engine_line`, with a unit
selected in every case:

    mask     DISPCNT   result
    0x0800   2708      BG3 off  -- buttons STILL THERE
    0x0400   2B08      BG2 off  -- buttons STILL THERE
    0x0100   2E08      BG0 off  -- buttons GONE, and so are the castle,
                                   the units, and every building. Terrain only.

**The HUD is drawn by the 3D engine**, exactly like the units and buildings.

And OBJ is off the whole time. `DISPCNT` reads `2F08` before the selection and
`2F08` after it: **bit 12 stays clear even while the HUD is on screen.** So the
HUD is not sprites at any point, and this is not a case of OBJ being enabled
only when something is selected.

## What that means for the fix

`adaptive_hud_anchor` cannot reach this HUD, and never could. It has exactly two
mechanisms and neither applies:

* an OBJ path, which remaps sprite X positions — and there are no sprites;
* a BG band path, which remaps `hud_x` per output column.

The BG band path is worth spelling out, because it is what made the field
striping (docs/193). `hud_x` is computed **once per output column and then
applied to every BG in `hud_bg_lines`**:

    int hud_x = -1;
    if (g_adaptive_hud_anchor) { ...left/centre/right bands... }
    else if (x >= extra && x < extra + 256) { hud_x = x - extra; }
    for (size_t i = 0; i < hud_bg_count; ++i) { ... line.color[hud_x] ... }

There is no per-layer distinction, so anchoring remapped BG1 and BG2 — the
512x256 layers that scroll with the world — along with anything that was
actually screen furniture. That is the field being cut into pieces.

A per-layer version would be well founded here, because the layers differ
structurally in a way that was measured rather than assumed: the world layers
are **512x256** (`BG1CNT 4187`, `BG2CNT 4081`, size bits = 1) while `BG3CNT
0284` is **256x256**, and every `BGxHOFS` is zero. But it would not fix bug 4,
because the buttons are not on any of them.

Moving this HUD means moving 3D geometry. The usual handle is that DS titles
draw 3D UI with an orthographic projection while the world is perspective, so
the two can be told apart in the geometry stream and the UI translated to the
widened edges. The runner exposes `gx_state` and `gx_polygon`/`gx_polygons`,
which is where that classification would be checked before any of it is built.
Not attempted yet.

## Status of the eight

    1 laggy              located (docs/193), not fixed. Needs ~1.45x in guest
                         execution; no single lever.
    2 clicks             INPUT PATH WORKS -- a tap selects a unit. Residual
                         sluggishness is the frame rate.
    3 black bars         FIXED (docs/193), 100% fill verified at six sizes.
    4 HUD at corners     mechanism now known: 3D geometry. Not fixed, and the
                         setting previously relied on could never have done it.
    5 left-edge          characterised (docs/193); windows ruled out.
    6 click past aspect  moot in practice: outside the guest's 256 columns
                         there is nothing drawn and nothing to click.
    7 static map         WORKS, verified by panning to open sea.
    8 striping           FIXED (docs/193). It was the anchor.
