# The adaptive widescreen path letterboxes the game field

> **Corrected by [docs/110](110-setlayerscroll-and-the-508.md).** Every argument
> below that reasons from the BG scroll registers being "pinned at 508" is
> unfounded: 508 is a hardcoded constant argument to `SetLayerScroll`, written
> every frame by `0203891C`, so the registers never carried any information
> about the camera. The conclusion that the field scrolls by rewriting the
> tilemap survives and is in fact strengthened.

**Status: the expanded field of view is NOT implemented for the screen
LEGO Battles needs it on.** Also delivers `tools/lb_warp.py`, a reproducible
debug warp from reset into a Free Play match.

## The debug warp

Nothing past the title screen could be reached with buttons — the front end is
stylus-driven (`docs/99`) — so none of the in-game work was testable. The debug
server has `touch`, `run_cycles`, `read_mem`, `write_mem` and `framebuffer`,
which is enough to drive and verify the whole front end without a window.

`tools/lb_warp.py` walks the route below, capturing a PNG at each step, and was
re-run from a cold reset to confirm it reproduces:

```
logo -> title -> Choose Profile -> name keyboard (Q,W,E) -> Ok
     -> Main Menu (Single Player) -> Free Play (select, then confirm)
     -> map select (Continue) -> rules (Continue)
     -> army customise (Continue) -> opponent setup (Start) -> in game
```

Reaches the field at ~3.31 billion ARM9 cycles. Every coordinate was read off a
captured frame; none is guessed.

Two things worth recording from building it:

* **Allocated is not active.** Scanning the heap for `User::` vtable pointers
  finds every screen object ever constructed, and the set does not change as you
  navigate. It is not a way to tell which screen is showing. The framebuffer is.
* **Free Play needs two taps**: the first moves the selection onto the tile, the
  second enters. The main menu enters on one. Assuming one rule for both stalls
  the route silently.

## What adaptive widescreen actually does to the bottom screen

`nds_gpu2d_adaptive_framebuffer` in `runner/src/gpu2d.cpp`:

```c
const int output_width = nds_gpu3d_output_width();
if (output_width <= 256) { if (width) *width = 256; return native; }

AdaptiveFrame& adaptive = g_adaptive_frame[screen & 1];
const int extra = (output_width - 256) / 2;
if (engine != 0) {
    for (int y = 0; y < 192; ++y)
        std::copy_n(native + y * 256, 256,
                    adaptive.data() + y * output_width + extra);
    return adaptive.data();
}
/* ... ~300 lines of real BG+OBJ composition, engine 0 only ... */
```

**For any engine other than engine A the function centres the native 256-wide
image and returns.** The buffer was just filled with `0xFF000000`, so the
`extra` columns on each side stay black. That is a letterbox, not a wider field
of view.

The compositor that does the real work is hard-wired to engine 0 throughout —
`nds_vram_renderer_palette(0)`, `nds_vram_renderer_oam(0)`,
`nds_vram_renderer_view(0)`, `g_unit[0]`, `render_obj_line(0, ...)` — and its
own comment says it targets "main-engine 3D plus OBJ", a top-screen 3D scene.
LEGO Battles' field is a 2D tilemap on the bottom screen.

## Why that lands on this game specifically

Observed in a running match through the debug server: **engine A draws the top
screen** (minimap, gold, population) and **engine B draws the bottom screen**
(the game field). `port/game.toml` sets `adaptive_widescreen = "bottom"`,
because for an RTS the field is the screen that must grow (`docs/84`).

So the one screen that needs widening is the one screen the adaptive path
refuses to widen. Enabling it today buys black bars.

*Stated precisely:* the engine/screen mapping is **observed**; the letterbox is
**read from the source above**, not yet photographed, because `output_width` is
256 unless the SDL frontend is running and this analysis was done headless.

## What this means for "constrain the view to the world corners"

The concern that a wider view would scroll past the map edge into void is
**real but downstream**. The guest clamps scrolling for a 256-wide view, so once
the field is genuinely widened the extra columns will run past the world at the
edges. That clamp has not been located yet; ruled out so far:

* `func_020DDE30` — uses 256 and 192 but is selection cycling, not the camera.
* `func_0202D1B4` writes `BGnHOFS/VOFS = (-x & 0x1FF) | ((-y & 0x1FF) << 16)`
  and is the generic per-layer scroll applier, reached from the tile-layer
  manager around `0x02038000`-`0x0203A000`. The clamp is upstream of it.

Maps are 96x96 (65 of them), 64x64 (56) and 48x48 (1) tiles (`docs/data/maps.tsv`),
so the clamp must be map-relative, not a constant.

**Order of work:** widen engine B first, then find and adjust the clamp. Looking
for the clamp before there is a wider view to clamp would be testing something
that cannot yet be observed.

---

# The Enhanced window had one bug wearing two faces

Reported from the running port: the top screen was missing and the touch screen
could not be clicked. Both came from **mixing two coordinate spaces**.

`SDL_RenderSetLogicalSize` scales *mouse events* as well as drawing. Enhanced set
it to `448x192` in a `896x768` window, so:

* SDL delivered mouse events in logical coordinates (`0..448 x 0..192`) — the
  shipped selftest says so in its own comment, and the guest mapping relied on it.
* **ImGui got those same coordinates while believing its display was `896x768`**,
  which is what it was told at `NewFrame`. So ImGui thought the cursor sat near
  the top-left whatever the player did, and `lb_overlay_wants_mouse()` reported a
  panel hit for clicks aimed at the game — killing the stylus.
* ImGui also *drew* through that logical size, scaling every panel by 2 and
  offsetting it by the letterbox, which put the "Top Screen" panel — anchored
  bottom-right — completely outside the window.

One cause, two symptoms that looked unrelated.

**Fix: Enhanced keeps a single coordinate space, window pixels.** It sets no
logical size, computes the field's destination rect itself (fit, never stretch),
publishes it with `lb_overlay_set_field_rect`, and maps clicks back through that
rect. Vanilla is untouched and still uses the logical path.

A third place needed the same treatment. The frontend gates
`SDL_MOUSEBUTTONDOWN` on a 256-column band at `bottom_content_left` plus, in the
stacked layout, `y >= kScreenHeight`. Both are logical-space tests: in window
pixels they accept a narrow strip and silently reject the rest of a field the
player can see. `lb_overlay_mouse_in_touch_area` now answers that question, and
reproduces the original test exactly when Enhanced is off.

**Verified, not asserted:** with the fix built, injecting a *mouse* button event
through `frontend_input` at the title screen advanced the game to Choose Profile.
That exercises the real path end to end — SDL event, gate, mapping, guest touch —
rather than the debug `touch` command, which bypasses all three.

---

# What actually draws the game field, measured in a running match

The earlier sections reasoned about "engine B" from `adaptive_widescreen =
"bottom"`. **That was wrong, and the ROM says so.** Read out of a live Free Play
match through the debug server:

```
POWCNT1 = 0000020F   bit 15 (display swap) = 0  -> engine A is on the BOTTOM screen
engine A  DISPCNT=00012F08  bgmode=0  BG=[1,1,1,1]  OBJ=0  WIN=0x2000  3D=1
engine B  DISPCNT=00011F00  bgmode=0  BG=[1,1,1,1]  OBJ=1  WIN=0      3D=0
```

* **Engine A draws the field, on the bottom screen.** It uses **BG0 as the 3D
  layer**, has **OBJ disabled**, and has **WIN0 enabled**.
* **Engine B draws the top HUD**, plain 2D with objects.

The debug `framebuffer` command's `engine` parameter is misleading: it selects a
**screen**, and `nds_gpu2d_adaptive_framebuffer` then maps screen to engine via
`engine_a_on_top ? screen : (screen ^ 1)`. Reading it as an engine index is what
produced the wrong conclusion.

That the field is 3D also explains the phrase "modelled special
units": the units are geometry, not sprites, which is consistent with OBJ being
off on that engine.

## Consequence: the 2D widener is not the answer for the field

`src/lb_wide2d.inc` (added here) is a correct 2D counterpart to the adaptive
compositor and it does widen a plain text-BG scene — verified by a temporary
magenta probe in the widened columns, which appeared for engine B's scenes and
never for the field.

But the field is not a plain 2D scene. It needs the **engine-A** path, and that
path refuses it for a reason the shipped code states plainly:

```c
const bool windows_enabled = (u.dispcnt & 0xE000u) != 0u;
const bool supported_scene = ... && !windows_enabled && ...;
```

**WIN0 is enabled on the field, so the shipped widener letterboxes it.** Window
regions are defined in hardware coordinates; widening one means deciding what
the window means in columns the hardware never had.

## The real work, in order

1. **Extend the engine-A compositor to windowed scenes** — decide what WIN0
   means outside the hardware viewport (almost certainly: the new columns
   inherit the region the window has at the viewport edge).
2. **Stop the 3D renderer culling at the vanilla viewport**, so geometry outside
   256 columns is submitted and drawn — units, buildings, projectiles, particles.
3. **Guest-side entity culling**, if the game itself skips submitting geometry
   outside the visible area. Lockstep-safe (`docs/85`) but a guest change.
4. **Fog of war** across the widened columns.
5. **The minimap viewport rectangle** on the top screen, which draws the
   on-screen area and would otherwise still describe a 256-wide view.

Items 2 to 5 are the requirement that objects outside the vanilla FOV
are not culled; none of them can be tested until item 1 puts real pixels in the
widened columns.

---

# The field is widened. Three things were in the way, not one.

Measured, in this order, each found by making the code say why it stood down
rather than by guessing:

**1. The scene was refused for having windows.** The engine-A widener's
`supported_scene` excludes `windows_enabled`, and the field enables WIN0. But
this renderer **implements no window logic at all** -- the only mentions in
`gpu2d.cpp` are that test and an OBJ-window stub. The native path ignores WIN0
entirely, so refusing to widen for it excluded the one screen that needs
widening, in exchange for nothing. (`WIN0H`/`WIN0V` are write-only and read back
as 0; the readable state is `WININ=0x3F`, `WINOUT=0x0000`.)

**2. Background extension was never switched on.** `g_adaptive_bg_extend`
defaults to `false` and **nothing in the runner ever called its setter**. Its
sibling setters in `main.cpp` are all gated on `NDS_ADAPTIVE_TOP`, an LSW2
assumption; this title is adaptive on the **bottom**. Until it was enabled, only
the 3D layer could fill the new columns, so a tilemap RTS got black.

**3. Only ONE background was allowed to extend.** `wide_bg_slot` picks the
furthest-back layer, because extending a 256-wide tilemap wraps it onto itself
and prints the art twice -- a real problem, and the reason the restriction
exists. But it is the wrong test. Engine A's field is:

| layer | priority | tilemap |
| --- | --- | --- |
| BG1 | 3 (furthest back) | **512x256** |
| BG2 | 1 | **512x256** |
| BG3 | 0 (front) | 256x256 |

**A 512-column tilemap has real data past the viewport; a 256-column one does
not.** Extending only BG1 widened the picture by *four pixels*. Testing the map
size instead extends BG1 and BG2 and leaves BG3 alone, which is both wider and
more correct than the single-slot rule -- it distinguishes the case the original
comment was worried about from the case it was preventing.

Result, from a live match: content spans **x = 0..447** of a 448-wide surface,
where before it was 96..351 with black either side.

## Still open

* **Objects are not extended.** OBJ is disabled on this engine and the units are
  3D geometry, so what appears in the new columns is terrain. Whether the 3D
  renderer submits geometry outside the vanilla viewport is the next question.
* **Fog of war, particles, projectiles** across the new columns.
* **The minimap viewport rectangle** on the top screen still describes a
  256-wide view.
* **Menus**: the same widening applies, but a menu background that is a single
  colour or a 256-wide tilemap needs stretching or tiling rather than extension.

## Menus: edge fill, because an affine background has no honest continuation

The menus run in **BG mode 3** (`DISPCNT 00010D0B`), which makes BG3 an
affine/extended layer. An affine layer's transform is defined against the
hardware screen, so decoding past the viewport walks off the map rather than
revealing more of it: there is nothing truthful to extend.

`lb_edge_fill` carries **each row's edge pixel** outward instead. For the flat
and vertically-graded backgrounds these menus use, that is exact -- a single
colour tiles perfectly and a gradient continues -- and for a patterned one it
smears rather than fabricating structure. It never distorts the menu's own
layout, which a horizontal stretch of the whole image would.

Verified: the Single Player menu now covers x = 0..447 with a seamless gradient,
its buttons still centred and unstretched.

---

# RETRACTED: the field was not widened. It was showing stale memory.

The section above claims the field spans x = 0..447 with "real terrain and trees
on both flanks". **That is wrong.** Seen on a real screen rather than through a
pixel histogram, the widened columns are a repeating block of stale tiles --
visible corruption either side of the play area.

**The error:** BG1 and BG2 are 512x256, and I treated "the tilemap is 512 columns
wide" as "the tilemap has 512 columns of map in it". It does not. The game fills
tile entries only for the 256-column window it draws, plus a small scroll
margin; the rest of the 512-column map holds whatever was written there last.
Decoding it reads memory the game never filled.

My verification was a colour histogram -- "the side bands now have 24/87 distinct
colours instead of 1". That distinguishes *empty* from *not empty*. It cannot
distinguish *map* from *garbage*, and I reported it as if it could. A histogram
is not a picture.

**Also retracted: `lb_edge_fill` as a solution.** Carrying each row's edge pixel
outward produces horizontal streaks. On the menus it passes; on the field it is
plainly a smear. It is padding, not field of view. Both it and the BG-extension switch are reverted; the port is back to the
shipped letterbox, which is at least honest about showing nothing.

## Why this title is harder than the 3D recomps

Widescreen in a 3D recomp -- Prime Hunters, and the same approach elsewhere --
works because geometry is submitted in world space and the renderer is asked for
a wider frustum: everything outside the original viewport was always there to
draw. **LEGO Battles' terrain is a 2D tilemap**, and a tilemap only contains what
the guest wrote into it. No host-side compositor can widen it, because the
pixels do not exist anywhere to be read.

Half the problem is already solved and half is not:

* **The 3D half works.** `nds_gpu3d_set_output_width` calls `SetRenderWidth`, so
  the 3D renderer genuinely rasterises at 448 with a widened frustum. LEGO
  Battles' units, buildings and projectiles are 3D geometry (OBJ is disabled on
  that engine), so they will appear in the new columns as soon as the guest
  submits them -- which is the entity-culling question.
* **The terrain half needs the guest.** The BG tilemap must be filled for a
  wider span. The hardware allows it: BG1 and BG2 are already 512 columns and
  only 448 are needed, so there is room for a full widened view without changing
  the map size.

## The actual next step

Find the code that writes tile entries as the camera scrolls, and widen the span
it fills. Located so far: `func_02038890` configures the field's layers (it
writes BG3CNT at `0x0400000E` with `0x284`, matching the live register), reached
from `func_02039A5C` and `func_02039D1C`. The tile-fill loop itself is **not yet
found** -- it addresses VRAM through a computed base rather than a literal, so
scanning the region for `0x06xxxxxx` constants finds nothing.

Until that is done, the honest state is the letterbox.

## Requirement: HUD elements should re-anchor as the window changes

Elements pinned to a screen edge or corner should follow the new edge when the
window resizes, rather than staying where the 256-wide screen put them.

**The runner already has this mechanism**, unused by this title:

```c
void nds_gpu2d_set_adaptive_hud_anchor(bool enabled, int centre_width);
```

`gpu2d.cpp` consumes it as `g_adaptive_hud_anchor` / `g_adaptive_hud_center_width`
and, when on, treats the middle `centre_width` columns as the HUD's centre band
and pins the rest outward -- exactly the required behaviour.

It is set from `main.cpp` beside the skybox-fill and SM64DS switches, and like
`adaptive_bg_extend` (`docs/107`) **its call is gated on `NDS_ADAPTIVE_TOP`**:

```c
nds_gpu2d_set_adaptive_hud_anchor(
    frontend_options.adaptive_hud_anchor &&
    (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u,
    frontend_options.adaptive_hud_center_width);
```

This title is adaptive on the **bottom**, so the condition is false and the
feature can never engage -- the same LSW2 assumption that kept BG extension off.

**Not yet done, and deliberately:** turning it on is a two-line change, but the
widened field it would anchor against does not exist yet (the field still
letterboxes, `docs/107`). Enabling an anchoring pass over a picture that is not
widened would move HUD elements to the edges of a black band. The order is: make
the field genuinely wider first, then anchor the HUD to it.

## The raster widens; the projection does not

With the windows check relaxed and **no** BG extension, the engine-A widener now
runs on the field -- `[lb_wideA] screen 1: WIDENING (dispcnt 00010F08, w=448)` --
and the result is a 448-wide surface whose side bands are **a single colour,
pure black**. The 3D layer is rasterised at 448 and contributes nothing outside
the middle 256 columns.

That is the expected behaviour of `SetRenderWidth` on its own: it widens the
**raster**, while the guest's projection matrix still maps the world onto a
256-wide viewport. Rendering that projection into a wider buffer adds empty
space, not field of view. Widening a 3D scene properly means widening the
**horizontal frustum** -- the standard 3D widescreen technique -- which means
touching the projection the guest loads through GX, not the compositor.

## Two readings of the BG evidence, and why I am not choosing yet

Measured: `BG1`, `BG2` and `BG3` scroll registers sit at **508** and do not move
under any input tried -- D-pad, stylus drag, or a held touch at each screen edge.

* **Reading A: the terrain is 3D geometry** and the BG layers are static
  overlays, so BG scroll would never move and the tilemap watched in `docs/109`
  belongs to something else.
* **Reading B: the camera simply never moved** in any test, because no input
  that scrolls it has been found. Static BG scroll is equally consistent with
  this.

Every screenshot taken so far shows the same view, which fits B at least as well
as A. **Choosing between them requires making the view move once**, by any means,
and that is still unsolved -- so neither is recorded as fact.

What *is* solid either way: **the widened columns are empty**, from both the BG
path (block 1 never written, `docs/109`) and now the 3D path (black sides with
the widener active). No compositor change fills them. The remaining candidates
are a projection-matrix widening for the 3D case, or guest tile streaming for the
BG case, and which applies depends on the question above.

## Resolved: the terrain is NOT 3D. Reading B holds.

`gx_polygons` on a live match:

```
count: 8
  quad 0  4 verts  x 212..267  y   8..64
  quad 1  4 verts  x 203..227  y  56..80
  quad 2  4 verts  x 252..273  y 114..118
  ...
```

**Eight quads.** A 3D terrain would be hundreds. These are the units and
buildings, drawn as textured quads through the 3D engine -- which is what the
player sees as "modelled" pieces, and why OBJ is disabled on that engine.

So **the terrain is the BG tilemap after all**, and the static BG scroll
registers mean exactly what Reading B said: **the camera never moved in any test
so far.** Every screenshot showing the same view was not a coincidence.

That settles the earlier fork and puts the widescreen requirement back on the BG
side, where `docs/109` measured it:

* **terrain** needs block 1 of the tilemap filled -- the guest streamer, or the
  port generating those entries from the map;
* **units and buildings** are 3D quads and need the projection widened, or they
  will stay in the middle 256 columns while the terrain extends past them;
* **the HUD** then needs re-anchoring (the mechanism already exists, above).

All three are guest-side or projection-side. **No arrangement of the host
compositor produces a wider view**, which is now established from both
directions and is the single most useful conclusion of this thread.

## What this thread still needs, in order

1. **Make the view move.** Not the D-pad, not a field drag, not a screen-edge
   touch. Until this is solved nothing about scrolling can be measured, and it
   blocks everything else.
2. Fill block 1 for the widened columns.
3. Widen the 3D projection so the quads follow.
4. Re-anchor the HUD.
