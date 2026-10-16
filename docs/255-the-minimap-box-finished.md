# 255 — The minimap box: white, and the cartridge's own one erased

The defect: the yellow top-screen minimap outline that reflects the expanded
screen space did not hide the original white one, and it drew even while a
unit was selected, which hides the minimap. The expanded-space outline should
also be white, to match the vanilla one.

Three separate requirements. Two are measured below; the third is wired and is
**not** yet observed firing, which is said plainly rather than counted as done.

## 1. White, not yellow — done

`IM_COL32(255, 240, 64, 255)` → `IM_COL32(255, 255, 255, 255)`. The yellow
dated from when this box and the debug aspect outline both had to be told apart
from the game's own; it is the player's view indicator now, not a debug aid.

## 2. The cartridge's own box — erased, measured

The game draws a 16x18 indicator for the 256x192 it can show. docs/180 fitted
the map→minimap transform against that indicator and predicted its size from
the fit (`256 * ax = 15.3`, `192 * ay = 17.5` against 16.0 x 17.7 measured), and
lb_field's shroud sampler recorded its colour as `(251,251,251)`, 64 px.

Erasing it needed one thing getting right: **`g_top_pixels` is a pointer to the
frontend's buffer, not a copy**, and `lb_field`'s shroud readback samples that
same top screen every frame — it classifies `(251,251,251)` as explored ground.
Scrubbing in place would have rewritten the input of an unrelated measurement.
So `draw_top_screen()` copies to `g_top_scrub`, erases there, and uploads that;
the guest's framebuffer is untouched.

The erase is bounded by the predicted rect (`lb_field_map_to_minimap(camera)`
to `…(camera + 256,192)`, plus 2 px of margin) because `(251,251,251)` is NOT
unique to the box — the same white is used by the resource counters along the
bottom of the top screen. A whole-screen colour sweep would have eaten those.
Inside the rect, box pixels are replaced with the mean of their non-box 3x3
neighbours; the outline is one pixel wide, so that window is mostly minimap.

**Verification** — connected components of near-white pixels, same scene:

```
RAW guest top screen (unscrubbed)
     64 px  bbox 16x18 at (158,53)     <- the cartridge's camera box
     16 px  bbox  4x6  at (194,177)    <- resource counter digits
     16 px  bbox  4x6  at (178,177)
     ...

PANEL (scrubbed copy)
    360 px  bbox 57x48 at (140,44)     <- the corrected box, white
    140 px  bbox 14x18 at (180,223)    <- counter digits, untouched
     96 px  bbox 16x12 at (186,209)
     ...
```

64 px at exactly 16x18 in the raw screen, matching docs/180's measurement to the
pixel; **no 16x18 component in the scrubbed panel**, and the counter digits
survive. The corrected box is 57x48 — it covers what the adaptive surface
actually shows, with the cartridge's old 16x18 rect sitting inside it at
(158,53).

## The harness could select all along — it could not SEE that it had

Before any of part 3 could be tested, this had to be fixed, and it is the more
valuable finding.

`--click-sweep` reads the player's base position out of the port's `[lb_base]`
line, the field rect out of `[lb_geom]`, and the selection state out of
`[lb_hud] ... hudquads=N`. All three are env-gated — `LB_ENTLOG`, `LB_VERBOSE`,
`LB_HUDLOG` — and **nothing set them**. So every step printed
`has no geom/base yet` and the run ended `SELECTED at sx none`.

That is indistinguishable from "clicking does not select", and it was read that
way: four runs across `--select-hero`, `--select`, `--drag-select` and a plain
route all reported `hudquads=0`, and the conclusion recorded in the previous
revision of this document was that the harness cannot select a unit. **It can.**
With the three variables set, the same sweep on the same route reports:

```
click-sweep: calibrated offset (16, 16) in band
click-sweep: sx=123  sy=15  window=(808,296)  hud 0 -> 6  SELECTED
```

and `--build-flow --build-flow-unit builder` reports
`builder at (123, 63)` / `after tapping the builder, hudquads=6`.

`lb_exp.py` now arms all three whenever a selection probe is requested, and the
sweep captures `selected-sx<N>.png` while the selection is live — the only
moment a run has such a picture, and both the box gate and the brick-menu
work need one.

## 3. Not while a unit is selected — STILL OPEN, with both wrong rules recorded

Two rules were tried and both are wrong. Recorded so neither is tried again.

**Rule A — "hide it whenever something is selected"**
(`lb_hud_selection_active()`, the on-screen HUD-quad count). Wrong: a capture
with the player's BASE selected shows 6 HUD quads *with the minimap still on
the top screen*, so this hides the box in a state where it is valid.

**Rule B — "hide it unless the cartridge's own camera box is present"**, on the
reasoning that the game draws that box only as part of the minimap, so it
doubles as proof the minimap is up. Measured against both states:

```
                                   white components in the panel
BASE selected     250 px  48x40 at (138,41)   <- corrected box drawn
BUILDER selected  250 px  48x40 at (138,41)   <- corrected box drawn
```

Identical. Rule B changes nothing, because the premise is false.

**What the captures actually show.** The minimap lives in the RIGHT sub-panel
and stays there in every state observed — nothing selected, base selected, and
builder selected. Selecting the builder fills the LEFT sub-panel with the unit
portrait and a "Builder" title and a 150/150 bar; it does not move, shrink or
replace the minimap. So the premise both rules were built on — that selecting
something takes the minimap away — is not what this build does.

**The report is ambiguous and should not be guessed at.** The player wrote
"draws even while a unit is selected which hides the minimap". That parses two
ways: *the selection* hides the minimap (not observed here), or *the box* hides
the minimap — and the second is plainly true in these captures, where the
corrected box is 48x40 around a green explored blob perhaps a third that size,
covering most of the panel. Those want opposite fixes: suppress on a state, or
make the box less intrusive (thinner, unfilled, drawn only at its corners).

Neither is built yet; the choice between them is open. The two captures that
show the difference are
`screenshots/exp-selgate/selected-sx123.png` and
`screenshots/exp-bldsel/route/bf-1-selected.png`.

### The superseded note on rule A, kept because it was stated as fact

`lb_hud_selection_active()` (new, `lb_hud.cpp`) returns `g_hud_seen > 0`. That
is this port's documented selection signal: the in-field HUD does not exist
until something is selected, so a nonzero on-screen HUD-quad count means
something is. The box now skips drawing when it is true.

The signal is deliberately one-way and the caller treats it as one. Nonzero
means selected; **zero is ambiguous** — it is also what a frame reports when
every quad was rejected by the z test or the edge test. Used to SUPPRESS the
box, a false negative just draws it as it does today, rather than hiding the
minimap at the wrong moment.

**It has not been seen firing.** Four harness runs — `--select-hero`,
`--select 205 169`, `--drag-select`, and a plain route — reported
`hudquads=0` on every sampled frame, with `aim: no hero reported yet` and
`drag-select: geom=None`. So the harness never actually selected a unit, and
the gate was never exercised. That is a gap in the *test*, not evidence about
the gate, and the two must not be conflated: nothing here shows the suppression
works, and nothing here shows it does not.

Closing it needs a route that demonstrably selects something —
`hudquads > 0` in the log is the acceptance signal, and no capture should be
read as confirming this fix until that number is nonzero in the same run.
