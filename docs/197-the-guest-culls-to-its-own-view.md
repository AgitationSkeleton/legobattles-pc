# 197 — The guest culls to its own view, and that is the whole feature

Status: **measured, but the instrument is now in doubt -- see the warning
below.** This is the finding that decides what "adaptive screen aspect, fully
implemented" costs, so it is worth stating on its own.

> ## WARNING: re-measure before building on this
>
> The polygon counts here come from `gx_polygons`, and two rig faults were
> found afterwards (docs/198): stray emulator instances sharing the debug port
> under a name the kill command did not match, so commands and screenshots
> could reach different processes; and the same two polygons later read
> identically at the TITLE SCREEN as had been recorded in a battle.
>
> The runs behind this document used `lb_exp.py`, whose children are named
> `nds_runner.exe` and WERE being killed correctly, so they are less likely to
> be affected than docs/198's. But "less likely" is not a measurement.
> `assert_single_runner()` now guards the tools; the pan sweep below should be
> re-run under it before any work is built on the conclusion.

## The question

The widened surface is 448 columns and the guest's own view is columns 96..351
of it. Everything the feature needs beyond "fill the window" — more field of
play, units and buildings not culled, clickable space past the original aspect —
depends on one fact: **does the guest submit anything outside its 256 columns?**

Two ways it could have gone. If the guest hands the renderer everything nearby
and only the DS's viewport clips it, then widening the viewport is a renderer
change and the whole feature is cheap. If the guest culls, nothing the host does
to the raster can invent what was never submitted.

## The measurement

`lb_exp.py --pan` now dumps `gx_polygons` at each pan step and counts polygons
whose bounding box falls outside the native band:

    pan step 0: 3 polys, 2 outside native, x  80..144
    pan step 1: 0 polys, 0 outside native
    pan step 2: 0 polys, 0 outside native
    pan step 3: 0 polys, 0 outside native
    pan step 4: 0 polys, 0 outside native
    pan step 5: 0 polys, 0 outside native

Both halves of the answer are in there.

**Geometry does render outside the native view.** `min_x` of 80 is sixteen
columns left of column 96, so the DS viewport is not clipping at the native edge
— melonDS's widened frustum carries a unit that straddles the screen border, and
it is drawn into the widened columns. That part already works.

**But it stops.** Between step 0 and step 1 the world moved about 123 columns
left, which puts those same units at roughly x -43..21 — still overlapping the
448 surface, so a sliver should have been rendered. The polygon count is zero.
The guest stopped submitting them.

So the guest culls to its own view, with a margin of at least 16 columns and
less than the ~123-column pan step. The margin cannot be pinned tighter than
that from this run: the D-pad is held for 0.35 s per step, which is far too
coarse, and a finer probe would need the camera written directly.

## What this means

Showing more battlefield is **not** a rendering problem. Three separate guest
mechanisms each clamp to the 256-column view, and all three have to move
together or the result is incoherent:

| what | where | measured |
|---|---|---|
| tile data | `func_020A3234`, `cmp r7, #0xA` | 10 metatiles = 32 columns (docs/196) |
| unit and building geometry | not yet located | culls past a small margin, this document |
| fog mask | `020333A4`, `mov r1, #32` | 32 tiles, and `LB_VIEW_WIDTH` already moves it (docs/192) |

`LB_VIEW_WIDTH=448` moves the third and nothing else, which is exactly why it
changed the fog object's stride from 4 to 7 bytes and did not change one pixel
of output.

The honest consequence: **the port cannot show more of the battlefield until the
guest's own view width is widened in all three places.** Until then the black
either side of the field is not a bug to be fixed in the compositor — it is the
truthful representation of ground the game has not drawn, and filling it with
anything else would be inventing map.

## What is worth doing next, in order

1. **Find the culling bound.** It is the one of the three not yet located. The
   view object carries the camera at `+0x5E`/`+0x60` and a dirty flag at
   `+0x66`; a view rectangle is likely nearby, and finding it would also give a
   finer instrument than the D-pad for measuring the margin.
2. **Then widen all three together**, and only then. Widening the tilemap alone
   would show terrain with units popping in at the old boundary, which is worse
   than the honest black.
3. **Then the HUD** (docs/194), which is five screen-locked 3D quads and is
   deliberately parked until the field fills the window, because anchoring a
   button into a black pillarbox is not an improvement.

## Harness note

The route now verifies every tap. It previously used fixed sleeps, and one run
ended at the main menu with the harness reporting "route did not
reach gameplay" — correct, but only because that check exists. `tap_verified`
re-taps when the screen does not move, which turns a silent wrong-screen result
into either a success or an honest failure.

---

# RE-MEASURED on a clean rig, and the conclusion above needs softening

The warning at the top can now be resolved. Everything below was measured with
`assert_single_runner()` active (one instance, guaranteed), with the route
reaching a real battle, and with `in_gameplay` confirmed at **every** reading
rather than only at the start.

## What the route needed before any of this could be trusted

Three separate faults, each of which had been producing plausible-looking runs:

* **Boot detection fired on the copyright screen.** The old test was "the screen
  stopped being black", and the boot goes black → logos → grey-on-black
  copyright → title. The route spent its first four taps on boot screens. It now
  waits for the title by COLOUR — that screen is a full blue gradient and
  nothing earlier in the boot is blue.
* **`tap_verified` retried up to four times.** Half the route's steps are
  SELECTION taps that only move a highlight, far below any change threshold, so
  they were read as failures and tapped again. The second tap confirms, and the
  third and fourth land on the NEXT screen — where (205,169) is a Back button on
  several. That is precisely how a route with correct coordinates walked forward
  and then came back out to the title. It now taps once and reports the change
  as information.
* **`frontend_input action=touch` takes WINDOW coordinates**, not guest ones,
  and runs them through the enhanced mapping, which rejects anything outside the
  centred 256-column band. Guest (128,96) becomes guest x = 64 − 96, negative,
  and was discarded silently. An earlier note blaming a race with the frontend
  for dropped raw touches was **wrong**: the real cause was the two-instance
  fault in docs/198. Raw `touch`, in guest coordinates, works.

## The re-measurement

Panning right, away from the base:

    pan step 0: 3 polys, 0 outside native, x 112..177
    pan step 1..5: 0 polys

Panning left, toward the map edge, then A/B/A/B on the cull width:

    pan step 0: 3 polys, 2 outside native, x 312..376
    pan step 1..5: 3 polys, 3 outside native, x 339..403   (camera clamped)

    width 256 -> 3 polys, 3 outside native, in_gameplay=True
    width 448 -> 3 polys, 3 outside native, in_gameplay=True
    width 256 -> 3 polys, 3 outside native, in_gameplay=True
    width 448 -> 3 polys, 3 outside native, in_gameplay=True

## What this changes

**"The guest culls to its own view" was too strong.** Geometry renders out to
x = 403 — **52 columns beyond the native right edge at 351** — under completely
stock settings. Units near the edge of the view are already being drawn into the
widened columns, and that is reproducible across five pan steps and four A/B
readings. The margin is far larger than the 16 columns the first measurement
suggested.

What survives is the weaker and still important claim: geometry does eventually
stop. Panning right, the polygon count goes to zero and stays there, so the
guest is not submitting the whole map.

**The A/B remains inconclusive, for a reason that is now understood rather than
unknown.** The camera clamped against the map boundary after one pan step —
steps 1 through 5 are identical — so there is no map further right for a wider
rectangle to gather from. The experiment needs a camera parked mid-map with
units beyond the view, which needs the camera written directly, which needs the
view object's address. That is the next step, and it is a smaller one than it
looked: the structure test in `lb_probe.py camera` already filters on the four
callback handles at +0x4C..+0x58.

## And a capture settles which half is actually missing

`screenshots/exp-cullab/1280x720.png`, in a real battle at 1280x720:

**The terrain stops dead at the native edge and the UNITS CARRY ON PAST IT.**
The tilemap ends at window x≈1085; the player's base and two units are drawn
from x≈1085 to x≈1260, standing on black.

That is the 52-column margin above, made visible, and it inverts the plan in
docs/196 and docs/197:

* **3D geometry — units and buildings — already renders in the widened
  columns.** Nothing needs to be done to the entity cull to see units out
  there. They are there now.
* **The 2D tilemap does not.** The black either side is missing TERRAIN, not
  missing units.

So the remaining work for "more of the field of play" is exactly the one thing
already reverse engineered end to end: the host-side tilemap fill of
docs/196 — writing the tile columns the guest leaves empty into VRAM screen
block 1, which the port's decoder already reads and which needs no change to the
guest's staging buffers, fill loop or DMA.

The ordering advice in "What is worth doing next" above is therefore wrong and
is retracted: it said to widen all three mechanisms together, on the grounds
that terrain without units would look worse. Terrain is the half that is
missing, and units are already drawn on top of the black.

Still true: fog. The widened terrain must carry the shroud, or the new columns
reveal ground the player has not explored.
