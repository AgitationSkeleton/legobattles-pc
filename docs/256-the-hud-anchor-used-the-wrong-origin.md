# 256 — The HUD anchor converted surface x with the wrong origin

Defect #6: the UI animations (the blue/red/yellow brick menus for
units/buildings) can sometimes follow the original aspect's positioning, and
the touchscreen UI during gameplay (the red/blue/yellow bricks, the deselect
button and the combat-occurring button) does not align exactly with the screen
edges but floats slightly away from them.

## The contradiction was inside one comment

`lb_hud_scan()` converts a HUD quad's surface x back to the guest's own
256-wide coordinates so it can ask "which edge is this element against". The
comment above that conversion said:

> `extra` converts surface x back to the guest's own 256-wide coordinates, so
> it is **always half the surface's overhang**.

and the line under it said:

```c
const int extra = band;
```

The band is `lb_band_left(output_width)`, which is **clamped at a map edge** and
is then not half the overhang at all. The comment stated the requirement
correctly; the code assigned something else.

## Which origin is right, decided against two known positions

This file already knew two native coordinates independently of anything here:
the deselect icon, which the game's own hit test places at **native x 4..27**,
and which the harness reports at **y 167..191**. One logged quad, builder
selected, surface 946x516, `band=132,144`, half-overhang `345,162`:

```
[lb_hud]   x 348..372 y 311..335

    X  via band          216..240      via half-overhang    3..27   <- 4..27
    Y  via band          167..191      via half-overhang  149..173   <- 167..191
```

**Each axis is right under a different rule.** X wants half the overhang, Y
wants the band — which is exactly the asymmetry the viewport already has: the
vertical path carries `lb_ybias = lb_band_y - lb_ycentre`, the horizontal one
does not. So `extra` becomes `(output_width - 256) / 2` and `extra_y` stays
`band_y`, each with the measurement written next to it.

It also restores the identity the movement formulas were written against.
`dx_left` and `dx_right` reduce to `-shift` and `+shift` exactly when
`extra == (W - 256) / 2`, which their own comment says.

## What it was doing wrong

With `band`, the left-edge brick column read as native x 216 — the **middle
third** — so the classifier either left it alone or, at other camera positions,
threw it RIGHT by the inset rule. That is both halves of the report: an element
"following the original aspect's positioning" and an element that "floats"
instead of meeting the edge. The "sometimes" is the band being clamped only near
a map edge.

## Verified

Same route, same build-flow selection of the builder, 1600x900 window
(surface 786x426), measuring the blue brick by colour:

```
BEFORE (extra=band)       blue px=1220  bbox x 548..579  y 66..113
AFTER  (extra=overhang)   blue px=1220  bbox x  18..49   y 66..113
```

Identical pixel count, so the element did not resize — it moved, by 530 window
pixels, which is the logged `dx = -265` at magnification 2. It now sits hard
against the window's left edge instead of 34% of the way across. The per-quad
log confirms the deselect icon lands at surface x 3..28, y 401..425 of 426 —
the bottom-left corner.

```
BEFORE   x 268..276 y 133..157 -> +0,-117        <- stranded mid-screen
AFTER    x 268..276 y 133..157 -> -265,-117      <- exactly -shift
```

## Vanilla (Enhanced off) is broken, and it is now a deploy gate

A deploy requirement: vanilla rendering, with the enhancements set to off,
must still work (and run well) before a build with new features and fixes is
deployed.

Checked by flipping `"enhanced": false` in the settings file beside the
executable (there is no env override for it). Vanilla really is engaged — the
run presents `presented-bot-0-256x192.png` against enhanced's
`presented-bot-0-1706x696.png` — and it runs faster, 47.6 and 48.1 fps against
~35.

**It does not render correctly.** `screenshots/exp-vanilla2/route/linger-010.png`
(taken with `LB_NOTOP=1`, so the ImGui panel is not involved) shows the two
screens small and misplaced in a 1920x1080 window, separated by a black band,
with a block of **RGB noise** where the lower screen's upper rows should be.

**There turned out to be TWO separate faults here, and the first one hid the
second.** Both are recorded because the intermediate conclusion — "vanilla is
fine, it was only the capture" — was stated and is wrong.

### Fault 1: the capture harness was photographing off-screen window

Every 1920x1080 capture confined its content to exactly `x 1..1475, y 1..764`.
So did a 1600x900 one. **Identical extents at different requested sizes**, which
no rendering difference can produce. The port centres itself on the display it
opens on, and `lb_capture.ps1` resizes about the window's *current* top-left
(the fix that stopped the sweep dragging it onto another display) — measured at
(444, 315) relative to a 1920x1080 display:

```
1920 - 444 = 1476      1080 - 315 = 765
```

The window hung off the right and bottom, and `PrintWindow` fills only the
on-screen part of the bitmap, leaving the rest as uninitialised memory. That
memory is what the "RGB noise" in the first reading was.

Fixed: the resize now clamps the position so the window fits the display it is
on, and the capture prints `WARNING: ... this capture is NOT evidence` whenever
the window rect still extends past its display. A silent bad capture is worse
than none, because it reads as data.

After the fix the same run captures `x 1..1914, y 1..1074` — the whole window.

### Fault 2: vanilla compositing really is broken

With a complete capture, **the noise is still there**, so it was never only the
harness. `screenshots/exp-vanilla3/route/linger-010.png`:

- a 256-wide texture drawn at 2x (measured 514 px across) containing, top to
  bottom, the HUD strip, a green field strip, a black band, and then a block of
  RGB noise — all inside what should be one 192-row screen;
- a separate green draw about 1010 px wide, a different scale entirely,
  overlapping below it.

Two screens at two scales with garbage between them. Meanwhile the port's OWN
dumps from that same run are clean: `presented-bot-0-256x192.png` is correct
terrain and `presented-top-0-256x192.png` is a correct HUD with the cartridge's
16x18 camera box present (the scrub correctly does not run in vanilla). So the
guest rendering is right and the **window compositing** is wrong.

One oddity worth carrying into the fix: the vanilla run presented 8 frames at
256x192 and **one at 512x192**, and the banner reports `adaptive_width=512`. So
turning Enhanced off does not put every frame back to the native surface.

**Still not attributed to this round of changes**, and that is now a smaller question
than it was: the three changes remain implausible reaches (the HUD anchor
early-returns at `extra <= 0 && extra_y <= 0`, which is every 256-wide frame;
the scrub only writes the ImGui panel texture, and `LB_NOTOP=1` switched that
off; the fog gate only removed logging). It still has to be measured against a
build from before these changes before the fix is designed.

One minor defect found alongside: the startup banner prints `enhanced=1` even
in a vanilla run, because it is emitted before `[lb_settings] loaded ...`. It
is not a reliable readout of the mode and a run must be identified by its
presented surface size instead.

## Closing the rest of item #6

The remaining half was "the combat-occurring button" and anything the anchor
still declines. Measured with the builder selected, reading ONE `[lb_hud]`
block so the band and the quads cannot come from different moments:

```
6 corner element(s) moved, surface=946x516 shift=345,162 band=345,144
  hudquads=6   rejected: z=4 (lowest 4718080, threshold 4300000) offscreen=0

  x 348..357 y 160..184 -> -345,-144      the brick column
  x 357..365 y 160..184 -> -345,-144
  x 348..357 y 184..208 -> -345,-144
  x 348..357 y 184..208 -> -345,-144
  x 357..365 y 184..208 -> -345,-144
  x 348..372 y 311..335 -> -345,+180      the deselect icon
```

**Six of six moved. `offscreen=0`. No `SKIPPED` line anywhere in the run.** The
four `z` rejections are world geometry under the depth threshold, which is what
that test is for.

Both anchors check out against positions known independently:

```
deselect  native x = 348-345 =   3..27    (the game's own hit test: 4..27)
          native y = 311-144 = 167..191   (the harness measured: 167..191)
          dy_bottom = (516+516)/2 - 192 - 144 = 180, and dy = +180
```

so it lands at surface y 491..515 of 516 — the bottom edge — and at x 3, the
left edge. The bricks go to the top-left corner. Confirmed in the capture:
the blue brick's leftmost pixel is x 18 and the deselect icon's cluster in the
lower quarter starts at x 28, y 720 of 900.

### One thing NOT verified, and why

The **combat-occurring button** never appeared: no combat happens on this
route, so the state that draws it was never entered. It is not a separate
mechanism — it is a corner element like the deselect icon, and the anchor
declined **nothing** in this run — so there is no element-specific failure left
to fix. But "it will be anchored too" is an inference from the absence of
declines, not an observation of that button, and it is recorded as such.

### A harness bug fixed on the way

`--build-flow` died with `UnboundLocalError: cannot access local variable
'tap'`. The closed-loop `--pan-return` code added earlier in this work bound a
local named `tap` for a press duration; `tap` is also a module-level FUNCTION
used throughout `main()`, and Python decides local-vs-global per function
rather than per line, so that one assignment broke every earlier call in the
same function. Renamed to `press`.
