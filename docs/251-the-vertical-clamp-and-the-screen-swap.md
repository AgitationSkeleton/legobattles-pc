# 251 — Geometry is clamped vertically, and the screen swap is disallowed

Status: **the truncation cause is located; the swap is fixed.**

## Sprites cut at the top: measured

The report: nudging the camera down cuts the base and the top of the earth
unit.

First suspect cleared. The entity gather rect's **top edge is widened**, and
adaptively -- `[lb_cull] gather rect top edge camy-16 -> camy-528 px (1 -> 33
metatile rows)`. The game is submitting the entities.

The polygon list then locates it. One run, camera panned **down** through
`cam y` 88 -> 408, 136 polygons:

    guest x range:  -84 .. 187
    guest y range:    0 .. 192

**Horizontally geometry reaches 84 px outside the native columns. Vertically it
never leaves the DS's own 192 rows.** That asymmetry, in one measurement, with
the camera moving in the axis under test, is the truncation: an entity above
the camera is gathered, and then the vertex pipeline gives it no rows to be
drawn in.

This is the same shape as the two earlier horizontal-only widenings (the gather
rect, the view dimensions) and the vertex Y mask found alongside it -- four
now. When a widening is added in this port, **the vertical twin is the thing to
go looking for.**

### RETRACTED: scaling the viewport height is not the fix

The obvious mirror of `enhanced_full_viewport` -- which swaps `RenderWidth` in
for the viewport WIDTH -- is to swap `RenderHeight` in for `Viewport[5]`. It
was tried. **It stretches the sprites**, measured by polygon size on the same
route with one variable:

    before   dominant poly height 24 (109 polys), 56
    after    dominant poly height 42  (43 polys), 98

24 -> 42 and 56 -> 98 are both exactly `RenderHeight / 192` = 1.75. Scaling
normalised Y over more rows magnifies the picture; it does not show more of
the world: it stretches sprites vertically, which is visible at a glance.
Reverted; heights measure 24 dominant
again.

### ...and that retracts the diagnosis above it, too

Widths were **unchanged** across that same pair. If the game took the
`enhanced_full_viewport` branch, substituting `RenderWidth` would have
stretched them the same way. It did not, so this game does not take that
branch -- exactly as the note beside it always said -- and that branch is
therefore **not** what buys the horizontal reach either.

Which undermines the "guest x -84 vs guest y 0..192" asymmetry this document
was built on. `guest x` in the polygon dump is `surf_x` minus the band read at
PRINT time, while the position was transformed with the band read at TRANSFORM
time. When those two disagree -- and they can, the whole
`lb_band_for_geometry` / `lb_band_promote` chain exists because they can -- the
subtraction reports negative guest x with no geometry out there at all.

So the honest state is: sprites truncate at the top, the gather rect is not the
cause, and the mechanism is **not yet identified**. The next measurement must
compare like with like -- the same band on both sides of the subtraction, or
surface coordinates against the band printed in the same line.

### The vertex Y mask, fixed separately

`FinalPosition[1]` was masked `0x1FF` -- 511 -- while `kMaxAdaptiveHeight` is
768, so on any surface over 512 rows a vertex below row 511 wraps to the top of
the picture. A 620x1100 window gives a 296x526 surface, already past it. Raised
to `0x3FF`. This is a real bug and was found on the way, but it is **not** the
clamp above: the clamp holds at surfaces far shorter than 512 rows.

## The screens switching places

POWCNT1 bit 15 routes engine A to the top LCD when set and the bottom when
clear, and **LEGO Battles flips it between states** -- measured `0x820F` at the
title and `0x020F` in a battle. On a DS that is correct and invisible: both
screens are in front of the player and the picture moves to the other one.

In the enhanced single-window layout it is very visible. The window draws one
physical screen, so the moment the guest flips that bit the window shows the
other engine -- the field replaced by a HUD, or a menu by the minimap: the
screens appear to switch places. The swap therefore has to be disallowed while
adaptive screen size is on.

Enhanced now **pins** the routing: engine A always lands on the buffer the
window presents. Both routing sites go through one helper so they cannot
disagree -- the scanline write that files a rendered line under a screen, and
the adaptive compositor that maps a screen back to its engine.

Nothing writes to POWCNT1. Verified: it still reads `0x020F` in play, so the
guest's own view of its registers is untouched; this changes only which
physical buffer each engine's scanlines are filed under, which is a
presentation choice the host is entitled to make. Vanilla still follows the bit
exactly.

No key binding was involved. The harness pans with `0x80`, which is KEYINPUT
bit 7 (Down) -- the same table in which the harness's own aiming uses 0x10 and
0x20 for Right and Left. What reached the swapped state was a routing tap that
opened a unit panel, and the flip came from the game's own state change.

## Measured at last: the branch IS taken, and the axes are not symmetric

Two passes argued in opposite directions about whether this game takes
`enhanced_full_viewport`, one from negative guest x and one from unchanged
sprite widths. The values were always available. `LB_VIEWPORTLOG=1`:

    [lb_viewport] full=1  Viewport[0]=0 [3]=0 [4]=256 [5]=192
                  ->  x=0 w=484 y=72 h=192   RenderWidth=484 RenderHeight=336

**It is taken.** Horizontally the viewport becomes `x=0, w=RenderWidth` -- the
whole surface. Vertically it stays `y=band, h=192` -- the guest's own rows,
shifted and no more. `posY` is unsigned and `viewport_y` is added to a
non-negative term, so **nothing can ever map above the guest's first row**.
That is the truncation, measured rather than argued.

### The retraction of this was itself wrong

The earlier note above withdrew this diagnosis on the grounds that sprite
widths did not change. That reasoning is invalid: the A/B it rested on varied
only the HEIGHT substitution, so it could not test the width branch at all.
The diagnosis stands; only the proposed fix was wrong.

### And the fix is still not "mirror it"

Substituting `RenderHeight` for `Viewport[5]` remains measured as a stretch --
dominant sprite height 24 -> 42, exactly `RenderHeight/192`. So the two axes do
not behave alike under the same substitution, and that is the thing to explain
before changing anything: the horizontal substitution is in force right now at
`w=484` against a native 256, and the picture is not reported as horizontally
stretched.

Either the horizontal case IS stretching and nobody has measured a native
baseline to compare against, or the game's geometry is not uniform in the two
axes (billboarded sprites sized in screen space would do it). A clean
before/after of sprite WIDTHS with the width substitution forced off is the
next measurement, and it is one variable.

**No fix attempted this pass.** Three wrong answers on this bug -- two
diagnoses and one stretch -- are enough to justify measuring the width axis
properly before touching the height one again.

## The horizontal substitution stretches too, and always has

`LB_NOFULLVP=1` forces `enhanced_full_viewport` off so the two can be compared.
Same route, same window, one variable:

| substitution | viewport | dominant sprite width |
|---|---|---|
| on (the default) | `x=0 w=484` | **24** |
| off | `x=114 w=256` | **13** |

`24 / 13 = 1.85`; `RenderWidth / 256 = 484 / 256 = 1.89`.

**So the horizontal path is not a working model to copy. It is the same stretch
as the vertical experiment, already shipped.** Widening `viewport_width` scales
normalised X over more columns; it magnifies the scene rather than showing more
of it, exactly as substituting `RenderHeight` magnified it vertically.

This also corrects a claim that has stood in `apply_hooks.py` for some time:

> the X side has an `enhanced_full_viewport` special case that substitutes
> RenderWidth for the viewport width, but this game does not take it -- units
> measure 24..31 px in the polygon dump, native size

Both halves are wrong. The game **does** take it (`full=1`, measured), and the
24 px that was read as "native size" is the *stretched* size; native is 13.

### What this means for the truncation

The asymmetry is real -- X covers the whole surface, Y only the guest's rows --
but the horizontal half achieves its coverage by magnifying, not by widening
the frustum. So "make Y match X" was never the right goal, and the stretch
seen in the vertical experiment is present horizontally in
every build shipped so far.

A correct fix widens the **frustum** on both axes: more world at the same
pixels-per-world, which means scaling the projection, not the viewport. That is
a different and larger change, and it wants the vanilla renderer as its
reference -- with Enhanced off everything is native, so unit size against
terrain features there is the baseline any fix has to reproduce.

**Not attempted here.** The measurement above is the useful product of this
pass: it rules out the approach two separate attempts were built on.

## RETRACTED AGAIN: the horizontal substitution does NOT stretch

The section above concluded, from an ON/OFF pair at one surface size, that
`enhanced_full_viewport` magnifies by `RenderWidth/256`. **That is wrong.** It
compared ON against OFF and attributed the difference to ON, without a native
reference to say which of the two was the anomaly.

Adding the missing size, sprite width against surface width:

| config | RenderWidth | width | height |
|---|---|---|---|
| substitution ON | 276 | **24** | 24 |
| substitution ON | 484 | **24** | 24 |
| substitution OFF | 484 | **13** | 24 |
| + vertical ON | 484 | 24 | **42** |

Width is **24 at both 276 and 484**: invariant to how wide the surface is. The
horizontal substitution therefore preserves native sprite size and extends
COVERAGE, which is exactly what it is for. Turning it off is what distorts --
it shrinks sprites to `24 * 256/484 = 12.7`, measured 13 -- and the picture
confirms it: with it off the base is visibly narrow against terrain that has
not moved.

So the claim in the previous commit that "units are ~1.9x too wide in every
shipped build" is withdrawn. They are the right size.

## And the two axes genuinely differ

The vertical twin does not behave like it. With `LB_FULLVP_Y=1` at
RenderHeight 336, heights go 24 -> 42, exactly `RenderHeight/192`, while widths
stay 24. The picture shows the base visibly elongated -- the vertical sprite
stretch, confirmed against a native reference rather than taken on trust.

So: the same substitution is size-preserving on X and size-scaling on Y. That
asymmetry is a property of how this game emits its geometry, not of the code
being mirrored wrongly, and it is the thing to understand before any further
attempt. `LB_FULLVP_Y` is left in, defaulting OFF, so the next attempt can A/B
against it instead of rebuilding the experiment.

**Four wrong answers on this bug now**, one of them visible in a single
glance at the picture. Every one came from comparing two
configurations without a third that is known-correct.

## A lead, recorded rather than claimed

Reaching for a genuinely native reference, the port was switched to Vanilla
(`settings.json`, `enhanced: false`) for one run. It logged

    [lb_viewport] full=1 ... x=0 w=512 y=0 h=192  RenderWidth=512 RenderHeight=192

with Enhanced **off** -- suggesting the toggle does not reset the 3D output
width, which would mean Vanilla is not pure-native for the 3D path and the
requirement that the off position is pure-vanilla rendering is not met.

**Not claimed**, for two reasons. `[lb_viewport]` is a one-shot at the first
vertex transform, which fires before the frontend has applied the vanilla
layout -- the same timing trap that made the tileset dump print twenty rows of
zeros until it was gated on `fog_learned()`. And the run logged no polygons at
all, so it may not have reached gameplay.

Worth testing properly: gate the probe on being in a battle, and confirm
whether `nds_gpu3d_set_output_width` is reset when Enhanced is turned off. The
setting was restored to `true` immediately afterwards.

Also noted for whoever tests it: the width measurement that still needs a
third, known-correct point is "does the SAME polygon change size across surface
widths". Comparing the modal width across configurations cannot answer that,
because the mode may be a different object at each size -- which is the most
likely reason the 24-at-276 and 24-at-484 readings look impossible against the
arithmetic, where `posX = ndc * viewport_width / den` must scale.

## Settled: identity-matched polygons against a true native reference

Every earlier reading here compared MODAL polygon sizes, which cannot answer
whether a given sprite scales -- the mode may be a different object at each
surface size. Matching polygons by `(submission index, tex, pal)` and adding a
third configuration with `RenderWidth = 256`, where the substitution cannot
apply at all (`[lb_viewport] full=0 ... w=256`):

| polygon | NATIVE 256 | X-only 484 | X+Y 484 |
|---|---|---|---|
| sub 1 | 24 x **24** | 23 x **24** | 23 x **42** |
| sub 2 | 25 x **24** | 24 x **24** | 24 x **42** |
| sub 0 | 20 x 56 | 55 x 56 | 55 x 98 |

**Horizontal: size-preserving.** X-only width equals native width (23 vs 24,
24 vs 25) while the surface is nearly twice as wide. Turning the substitution
off compresses the same polygons to 13 and 12, about native/1.89 -- so OFF is
the anomaly, not ON.

**Vertical: a genuine stretch.** Height goes 24 -> 42 against a native 24,
exactly `RenderHeight/192`. The vertical sprite stretch is confirmed against
a native reference rather than against the
other broken configuration.

(sub 0 disagrees on width and is not leaned on; it is most likely a different
object instance between runs, since the hero walks.)

### Which retractions stand

This vindicates the retraction in `38c958f5` and discards the claim in
`93ae09f1` that units are ~1.9x too wide in shipped builds. They are the right
size. Both wrong answers came from the same flaw: comparing two configurations
by a statistic over different objects.

### The mechanism is still unexplained, and that is the next question

`SoftRenderer::SetRenderWidth` only clamps a variable and clears
`FrameIdentical`; there is no projection compensation anywhere, so by the
arithmetic `posX = ndc * viewport_width / den` the X substitution *should*
scale sprites and measurably does not. Something keys the NDC-to-pixel mapping
to `RenderWidth` in a way that has no `RenderHeight` counterpart, and finding
it is what makes a correct vertical fix possible. Scaling `Viewport[5]` is not
that fix, measured twice now.

## RETRACTED: the "native reference" run was not native

The comparison above used a run at a 256x192 window as the native reference.
Its `[lb_viewport]` line did read `full=0 ... w=256` -- but that is a one-shot
at the first vertex transform, and the polygons it logged came from surfaces
**946, 1266 and 1706**, all from the end-of-run size sweep. The run logged no
256-wide surface at all. The "NATIVE" column was not native and the three-way
table is withdrawn.

## Corrected: matched by identity AND by surface width

Comparing only polygons logged in frames at the same surface width:

| config | sub 1 width at 484 / 946 / 1266 |
|---|---|
| X-only | **23 / 25 / 23** -- invariant |
| X-off | 13 / 7 / 5 -- shrinks as `256/RenderWidth` |
| X+Y | widths as X-only; **heights 42 / 65 / 87** -- grow as `RenderHeight/192` |

The conclusion survives on better evidence: the X substitution is
size-preserving, turning it off compresses, and the Y equivalent magnifies.

**And the useful new fact:** under X-only, sprite HEIGHT is 24 at every surface
size. Vertically the sprites are already at native scale. Vertical scale was
never the problem -- **coverage** is.

## The mechanism, at last

Three measurements agree:

* Patching the game's view height (`LB_VIEW_HEIGHT=384`, `[lb_fov] view height
  24 -> 48 tiles` confirmed firing) leaves `Viewport[5] = 192`. The 3D viewport
  comes from the guest's own GX VIEWPORT command, not from the view-dimension
  object, so that patch cannot buy 3D coverage.
* Geometry reaches `guest x -114` -- outside the native 256 columns -- which can
  only happen if the projection puts world there and the DS viewport was
  clipping it away.
* `SoftRenderer::SetRenderWidth` only clamps a variable, and every `RenderWidth`
  use in the rasteriser is a clamp or a stride. Nothing compensates.

So **the game's projection is horizontally over-wide and vertically exact.**
Widening `viewport_width` reveals frustum that was already there, at unchanged
scale -- which is why X is size-preserving and why it works at all. Vertically
there is no spare frustum, so widening `viewport_height` can only magnify.

### What a correct vertical fix looks like

Not the viewport. The **projection**: scale its Y by `192/RenderHeight` while
setting `viewport_height = RenderHeight`. Then

    posY = (ndc * 192/RenderHeight) * RenderHeight / den = ndc * 192 / den

-- identical pixels-per-world, so no stretch -- while the NDC range now covers
`RenderHeight/192` times more world. That is the vertical twin of what the
horizontal axis gets for free from the game's own projection.

Implementing it means intercepting the projection matrix rather than the
viewport, which is a different seam and has not been attempted.

## FIXED: widen the vertical frustum at the clip matrix

Derived from the mechanism above rather than guessed, which is what separates
this attempt from the four before it.

`Position[1]` is built from `ClipMatrix[1,5,9,13]`, and `UpdateClipMatrix` is
where the projection enters -- **before** the geometry engine clips. Scaling
those four entries by `192/RenderHeight`, with `viewport_height = RenderHeight`
and `viewport_y = 0`, gives

    posY = (k * P1) * RenderHeight / 2w,  k = 192/RenderHeight
         = P1 * 192 / 2w

-- the native pixels-per-world exactly -- while the NDC range now spans
`RenderHeight/192` times more world. Coverage without magnification.

Doing it at the viewport alone could never work: the frustum clip runs first,
so there is no geometry out there to reveal.

### Verified on both criteria

Polygons matched by `(sub, tex, pal)` **and** by surface width, at 484:

| polygon | X-only baseline | clip-matrix fix | viewport-only (the stretch) |
|---|---|---|---|
| sub 0 | 55 x 56 | **55 x 56** | 55 x 98 |
| sub 1 | 23 x 24 | **23 x 24** | 23 x 42 |
| sub 2 | 24 x 24 | **24 x 23** | 24 x 42 |

Sizes identical to the baseline, one off-by-one from fixed-point rounding.

And polygons now appear **above the band top** -- `topY < band_y` -- which had
never happened in any previous measurement: 6 of 178 in the default build, 0
before. The capture shows the base correctly proportioned, not elongated.

On by default; `LB_FULLVP_Y=0` turns the pair off. The count is modest because
few entities sit above the camera in the test scene; the case to watch is the
reported one, panning down with the base above the view.

## The clamped band broke it, and the fix for that

Shipping the clip-matrix change as-is would have introduced a regression at map
edges, caught by testing the case rather than the happy path.

With `viewport_height = RenderHeight` and `viewport_y = 0`, the guest's own 192
rows land at the SURFACE's centre, `(RenderHeight-192)/2`. The 2D layers are
composited at `lb_band_y`, and those are not the same number whenever the band
is **clamped** at a map edge -- which is the whole point of the map-edge
confinement this port already has.

Measured at the map's north edge, `band_y = 0`, polygons matched by identity
and surface width, against the same route with the fix disabled:

    surface 216   +12      surface 360   +84
    surface 516  +162      surface 696  +252

exactly `(RenderHeight-192)/2` every time: units floating that far below the
ground they stand on.

The correction is a signed bias of `lb_band_y - (RenderHeight-192)/2`, applied
to `posY` with the RESULT clamped at zero. It cannot be folded into
`viewport_y`, which is unsigned -- clamping the origin instead leaves the bias
at zero in precisely the case that needs it, which a first attempt did.

After it, at the same north edge:

    topY deltas [-1, 0] at every surface;  heights 24 -> 24, 56 -> 56

Alignment preserved to within a pixel of rounding, sizes untouched. Where the
band is not clamped `band_y` equals the centre, the bias is zero, and the
coverage gain measured above is unaffected.

## Regression sweep after the vertical fix

The change touches `viewport_y` and the band values the HUD anchor, the entity
gather rect and the aspect box all read, so the doc's other fixes were
re-measured rather than assumed intact. One run, default settings:

| check | result |
|---|---|
| fog tiles seeded | frame **0** (`seeded 0C2 0C0 0C1 0BD 0BB 0BC`) |
| fog record | `LOCATED 64x64 bits`, agreement **132/132 (100.0%)** |
| band cells drawn clear where the record says hidden | **0** |
| HUD anchor | **5 of 5** at 786x426, 946x516 and 1266x696 |
| entity gather rect | adaptive: right `camx+1712`, left `camx-1488` |
| aspect box | **256x192 at (409,144)**, matching `band=409,144` |
| frame rate | 38.7 fps, unchanged |

The HUD cases include clamped bands (`946x516: shift=345,162 band=324,144`),
which is the configuration the vertical fix most plausibly disturbed, and it
anchors correctly there.
