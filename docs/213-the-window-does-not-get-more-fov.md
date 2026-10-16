# 213 — What the window actually shows, and why more of it is hard

Two defects: widescreen does not scale with the window to provide more field of
view around the original aspect, and the HUD buttons do not fully follow the
window size.

Both halves are real. They have one shared cause and one separate one.

## Measured, not assumed

`LB_VERBOSE=1` prints the three rectangles that settle this:

    [lb_geom] window=1892x1031  guest=448x192  field=-257,0 2406x1031
    [lb_hud]  surface=448 visible=355 shift=49

The guest surface is **448x192 and always is**. `presentation.screen_widths[]`
is assigned once at window creation from `adaptive_max_width`, and never tracks
a resize.

The enhanced layout **covers** rather than letterboxes: it scales the surface to
fill the window and lets the excess fall outside. In an 1892-wide window the
destination is 2406 px, so **514 columns are cropped, 257 per side**, and only
about 355 of the 448 rendered columns are ever on screen.

## The HUD half: fixed

The move was anchored to the surface's edge, which on any window narrower than
2.33:1 is in the cropped region. A button moved to surface x 443 was thrown into
the part of the picture the player cannot see, so it simply vanished — worse than
leaving it where it was.

`lb_hud_set_visible_width()` now reports how many columns the layout is showing,
and the shift uses that instead. Two widths, kept apart deliberately:

* `extra = (surface - 256) / 2` converts surface x back to the guest's own
  coordinates, for deciding whether something is in a corner at all;
* `shift = (visible - 256) / 2` is how far it may actually move.

Verified at 1920x1080: the objective button now sits hard against the bottom
right of the window (`screenshots/exp-fov/corner-1920.png`).

## The FOV half: it does scale, but only with ASPECT, and it is capped

What the visible width actually does:

| window | visible guest columns | vs the DS |
|---|---|---|
| 4:3 | 256 | 1.00x |
| 16:9 | ~341 | 1.33x |
| 21:9 | 448 | 1.75x |

So it *is* aspect-adaptive. What it does **not** do is give more field of view
when the same-shaped window gets bigger — a larger 16:9 window shows the same
341x192, drawn larger.

And that is correct as far as it can go, because **the surface is 192 rows and
nothing extends it**. 341/192 is exactly 16:9. Given a fixed 192-row height,
341 columns is not a compromise — it is the whole of what a 16:9 window can show
without stretching.

### Two ceilings, both structural

1. **448.** Not this port's choice and not a config limit — it is baked into the
   vendored melonDS:

       void GPU3D::SetRenderWidth(u32 width) noexcept {
           if (width < 256) width = 256;
           if (width > 448) width = 448;

   with `u32 ScrolledLine[448]` and `ScrolledAttrLine[448]` sized to match.
   448/192 = 2.33:1, i.e. 21:9, is the widest the renderer can produce.

2. **192 rows, everywhere.** There is no vertical counterpart to
   `adaptive_width`. melonDS's whole pipeline is 192 scanlines and the runner's
   compositor loops `for (int y = 0; y < 192; ++y)` throughout.

So more field of view around the original aspect — more map in *both* directions rather
than a wider strip — needs the vertical extension, and that is a change to
vendored renderer code of a different order from anything done so far. It is not
refused, it is scoped: it touches melonDS's rasteriser, its scanline buffers, the
2D compositor, the capture path and the presentation, and it would want its own
dedicated piece of work rather than being bolted on at the end of another.

### What gives more field of view today

The **"View width (DS screens)"** slider in the Display tab, already wired: it
widens the GAME's own view rather than the raster, so the same 341 visible
columns cover more battlefield. That is the lever that is not capped at 448.

## Also corrected here

docs/209's retraction of the army-select claim listed, among its three mistakes,
"at 4:3 the width is 256 and there is no extension at all". The *visible* width
at 4:3 is 256, which is what that argument needed and it stands — but the
**surface** is 448 at every window shape, not sized from the aspect. The
retraction's conclusion is unaffected; the mechanism named in it was wrong.

---

## Shipped first: stop throwing away the columns that ARE rendered

Before touching the renderer, there is free field of view already being drawn and
discarded. The surface is 448 columns wide whatever the window is, and FILL
crops about a hundred of them on a 16:9 window.

New Display control, badged WIRED: **"Show every rendered column (bars instead
of cropping)"** (`LB_FIT=1`). It swaps the cover `max()` for a fit `min()`.

Measured at 1892x1031:

    FILL  field=-257,0  2406x1031   -> 514 columns cropped, ~355 visible
    FIT   field=0,110   1892x811    -> all 448 visible, 110 px bars

That is **448 vs 355 columns, +26% battlefield**, for the cost of bars. The HUD
follows either way -- with FIT it reports `visible=448 shift=96` and anchors to
the true surface edge.

This does not replace the vertical extension; it is the part of the answer that
did not need one. 448x192 is 21:9, so a 21:9 window needs neither option.

---

## The vertical extension: a concrete plan, not a wish

Investigated far enough to be executable. Recorded so the next attempt
implements rather than re-derives.

### Why it is the only thing that helps FILL

FILL is aspect-limited by arithmetic: a window of aspect A showing a 192-row
surface can show exactly `192 * A` columns, no matter how wide the surface is.
So **raising the 448 cap does nothing for FILL** -- at 16:9 you would still see
341 columns. It only helps FIT and windows wider than 3.33:1. More field of view
in a FILL window requires more ROWS.

### The width patch is the model, and it does NOT stretch

`0009-gpu3d-host-adaptive-render-width.patch` reads:

    enhanced_full_viewport = RenderWidth > 256 && Viewport[0] == 0 && Viewport[4] == 256;
    vw = enhanced_full_viewport ? RenderWidth : Viewport[4];
    vx = enhanced_full_viewport ? 0 : Viewport[0] + (RenderWidth - 256) / 2;
    posX = ((posX * vw) / den) + vx;

The second branch is the one this game takes -- units measure 24..31 px in the
polygon dump, native size, so nothing is being stretched. It simply **shifts** the
scene by half the overhang to centre it, and the extra columns then contain
whatever geometry survives clipping. Y is exactly symmetric:
`vy = Viewport[3] + (RenderHeight - 192) / 2`.

### The pieces, in dependency order

1. **melonDS soft renderer.** `MaxRenderWidth = 448` and `NumScanlines = 194`
   are already `constexpr` with `BufferSize = ScanlineWidth * NumScanlines`, so
   the colour/depth/attr buffers resize from one constant. Add `RenderHeight`
   beside `RenderWidth` (default 192, so the native path is untouched), raise
   `NumScanlines` to `MaxRenderHeight + 2`, and apply the `vy` shift above.
2. **The rasteriser's scanline bounds.** Seven hard `192`s in `GPU3D_Soft.cpp`.
3. **The 2D compositor.** `nds_gpu2d_render_scanline` guards
   `if (line < 0 || line >= 192) return;` and is driven from `io.cpp` as VCOUNT
   advances, so the extra rows have no hardware event to hang off. They must be
   rendered in a batch at end of frame using the same register state -- which is
   FAITHFUL for this game: the scroll callback writes BGxOFS once per frame
   (measured, `bgx 0 508 508 508` constant across a frame), so there is no
   per-line state to miss.
4. **Framebuffer and texture heights** in `g_fb`, the presentation texture, and
   the capture path.
5. **This port's own 192s**: 14 in `gpu2d.cpp`'s compositor plus `kExtRows` in
   `lb_widemap.cpp`, the `y < 192` tests in `lb_hud.cpp`, and `kScreenHeight`
   in the layout hook.

### The risk, stated plainly

This is a change to vendored renderer code touching rasterisation, buffer sizes
and frame timing at once, and its failure mode is subtly wrong pixels rather than
a crash. It wants its own pass with a capture after each step, and the
existing A/B harness (`--sizes-in-game`, `LB_VERBOSE=1`) is what makes that
checkable.

---

## Resolved differently: size the surface to the window

Adaptive window size should be one toggle (off meaning pure-vanilla rendering),
rather than multiple: it is one feature even if it encompasses many functions.

That is the better design, and it turned out to point at a better implementation
too. The FIT/FILL choice existed only because the surface was a fixed 448x192
that had to meet a differently-shaped window somehow. **Render at the window's
own aspect and the question dissolves.**

`nds_gpu3d_output_width()` is a single knob -- the 2D compositor takes its
`output_width` from it -- so one call per frame does both layers:

    lb_want = clamp(even(192 * win_w / win_h), 256, 448)

Measured on the threaded soft renderer:

    window=1252x671  guest=358x192  field=0,0 1251x671
    window=1892x1031 guest=352x192  field=1,0 1890x1031
    [lb_hud] surface=352 visible=352 shift=48

The field rect now **equals the window**: nothing barred, nothing cropped, no
column drawn and thrown away, and `surface == visible` so the HUD anchors to the
true edge with no correction at all.

### One caveat, found by measuring

`nds_gpu3d_set_output_width` refuses a change while the renderer is accelerated:

    if (g_nds.GPU.GPU3D.IsRendererAccelerated() &&
        width != g_nds.GPU.GPU3D.GetRenderWidth()) return false;

The default policy is `auto`, which picks the OpenGL compute renderer, so the
width is fixed when the session starts and a resize falls back to cropping to
fit. **The field of view is the same either way** -- a 192-row surface shows
`192 * aspect` columns however wide it is rendered -- so this costs wasted
rasterisation, not view. `NDS_3D_RENDERER=soft` re-sizes live.

### The controls now

* **Enhanced / DS Vanilla** -- the one toggle. Off is pure vanilla rendering.
  Widening, the HUD anchor and the surface sizing are all parts of it, not
  separate opt-ins.
* **HUD scale** -- separate, as asked: how big you want the buttons is a
  preference, not part of getting the window size right.

`fit_all` is retired to an `LB_FIT` override for comparison captures, and the
HUD anchor now follows Enhanced rather than being its own checkbox.

---

## The 448 ceiling is gone -- and the real ceiling is the VERTEX FORMAT

Consistent scaling across window sizes -- the game not "zooming in" when the
window is larger -- comes before other topics such as multiplayer.

### What was raised

`game.toml` said 448 was "the runner's own maximum (frontend_config.cpp rejects
anything outside 256..448 or odd)". **That was wrong, and this port had already
half-fixed it**: an existing hook raises `nds_gpu3d_set_output_width` and
`frontend_config` to 1024. The thing actually clamping was the vendored
melonDS, silently:

    void GPU3D::SetRenderWidth(u32 width) noexcept {
        if (width > 448) width = 448;          // -> 1024

with `ScrolledLine[448]` and `ScrolledAttrLine[448]` sized to match, and
`MaxRenderWidth = 448` / `NumScanlines = 194` behind the soft renderer's
buffers. All raised: **1024 x 544**, about 13 MB of extra buffer, which is
nothing on a PC.

### And it changed nothing, which is the finding

    window=1892x1031  guest=352x192
    window=1572x851   guest=354x192
    window=1252x671   guest=358x192

The surface width is chosen as `192 * aspect`, so with the height pinned at 192
it cannot exceed about 352 on a 16:9 window **however high the width ceiling
goes**. The 448 cap was never the binding constraint.

This is the arithmetic behind the zooming-in defect. A bigger window at a fixed
aspect gets the same `352 x 192` scaled up -- which is exactly "the game zooming
in". Consistent scaling means holding the pixel scale and letting the surface
grow with the window:

    surface_w = window_w / scale     surface_h = window_h / scale

At 1892x1031 and scale 3 that is 631 x 344 -- well inside the new ceiling, and
about 2.5x the native width and 1.8x the height. **Every part of that is now
possible except the height**, which is still 192 everywhere.

So the vertical extension is not one option among several; it is the whole
remaining piece, and the width ceiling had to come off first or it would have
capped the result anyway.


### Correction: 1024 was unsound. The limit is 512 x 256.

The first version of this raised `MaxRenderWidth` to 1024. That is not
supportable, and the reason is two lines in melonDS's vertex transform:

    vtx->FinalPosition[0] = posX & 0x1FF;   // X: 0..511
    vtx->FinalPosition[1] = posY & 0xFF;    // Y: 0..255

Those masks emulate the DS's own 9-bit X and 8-bit Y screen coordinates. **An X
past 511 wraps** and puts geometry somewhere else entirely. Nothing had actually
requested a width past 448 -- the surface is chosen as `192 * aspect`, so it had
never exceeded 352 -- which is the only reason no corruption was observed. It
was a latent bug, introduced and then caught by reading the transform before
building on it.

Corrected to **512 x 256**, which needs no mask change at all:

* 16:9 gets **455 x 256** -- 1.78x the native width, 1.33x the height, and
  **1.7x the visible area** against today's 352 x 192.
* The height is the half that is still missing; 512 x 256 is what the format
  allows once it lands.

Regression checked after the correction: `[lb_terrain] 0/768 differ`, 36.5 fps.

Going past 256 rows would mean widening `posY & 0xFF` itself -- host-only, in
the same spirit as the width patch, but it changes a field that emulates
hardware truncation and it is not needed for any ordinary monitor. 455 x 256 is
the target.

---

## The melonDS half of the vertical extension is in (inert)

Three hooks, all mirroring what patch 0009 did for width, all defaulting to 192
so this step changes nothing on its own. Regression after: `0/768 differ`,
42.3 fps, geometry still `352x192`.

**1. `RenderHeight` beside `RenderWidth`.** `GPU3D.h` gets the member, the
setter/getter and the `Renderer3D` virtual; `GPU3D.cpp` gets
`GPU3D::SetRenderHeight` and forwards on renderer swap; `GPU3D_Soft` gets its
own clamped copy. Clamped to **256** because `FinalPosition[1]` is masked with
`0xFF`.

**2. The vertical viewport shift.**

    const u32 viewport_y = Viewport[3] + (RenderHeight - 192) / 2;
    posY = ((posY * Viewport[5]) / den) + viewport_y;

SHIFT, never stretch. The X side has an `enhanced_full_viewport` case that
substitutes `RenderWidth` for the viewport width, but this game does not take
it -- units measure 24..31 px in the polygon dump, native size -- so the branch
that actually runs just moves the scene to centre it and lets the extra space
carry whatever survives clipping. Doing only the shift means nothing can be
distorted. At `RenderHeight` 256 the offset is 32, so a native 0..191 becomes
32..223 and stays inside the `0xFF` mask.

**3. The scanline counts.** The clear loops, the render loop and
`RenderedScanlines` all follow `RenderHeight` instead of a literal 192.

A note on method: the clear loop appears **twice** -- the textured-clear path
and the flat one -- and `replace()` insists on a unique match. That rule is
right, so each was anchored on its own preceding line rather than the check
being loosened.

### What is left, and it is the ndsrecomp half

melonDS can now produce more rows; nothing asks it to. Remaining:

* `nds_gpu2d_render_scanline` returns early for `line >= 192`, and the extra
  rows have no hardware event to hang off -- they must be rendered in a batch at
  end of frame. Faithful here: `BGxOFS` is written once per frame, measured
  constant at `bgx 0 508 508 508`.
* the 2D compositor's own `for (y = 0; y < 192; ++y)` loops, the framebuffers
  and the presentation texture;
* this port's `kExtRows`, `y < 192` in `lb_hud.cpp`, `kScreenHeight` in the
  layout hook;
* and finally the layout choosing `surface = window / scale` instead of
  aspect-only, which is the part the player actually sees.

---

## The ndsrecomp half, part one: the frame can now hold the rows

Still inert -- height stays 192 -- and regression clean: `0/768 differ`, 40.3
fps, geometry unchanged at `352x192`.

* `nds_gpu3d_set_output_height` / `nds_gpu3d_output_height` in the runner
  wrapper, mirroring the width pair, including the same refusal when the
  renderer is accelerated.
* `kMaxAdaptiveHeight = 256`, and `AdaptiveFrame` sized
  `kMaxAdaptiveWidth * kMaxAdaptiveHeight` instead of `* 192`.

### The structural finding that makes the rest tractable

`nds_gpu2d_adaptive_framebuffer()` is a **whole-frame** renderer. It has its own
`for (int y = 0; y < 192; ++y)` loop and calls `decode_bg_line` per row itself,
rather than being driven by the per-scanline `nds_gpu2d_render_scanline` that
`io.cpp` calls as VCOUNT advances.

That matters a great deal, and it removes the thing docs/213 had flagged as the
awkward part. The earlier plan said the extra rows "have no hardware event to
hang off" and would need a batched end-of-frame pass. **They already have one.**
The adaptive compositor is that pass, `decode_bg_line` computes
`line.sy = (bgy + y) & 0x1FF` and so is well defined for any row, and the
native 256x192 `Frame` -- which really is bound to the DS's own scanline
timing -- is a separate path that does not need to change at all.

So what remains is: the adaptive path's `y` loops, teaching that function to
report a height as well as a width, the presentation texture, and the layout's
`surface = window / scale`.

### A third 448, already dealt with

`kMaxAdaptiveWidth` was a separate 448 from melonDS's `MaxRenderWidth` and from
the runner's clamp. docs/166 had already found all three and raised this one to
1024; the hook added here would have been a duplicate and was dropped.

### Part two: the compositor knows about a height

Still inert; `0/768 differ`, 41.8 fps.

`nds_gpu2d_adaptive_framebuffer` now reads `output_height` alongside
`output_width`, derives

    const int extra_y = (output_height - 192) / 2;   // where guest row 0 lands

and sizes its clear and the HD emit buffers from it.

**The subtlety that runs through all of the rest.** The compositor's loop
variable is used as BOTH the output row and the guest scanline, and once the
height grows those are different things:

* melonDS's viewport shift has already moved the 3D scene down by
  `(RenderHeight - 192) / 2`, so **the 3D line is indexed by the OUTPUT row**;
* the 2D layers are still authored in the guest's own 192 rows, so **they are
  indexed by output row minus `extra_y`**.

Getting that backwards would slide the terrain against the units by 32 px --
which would look like a rendering bug and not like an off-by-one. While the
height is 192 the offset is zero and every expression is what it always was,
which is why this step remains inert.

### Two hooks withdrawn rather than forced

The "copy the native picture" paths -- engine B, and any scene the widener
declines -- could not be centred by patching upstream text, because this port's
own `lb_wide2d` hooks have already rewritten both blocks and the original text
is gone. `replace()` refused, correctly. Centring them means editing those hooks
instead; they are menus and non-3D scenes and are unaffected while the height is
192, so they belong to the step that turns the height on.

### Part three: the composite loop separates output row from guest row

Still inert; `0/768 differ`, 39.3 fps, geometry `352x192`.

    for (int y = 0; y < output_height; ++y) {
        const int gy = y - extra_y;
        ...
        decode_bg_line(0, hud_bgs[i], gy, ...);   // 2D: guest row
        line3d = ... y ...;                       // 3D: output row

`y` stays the output row -- the 3D line, the attribute line, the destination
pointer and the HUD shift all use it, because melonDS's viewport shift has
already moved the 3D scene down. `gy` is the guest's own scanline and the 2D
layers use that. At height 192 `extra_y` is 0 and `gy == y`.

OBJ is **clamped** to the guest's rows rather than extended, and the rows
outside get a cleared sprite line: sprites are authored for a 192-row screen and
there is nothing outside it to draw. (In this game OBJ is disabled outright
during gameplay in any case.)

**Anchored on the HD block above it**, because the same loop header
`for (int y = 0; y < 192; ++y) { render_obj_line(0, y, ...` also opens the
direct-present object path a few hundred lines earlier -- a different function
that must not be touched. `replace()` reported "appears 2 times" and refused,
which is the third time its uniqueness rule has stopped a wrong
edit.

---

## It works, and it exposes exactly the problem that was predicted

`LB_VFOV=1` turns the extra rows on. With it:

    window=1892x1031  guest=472x256  field=-4,0 1901x1031  scale=4.027

**472 x 256 against the old 352 x 192 -- 1.79x the visible area, filling the
window, at a constant magnification.** That is the goal, and
the render path does it.

### The sizing had to be reordered

The first attempt sized the surface and then re-derived the presentation scale
with the old cover/fit rule, whose "never show less than the DS" cap then
shrank it -- and a short window came out **pillarboxed**, bars down both sides.
The scale has to be settled FIRST, capped so 256x192 always fits, and the
surface sized from it; then presenting at that scale fills the window by
construction and cover only absorbs the even-size rounding.

### And the predicted failure is exactly the one that happened

The top of the screen fills with **stale sprite tiles and checkerboard**
(`screenshots/exp-vfov3/game-1920x1080.png`). The extra rows read tilemap rows
the guest never maintains -- precisely the problem `lb_widemap` already solves
for COLUMNS, flagged well before it appeared.

`lb_widemap`'s row index still starts at zero:

    const int kExtRows = 26;
    ...
    if (trow < 0 || trow >= kExtRows) return 0;

Columns get an origin (`kExtLo = -16`) and rows do not, so there is nothing to
serve a row above the guest's own 24. Rows need the same treatment: a
`kExtRowLo`, the scan filling them, and the three suppliers accepting negatives.

### So the extra rows are OPT-IN for now

`LB_VFOV=1`. A default session keeps 192 rows and only the width adapts, because
shipping a garbage band across the top would be worse than not having the
feature. Default verified clean at 1920x1080, `0/768 differ`, 41.9 fps.
