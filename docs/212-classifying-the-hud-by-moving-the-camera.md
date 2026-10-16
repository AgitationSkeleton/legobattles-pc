# 212 — Classifying the HUD by moving the camera

Goal: HUD repositioning and scaling (with optional scaling in ImGui). This had
**not been started**, with the open note "HUD is 3D geometry at constant z,
screen-locked; needs ortho/perspective classification".
This is that classification, done by measurement.

## Why the existing anchor key cannot do it

`port/game.toml` already records the reason:

> Turning this off does not place the in-field HUD; nothing does yet. OBJ is
> disabled outright in gameplay (DISPCNT 00010F08, bit 12 clear), so this key
> could never have moved those buttons in the first place — it only ever had the
> BG bands to act on, which are the field.

`adaptive_hud_anchor` is upstream's rule for a 2D HUD. This game has no 2D HUD in
the field: no sprites, and the BG layers are terrain and fog. The in-field HUD is
**3D geometry**, mixed into the same polygon list as the units.

## The test: pan, and see what stays put

World geometry slides when the camera pans; screen-locked geometry does not.
That is the same test that settled whether the fog was map- or screen-anchored,
and it needs no assumption about z or about projection matrices.

`--polydump` records `gx_polygons` before and after three camera steps. One
frame of Knights Act 3 decomposes completely:

| submission | what | box | z |
|---|---|---|---|
| 0 | backdrop, 256 px wide | x 149..404, y 170..192 | 4259328 |
| 1 | **the button** | x 323..347, y 168..192 | 4267520 |
| 2..101 | 25x4 grid of 8-px tiles — the tutorial box | x 123..323, y 160..192 | 4267520 |
| 102..108 | seven quads | x 203..275, y 88..144 | 6987264..7380480 |

After the pan, submissions 102..108 have moved to **x 0..52** — they are world
geometry. Everything at z ~4.26M either vanished (the tutorial box closed) or
stayed exactly where it was.

**Exactly one polygon has an identical box in both samples**: `x 323..347,
y 168..192, z 4267520`. That is the objective button in the bottom-right corner.

## The classification

**HUD geometry sits at a small set of constant z values around 4.26M; world
geometry is at ~7M.** The two are well separated, and this agrees with docs/195,
which named 4267520 as a HUD depth.

Things that are NOT classifiers, checked and rejected:

* `attr` is **2031808 for every polygon in both samples**, HUD and world alike.
* `submission_index` is dense 0..108 with one polygon each — it orders the list
  but does not group it.
* `tex_param` / `tex_palette` vary per texture, not per role.

## The defect this exposes, stated in pixels

The button is drawn at surface **x 323..347**. The widened surface is 448 wide
and the native band occupies columns 96..351, so in native terms that is
x 227..251 — the bottom-right corner of the original 256-px screen, which is
right.

But the new right edge is **447**, not 351. A corner-anchored button should be at
**423..447**. It is 100 px short, sitting in the middle-right of a wide picture
instead of in its corner. That is the HUD repositioning defect, measured.

The same applies mirrored on the left, and the 256-px-wide backdrop at
submission 0 will need widening rather than moving, since a bar that spans the
screen should still span it.

## What has to happen next

The fix has to move HUD polygons horizontally by the extension width, outward
from the centre: left-anchored elements by -96, right-anchored by +96, and
full-width bars stretched rather than translated. The classifier above is what
selects them.

Two things to settle before writing it:

1. **Where to apply the offset.** Translating at raster time is a blit and would
   fight the depth buffer; translating the polygon's screen x before rasterising
   is the honest place. That needs the hook point in the widened 3D path.
2. **Which anchor each element has.** "Right of centre means right-anchored" is
   the obvious rule and is probably right for corner buttons, but the backdrop
   at submission 0 shows it is not universal.

Neither is guesswork about the ROM — both are about this port's own renderer.

---

## Implemented: `lb_hud.cpp`

Both halves (repositioning and scaling) are now in, in `port/runner/src/lb_hud.cpp`.

**Where the move happens.** On the RASTERISED line, not on vertices. The polygon
list is already in widened-surface coordinates and the line is flat RGBA by the
time the port composites it, so there is no depth buffer to fight; translating
vertices would mean patching melonDS's transform, which this build treats as
read-only.

**Which elements move.** `min_z` below 5,000,000 — halfway between the HUD's
4.27M and the world's ~6.4M floor — **and** narrower than 64 px **and** touching
an edge of the native 256 within 16 px. The width cap is load-bearing: the same
frame carries a 255-px-wide quad at HUD depth whose right edge is past the native
screen, and an edge test alone would have thrown it 96 px sideways. A bar wants
stretching, not moving, and that is left undone deliberately.

Verified by `LB_HUDLOG=1`, which reports exactly what it selected:

    [lb_hud] 1 corner element(s) moved, extra=96
    [lb_hud]   x 323..347 y 168..192 -> +96

One element. Not the 100 dialog tiles, not the backdrop.

**A/B, not a single picture.** `LB_HUDANCHOR=0` and `=1` produce
`screenshots/hud-ab.png`: with the anchor off the objective button sits at
surface 324..347, stranded in the middle-right; with it on it is at 419..442,
hard against the widened right edge, and the old position is cleanly vacated —
no duplicate.

**Scaling.** `LB_HUDSCALE` / the "HUD scale" slider, 1.0x to 3.0x, anchored so
the corner stays put and the element grows inward and upward
(`screenshots/hud-scale-ab.png`).

Enlarging a bottom-anchored box means the destination starts ABOVE the source, so
a single top-to-bottom pass would need rows it has not reached. Rather than reach
into a frame buffer that only exists on the snapshot path, each box's pixels are
captured as its lines go past and the scaled copy is drawn from the **previous**
frame's capture. The in-field HUD is screen-locked and changes rarely, so one
frame of latency is invisible — and at scale 1.0 the cache is bypassed entirely,
so the default path has no lag and no extra copy.

One bug worth recording: the early-out that skips lines with no HUD on them
originally tested only the SOURCE rows. A scaled element's top rows are above
those, so every scaled button had its top sheared off. The test now uses the
enlarged box.

Both controls are in the Display tab, badged WIRED.

---

## Two bugs a screenshot found, and a third it implied

The screenshot showed clicks outside the original aspect not fully working, and
HUD elements not aligned with the window edge: the yellow and blue bricks and
the deselect and quest-info icons float.

What made the probe possible at all: that HUD only appears with at least one
unit selected, ideally a builder or hero, which also shows the yellow (magic)
or red (build) HUD bricks.

**The HUD does not exist until something is selected.** Every earlier probe ran
with nothing selected, saw ONE quad -- the quest button -- and concluded the
in-field HUD was one element. `--select-hero` aims the hero into the touchable
band, taps him, and then the real HUD appears: **six** quads.

    [lb_hud] 6 corner element(s) moved, surface=448 visible=352 shift=48
       x 100..108 y 16..40 -> -48     the brick panel, four quads
       x 108..115 y 16..40 -> -48
       x 100..108 y 40..64 -> -48
       x 108..115 y 40..64 -> -48
       x 323..347 y 168..192 -> +48   quest "!"
       x 100..123 y 168..192 -> -48   deselect

### Bug 1: health bars were being treated as HUD

With a unit selected the list also contained quads at **z 4718080** -- which
docs/195 already named as the health-bar depth. The threshold was 5,000,000,
picked as halfway to the world's ~6.4M floor, and it swept them up. Health bars
are world-anchored: they sit above their unit and move with it, so dragging one
to the screen edge looks exactly like a rendering bug.

Most were caught by a new guard requiring the element's native box to be inside
the DS's own 0..256 -- they were logged as `SKIPPED native x -96..-86` -- but a
health bar over a unit standing NEAR the native edge would have passed both
tests. The threshold is now **4,300,000**, between the HUD's 4267520 and the
health bars' 4718080. The guard stays as the second line of defence.

### Bug 2: moving the pixels did not move the button

The bigger one, and it made things worse than before the feature existed. The
game's own hit test still believes the deselect icon is at native x 4..27 --
this port only changed where it is DRAWN. So a click where the button now
appears reached the guest as a touch at native -45, outside the panel, and was
discarded. **Anchoring the HUD had made the HUD unclickable.**

`lb_hud_unmap_touch()` undoes the move for input: a click inside an element's
NEW rectangle is reported to the guest at its ORIGINAL position, so the
cartridge's unmodified hit test finds it where it expects. It undoes the scale
as well, so an enlarged button is clickable across its whole enlarged face. No
game code is patched and nothing has to be known about how the game hit-tests.

### Still open: clicking the WORLD out there

This fixes HUD elements, which are at fixed screen positions the port already
tracks. It does **not** make the widened columns' terrain clickable -- selecting
or commanding a unit standing out there still needs the extended coordinate
docs/214 is about, and that does need a game patch.
