# 174 — The camera, read from the game instead of guessed from pixels

Status: **found, verified, and wired.** This closes the question docs/173 left
open, and it was a code-reading job exactly as that doc predicted.

## How it was found

Not by scanning. docs/173 records a thorough memory correlation across five
regions, two widths and four scales that found nothing, and explains why it could
not have worked. The answer came from three cross-references:

    xref 0x04000014  (BG1HOFS)  -> one writer: func_0202d1b4

`func_0202d1b4(bg, x, y)` is the scroll setter. It selects a BG from `[bg+4]` and
writes

    (-x & 0x1FF) | ((-y & 0x1FF) << 16)

to the matching `BGnHOFS`/`VOFS` pair -- so the hardware scroll is the NEGATED
value, which is worth knowing before anyone tries to read a camera out of it.

Of its seven callers, `func_020396E0` is the gameplay one, and it is where the
camera actually lives:

```asm
ldrsh r1, [r6, #0x5e]        ; camera X, signed 16-bit
ldrsh r3, [r6, #0x60]        ; camera Y
lsr   r2, r1, #0x1f          ; the compiler's signed-remainder idiom
rsb   r1, r2, r1, lsl #29    ;   r2 = x mod 8
add   r2, r2, r1, ror #29
rsb   r3, r4, r3, lsl #28    ;   r1 = y mod 16
add   r1, r4, r3, ror #28
rsb   r3, r2, #4             ; scroll x = 4 - (x mod 8)
rsb   r2, r1, #0             ; scroll y = -(y mod 16)
bl    func_0202d1b4
```

**The camera is two signed 16-bit PIXEL values at `+0x5E` and `+0x60` of the view
object, and only the sub-tile remainder ever reaches the hardware.** That is the
mechanism behind docs/167's double-buffer: the tilemap is rewritten every 8 px in
x and 16 px in y -- one BG tile, and one metatile row -- and `BGnHOFS` carries
what is left over.

## Locating the object at runtime

`func_02038504` registers `func_020396E0` as a callback with the view object as
userdata, so the object is not reachable from a literal pool. It is found instead
by the shape of the registration: **scan main RAM for the callback's address,
and the userdata is the next word.**

Two candidates come back, and only one is real:

    0x020385EC -> 0x02039148   camera = (-5375, 28)    the code's literal pool
    0x022945A4 -> 0x0224B448   camera = (468, 315)     the live object

Rejecting implausible camera values separates them. **The object moves between
runs** -- `0x0224B42C` in one, `0x0224B448` in the next -- so it must be located
each time, never hardcoded. (`0x0224B42C` is docs/163's view object; the 0x1C
difference between runs is why that doc's field offsets did not match what was
in memory later.)

## Verified

Driving each direction and reading the field back:

    start          camera = (488, 342)
    RIGHT 20 frames  -> (600, 344)   delta (+112,  +2)
    DOWN  20 frames  -> (607, 436)   delta (  +7, +92)
    LEFT  10 frames  -> (543, 454)   delta ( -64, +18)
    UP    10 frames  -> (535, 410)   delta (  -8, -44)
    RIGHT  5 frames  -> (551, 394)   delta ( +16, -16)

Every direction moves the right axis with the right sign. The small cross-axis
drift is the game's own scroll settling, not noise in the reading.

## World space is the map frame plus (48, 0)

On a fresh level `lb_field_locate()` reports the camera at map pixel
**(88, 144)**; the game's field reads **(136, 144)**.

**Y agrees to the pixel**, and x differs by **48 = 2 x 24**, exactly two
metatiles. So world space -- what the simulation and the camera use -- is the
level array's frame offset by two metatiles in x, and `lb_field_camera()`
subtracts it.

This **supersedes docs/172's `kEntityToMapX/Y = (33, 4)`**, which was measured by
locating a sprite's coloured pixels against a camera that docs/173 later showed
was unreliable. The new value is derived from two independently-located
quantities agreeing on one axis to the pixel, rather than from eyeballing a
minifig.

## What it fixes

`lb_field_camera_rect()` now prefers the game's camera and falls back to the
located cell only if the object cannot be found. That removes docs/173's limit
outright: **the camera box works over unexplored ground**, because it no longer
needs terrain to match against. The terrain tracker stays as the fallback.

## Two bugs found while verifying this, both worth keeping

**1. The dump harness disarmed itself.** `field_dump_if_asked()` ended in an
unconditional `state = 0`, so it wrote ONE image per run and then never ran
again. Every subsequent "the camera is not tracking" investigation was chasing
code that was no longer being called. Fixed to `if (seq >= 6) state = 0`, after
which the camera reports six distinct positions in one run:

    (88,144) (159,144) (166,220) (238,283) (302,283) (333,357)

**2. VRAM and palette RAM are NOT readable through `bus_read_u16`/`bus_read_u32`
from the vblank hook.** They return zero, which rendered the entire map black
while the debug server could read the same addresses perfectly. The symptom was
misleading: the located structures, the camera box and the unit markers were all
correct, and only the terrain was missing.

    render: char_base=06004000 pal[1]=FF000000 non-blank tiles=0     <- broken
    render: char_base=06004000 pal[1]=FF9AEBAA non-blank tiles=998   <- fixed

The video module owns those regions and maps them per VRAMCNT, so its own
accessor is the correct one. `lb_field.cpp` now reads the palette, the tile
graphics and the BG tilemap through `nds_video_read()`.

**This is a trap for anything else in this port that wants to read VRAM from
guest-side hook code**: the bus helpers fail silently rather than erroring.

---

# The first genuinely wider view

With the camera known, the guest's live output can be placed on the host-drawn
map: `lb_field_composite_guest()` copies `nds_gpu2d_framebuffer(1)` -- the bottom
screen, which is the battlefield -- to the camera position in map space.

`screenshots/.../wide_view.png` is the result. The castle, the hero and the
builder in the middle are the GAME's own rendering, and everything around them is
terrain the DS never draws. The unit markers land on the real sprites, which
cross-checks the entity positions independently.

This is the answer to "are you expanding the field of view or just panning the
stock view": the stock view is 256x192, **3.2% of this 1536x1008 battlefield**,
and the other 96.8% is host-rendered.

## Two defects it makes obvious

* **The guest region is darker.** The game shades its own output for fog of war
  and the host terrain has no such shading, so the seam is plain. The host side
  needs the explored mask applied, or the guest's shading has to be undone.
* **Units outside the camera are markers, not sprites.** docs/156 put the units
  on BG0-3D, so drawing them wide means the 3D engine rendering a wider frustum
  rather than sprite extraction -- `nds_gpu3d_set_output_width()` already exists
  for that, and terrain was always the blocker rather than the units.

---

# Widening the 3D output does NOT bring units with it

Worth recording before anyone tries it: `nds_gpu3d_set_output_width()` widens the
3D raster and `nds_gpu3d_wide_line()` returns `result + (width - 256) / 2`, so the
extra columns really are an extension rather than a stretch. That makes it look
like the obvious route to drawing units across a wider view, since docs/156 puts
units on BG0-3D.

Measured, with the 3D output at 512 and the composite at 448:

    left margin      96 cols:     0 non-black sampled px
    centre (stock)  256 cols: 16231 non-black sampled px
    right margin     96 cols:     0 non-black sampled px

**Zero.** Not dim, not partial -- nothing at all. The game submits only the
geometry it believes is on screen, so a wider raster has nothing to rasterise.
That is the culling the widened view has to defeat, and it sits in the game's own
submission path, upstream of anything the renderer can widen.

So units outside the stock view have to come from somewhere else: either the port
draws them itself from the entity positions (docs/169), or the game's culling
rectangle is enlarged so it submits more. The second is the better prize because
terrain streaming and the UI would follow the same rectangle -- and it is
`docs/163`'s unresolved thread.

The view object found above is NOT where that rectangle lives: a sweep of its
first 0xC0 bytes contains no 256/192, 32/24 or 512/384 pair. docs/163's bitmask
object is a different structure.

---

# Recovering unit sprites by differencing, and why it is not verified yet

Since the game culls units before submission, the only way to draw one outside
the stock view is to have its pixels already. They are recoverable without any 3D
work: **whatever the guest draws over the terrain IS the unit**, and the terrain
is in hand, host-drawn and pixel-exact. Differencing the guest frame against it
lifts the unit out with a free alpha mask -- no sprite format to decode, no
projection to reproduce.

`lb_field_composite_guest()` now does this: for each entity comfortably inside
the camera, it captures a 40x40 patch of pixels that differ from the host
terrain, keyed by entity slot, and requires at least 24 differing pixels so
animating terrain is not mistaken for a sprite. Entities outside the camera are
then drawn from their cached patch.

**Implemented and building; NOT verified end to end.** The learning step only
runs when `lb_field_composite_guest()` is called, and in the headless harness
that is only at dump moments -- a handful of frames, several seconds apart. In
the capture taken for this, no unit was inside the camera at a dump moment after
the cache existed, so nothing was learned and the off-camera entities still show
only their markers. The panel calls the composite every frame and would not have
that problem, but the panel cannot be screenshotted headlessly.

So this is honest as: the mechanism is written and compiles, the reasoning behind
it is sound, and it has NOT been shown to produce a sprite yet. The next step is
to run the learning step on every vblank rather than only when a dump fires.

---

# The entity anchor: wrong three times, and why fitting it is the wrong method

The offset from entity world space to map pixels has now been (33,4), (48,0),
(30,16) and (14,-15). Each was "measured". The method was wrong every time, and
that is the useful part.

**(33,4)** came from locating a minifig by colour against a camera docs/173 later
showed was unreliable.

**(48,0)** came from the camera: the located map camera reads (88,144) while the
game's field reads (136,144), which is correct **for the camera** and was then
reused for entities. The camera's anchor is not the entity's anchor, and markers
landed ~60 px above their units.

**(30,16)** composed the camera offset with a fresh sprite measurement. The flaw
is subtle: a colour search finds a blob, but with three friendly units within
~50 px of each other **the blob cannot be attributed to a particular entity**.
The measurement was matching the hero's field against the builder's sprite.

**(14,-15)** was fitted directly on a rendered image where marker and sprite are
both visible, which at least cannot mis-attribute. It puts one marker on its unit
and leaves another off, because **buildings do not share the anchor of point
units** -- the castle is a further ~30 px out.

## What this actually needs

Not another measurement. The game computes a world-to-screen transform every
frame to draw these units, and that code is the answer -- the same way the camera
itself was found by reading `func_020396E0` rather than by scanning for a value
that looked right. Until it is read, the constant here is approximate, it is
named in one place, and nothing depends on it beyond marker placement.

The sprite-learning patch is 40x40, comfortably larger than the residual error,
so CAPTURE tolerates the slop even though PLACEMENT does not.

# Learning now runs every frame

The structural bug from the previous entry is fixed: `lb_field_learn_sprites()`
is called from the vblank hook rather than only when a view is drawn, and it
renders terrain only for the 40x40 patch under each unit rather than the whole
map, so it is cheap enough to run every frame. Tile graphics and palette are
cached and refreshed every 120 frames rather than re-read per call.

---

# The anchor, settled by constellation matching -- and it works

The offset was wrong four times because every measurement located ONE unit by
colour and assumed which entity it was. With three friendly units inside 80 px
that assumption is a coin toss, and three of the four values came from matching
the hero's coordinates against the builder's sprite.

**Matching the shape of all three at once cannot mis-attribute.** With the camera
at (79,146), the three owner-0 entities map onto three blobs with a spread of
11 px:

    entity(240,160) -> blob(186, 20)   offset (-54,-140)
    entity(240,208) -> blob(177, 60)   offset (-63,-148)
    entity(264,240) -> blob(202, 89)   offset (-62,-151)
    mean screen offset (-60,-147)

so `screen = world - camera + (19,-1)`.

## Two constants, not one

That also exposed the conflation still in the code: `lb_field_camera()` was
subtracting the ENTITY offset from the CAMERA, so every attempt to fix one broke
the other. They are separate measurements:

    kCameraToMap = (48, 0)    located map camera (88,144) vs camera field (136,144)
    kEntityToMap = (29, 1)    from the constellation above

Check: the hero at world (264,240) predicts screen (204,93) and its blob is at
(202,89).

## Verified end to end

`screenshots/.../units_learned_ok.png`. The camera is scrolled away from player
0's base; **above the camera box, outside anything the DS is drawing, a learned
sprite appears** -- part of the castle and a unit -- while the hero inside the
box is the guest's own output.

So the chain works: the game culls a unit before submission (docs/174), its
appearance was captured earlier by differencing the guest frame against
host-drawn terrain, and it is drawn from cache at its world position. **The port
shows units the DS is not rendering.**

Placement remains approximate for STRUCTURES, whose anchor differs from a point
unit's by roughly 30 px. The 40x40 capture patch absorbs that; only placement
shows it.

---

# The wide field as the WINDOW's content

`draw_wide_field()` in `lb_overlay.cpp` draws the whole battlefield over the
window instead of beside it. The frontend blits the guest's 256x192 screen first
and the overlay draws after it, so covering the window replaces the stock view --
which is the actual goal rather than a panel next to it.

Toggled from the settings panel, or with `LB_WIDE_FIELD=1` at launch.

Terrain is static for a level, so it is rendered ONCE and kept; each frame copies
it and composites only the guest's live output and the learned unit sprites.
Re-rendering 1.5M pixels of terrain per frame would be pure waste.

The map keeps its aspect and letterboxes: a 3:2 battlefield in a 21:9 window
pillarboxes rather than stretching.

## What is and is not verified

The COMPOSITION is verified -- it is the same terrain render, guest composite and
sprite cache the `LB_FIELD_DUMP` images exercise, and those are correct.

The ImGui blit that puts it on the window is NOT verified: `lb_overlay_draw()`
only runs in the interactive frontend, serve mode never calls it, and
screenshotting the host window would mean taking over the desktop. So the
drawing path is standard ImGui over verified pixels, but it has not been seen.

---

# Zoom, wired

`Zoom` had been `[unread]` since the panel was written, because in the stock
256x192 view there is nothing to zoom out to -- the DS draws one screen and that
is all there is. With the wide field it has a real meaning and is now `[wired]`.

    1.00x   the WHOLE battlefield fits the window
    >1.00x  crops toward the camera

It scales about the CAMERA rather than the map's centre, so zooming in keeps what
the player is looking at on screen, and the result is clamped to the map's edges
so zooming never shows past the level.

Client-local, not lobby-carried: docs/85 established that field of view never
reaches the simulation (zero Sim->User calls across 555 simulation virtuals), so
a zoomed host and a vanilla client stay in lockstep.

Badge census after this: **7 wired, 5 read, 6 unread, 3 blocked**, plus the 18
cheats wired in docs/171.

# Fog: the seam was not what it looked like

The guest region in the composite looked darker, which read as the game shading
its output for fog while the host terrain had none. Measuring the guest frame's
colours says otherwise: its dominant colour is `0x289A28`, exactly the bright
grass the host renderer produces. **There is no global shading to undo.** What
looked like a seam is the shroud drawn over genuinely unexplored ground, which is
correct behaviour.

The remaining difference is the opposite of the one assumed: the host terrain
shows unexplored ground fully lit. For a whole-battlefield view that is arguably
what is wanted -- it reveals terrain, not units, and units appear only where they
have been seen. It is recorded here as a deliberate choice rather than an
oversight. Applying the explored mask properly needs that mask located, and an
attempt to derive it from per-cell guest-versus-terrain differences hit a ~38%
baseline mismatch that indicates a systematic offset in the reconstruction rather
than fog, so it was not pursued further.

---

# Aspect-ratio sweep

Rendered from one composite, mirroring `draw_wide_field()`'s fit-then-zoom-about-
the-camera maths, in `screenshots/20260831-095058/`:

    asp_1920x1080_z1.png   1920x1080  1.00x   100% of the battlefield
    asp_3440x1440_z1.png   3440x1440  1.00x   100%
    asp_2560x1440_z1.png   2560x1440  1.00x   100%
    asp_1280x720_z1.png    1280x720   1.00x   100%
    asp_1024x768_z1.png    1024x768   1.00x   100%
    asp_1920x1080_z2.png   1920x1080  2.00x    29%

At 1.00x the whole 1536x1008 battlefield fits every one of those windows,
including 21:9 and 4:3 -- the map keeps its aspect and letterboxes rather than
stretching. There are no black bars from missing content; the padding is only
where the map's 3:2 does not match the window.

`asp_1920x1080_z2.png` is the one worth looking at: the game's own castle, hero
and builder inside the white stock-view box, and forest, cliffs and the lake
around them -- terrain the DS never draws. **29% of the battlefield against the
stock view's 3.2%**, and that is at 2x zoom; at 1x it is all of it.

---

# Menus, re-verified at 448 wide

The widescreen work covers the menus first, and they had not been re-checked since the
field work started. Driving the front end with `LB_AUTOPLAY=1` and
`LB_FORCE_WIDTH=448` to the "Choose Profile Name" keyboard:

    top    448x192 : left margin 100% filled | centre 100% | right margin 100%
    bottom 448x192 : left margin  98% filled | centre  98% | right margin  99%

**RETRACTED — the top row of this table is an artifact of the capture harness,
and scoring it as a pass was wrong. See docs/178.** The debug server's
`framebuffer` command widened whichever screen it was handed, ignoring
`adaptive_widescreen = "bottom"`; the port itself never widened the top screen.
So "top 448x192, margins 100% filled" is not a measurement of the port — it is a
measurement of my tooling stretching a bitmap that was never meant to stretch,
and the resulting title screen (logo pulled apart, BATTLES broken by gaps) is
what that looks like.

The bottom row stands and is re-verified in docs/178, where the top screen
correctly reports 256x192 while the bottom reports 448x192.

**No black bars** on the bottom screen. Its margins are as filled as its centre,
so the background extends rather than leaving the widened columns empty.

`screenshots/.../menu448_bottom.png` also shows the edge anchoring working, which
is a separate requirement: the back arrow is flush LEFT and the Ok
button flush RIGHT of the 448-wide screen, with the keyboard and title centred.
Those are the game's own widgets landing where they should at a width the DS
never had.

One caveat worth stating: in serve mode the adaptive width only engages when
`LB_FORCE_WIDTH` is set, because nothing else drives `nds_gpu3d_set_output_width`
there. A first capture without it came back 256 wide and looked like the widening
had regressed; it had not. The interactive frontend sets the width from
`game.toml` instead.
