# 307 — The footprint fix was never verifiable: the harness never reached placement

docs/306 settled the design for the placement footprint and named the next
step as implementation. Before writing any of it, the question worth asking
was whether the result could be *measured* — and the answer was no.

## The instrument

`[lb_fp]`, in `lb_widemap_scan` (render time, so the guest has already painted
this frame). It reads the footprint tilemap through **the guest's own row
table** at `0x021520F8` rather than a computed stride, because docs/286 found
the geometry lives entirely in that table — `func_0203D8E8` hardcodes no stride
at all — so reading it the guest's way is what keeps the probe honest if the
table ever moves.

It reports cell count, bounding box, the split between `0x4F` (placeable) and
`0x3F` (blocked), the camera, and whether the box touches the cull edge.

**It cannot lag the game.** The whole scan sits behind one cached `LB_FPLOG`
bool and is off by default. With it on, the cost is 832 halfword reads on a
frame whose summary *changed* — an unchanged summary prints nothing, so a
static footprint is silent.

First run, and it immediately confirmed two things from docs/286 that had only
been read off the disassembly:

```
[lb_fp] cells=832 box=0,0..31,25  placeable=0 blocked=0 other=832  cam=0,0
[lb_fp] cells=64  box=0,24..31,25 placeable=0 blocked=0 other=64   cam=136,144
```

The buffer is live at `0x021530A0` and addressable through the row table; and
**rows 24 and 25 stay `0x0000`** because `func_020370B4` clears only `0x600`
bytes — 24 rows — while the table builds 26. docs/286 noted that mismatch; this
is it in the running game.

What never appeared in any run is a single `0x4F` or `0x3F`.

## Why: the flow stops three steps short

`--build-flow` exists to drive the player's build sequence: tap the builder,
then the red brick, click an icon, place it in the world, then tap the
checkmark. It does the first two and stops. Two separate faults, both
measured:

**1. The re-select was one tap at the wrong offset.** The first selection
sweeps nine aim offsets twice — eighteen clicks — and the file explains why in
its own comment: the port logs the *entity origin* while the sprite's clickable
band sits below it, and even on the calibrated offset a click lands "about one
time in three". The re-select between trials then did **one** tap at `dy=0`.
Measured consequence: every quad after the first was skipped.

```
before:  5 x "could not re-select ... skipping"      0 of 5 quads tapped
after:   hudquads 6 -> 13, 13 -> 11, 7 -> 20, ...    5 of 5, two open menus
```

**2. The unit does not stay put, and the retries were not retries.** After the
first selection the builder had drifted to surface **x = -405**, far off the
left of the native strip, so every offset failed the `0 <= ax < 256` guard and
was **skipped without tapping**. Nine "attempts" that never touched the screen
are indistinguishable in the log from nine misses, which is what made a stale
position read as a flaky click.

An identity mix-up was the first hypothesis and it is **wrong**: the run holds
exactly one player builder, `ptr=02307460`, the only `owner=0` `[lb_builder]`
in 136 lines. The camera moved, not the unit's identity. `aim_unit` already
exists to pan it back, and both re-select paths now call it before spending
attempts on offsets.

## Status

**FIXED and verified:** the quad sweep. 0 of 5 tapped before, 5 of 5 after,
with two quads opening menus (`6 -> 13`, `7 -> 20`).

**ADDED:** a placement stage that re-opens the build list by whichever quad
opened it, taps each newly-appeared quad as a candidate building icon, and taps
in the field — reporting `[lb_fp]` after each, so it states a measurement
rather than assuming the taps worked.

**STILL FAILING:** the re-select immediately before that stage. The last quad
of the sweep takes `hudquads` to 0 (it is the cancel), and re-selection from
there does not recover within the attempts allowed.

**NOT FIXED:** the placement-footprint bug itself. No code for the footprint
fix was written in this pass, and none should be until a placement can be
driven — a fix that cannot be observed is not a fix.

**The finding that matters most:** the footprint bug was not verifiable by this
harness in any window size, which is the reason it has sat "traced but
unimplemented" across several passes. That is a prerequisite, not a lower priority.

## Next

Recover selection after the cancel quad — most likely by not tapping the cancel
quad at all during the sweep, since its effect is now known and re-discovering
it costs the run its selection.

## Second pass: three more faults fixed, and the one that remains

**FIXED — the sweep was destroying its own selection.** Its last quad,
`(16,180)`, takes `hudquads 18 -> 0`: it is the cancel. The sweep's own header
already says "the checkmark and cancel sit at the bottom", so re-discovering it
every run costs the state the placement stage needs. Bottom-row quads
(`cy >= 150`) are now skipped by name rather than by accident.

**FIXED — and this was the real one — a `0..255` guard I had added myself.**
The re-select rejected any unit outside the native strip. But the file says,
forty lines above, that this is *normal*:

> the camera clamps at the map edge while the unit sits at surface x -6, in the
> left band — legitimate game state, not a fault

Measured: **131 of 137** `owner=0` `[lb_builder]` samples in one run read
`sx=-6`; the six that did not are the moment `aim_unit` had it parked. So the
guard rejected the unit's ordinary position and every "attempt" was skipped
without touching anything — which is why nine attempts looked like nine misses.

`click6()` exists precisely to reach those coordinates through the **window**
rather than the DS touchscreen, and the first selection has no such guard,
which is why it succeeded where the re-select failed. Both re-select paths and
the field taps now use `click6` first, with `tap()` kept only as the in-range
fallback.

With those two, the sweep is reliable and the build list opens:

```
hudquads 6 -> 13,  13 -> 7,  7 -> 18,  18 -> 18
(skipping bottom-row quad at (16,180): checkmark/cancel, measured to deselect)
build list open, 6 new quad(s)
```

**STILL NOT REACHED: placement.** Per-icon instrumentation says where it stops:

```
icon (3,28): hudquads 6 -> 6, footprint 0
icon (11,28): hudquads 7 -> 7, footprint 0
```

Two distinct problems, both now visible rather than inferred:

1. **The stage re-taps `best_quad` when the list is already open**, which
   closes it again — the sweep ends at 18 and the icon taps begin at 6.
2. **Quad coordinates drift between reads.** The "new" quads come back as
   `(3,28) (11,28) (3,52) (11,52) (11,179)` against an earlier
   `(8,28) (16,28) (8,52) (16,52) (16,180)` — the same five buttons, about 5 px
   apart, because the surface-to-native shift differs between the two reads. So
   "not in the before set" identifies everything as new and nothing as an icon.

Also worth recording so it is not misread later: `footprint -1` in those lines
is **not** a game state. It means no `[lb_fp] cells=` line remained in the tail
window, because the probe only prints when its summary changes and the
footprint stayed empty. A probe that is silent when nothing changes is right;
the reader just has to know that -1 is "no report", not "zero cells".

## Status after this pass

**The footprint bug is still not fixed and no footprint code is written.** What changed is
that the harness now gets three steps further and reports precisely where it
stops, which is the difference between an unverifiable item and one blocked on
two named, small faults.

## Third pass: the build list is reached and driven; placement still does not start

Both faults named above are fixed, and both fixes are confirmed in the log.

**The list is no longer closed by the stage itself.** It now checks before
re-tapping:

```
build-flow: build list already open (hudquads=18); not re-tapping
build-flow: build list: 18 quad(s), 11 of them not one of the unit's own buttons
```

That one change also cured the "coordinates drift" symptom, which was not a
separate fault at all: `hud_rects()` takes the last logged block, and with the
list shut it was returning the unit's own six buttons a few pixels from where
the sweep had seen them. One cause, two symptoms.

**The icons are identified by position and size, and driven left to right.**
The 18-quad block is explicit about the geometry — a horizontal strip of ~24 px
quads at surface `y 157..181`, `x 272..488`, which at `shift=265,117` is native
`y 40..64`, `x 7..223`:

```
icon (44,52): hudquads 18 -> 18, footprint 0
icon (67,52): hudquads 18 -> 18, footprint 0
```

Three further corrections went in with it:

* **Order left to right.** The first candidate tried had been `x=227`, past the
  strip's right end, and it did nothing — a scroll arrow or an empty slot.
* **Re-open between icons.** A field tap is a world click and deselects, so
  from the second icon onwards every tap had been hitting a closed list.
* **Ask before touching the field.** If an icon starts placement the footprint
  paints at once, and a field tap would be a *place*, which ends it. Checking
  first is what separates "placement never started" from "placement started and
  the next tap finished it".
* **Match width, not just centre.** The leftmost icon is `(7,40,32,64)` —
  centre `(19,52)`, width 25 — while the unit's button is `(12,40,20,64)`,
  centre `(16,52)`, width 8. Three pixels apart, so a centre-only test was
  discarding the first icon in the strip.

**What has not changed: no footprint, ever.** With the list open and the icon
tapped, `hudquads` stays at 18 and the buffer stays inert. Either the taps are
not landing on the icons, or an icon tap alone does not enter placement.

## Status, and the honest accounting

Three passes of harness work have taken `--build-flow` from "five skipped
quads, nothing tapped" to "build list open, eleven icons identified and driven
in order". **The footprint bug itself is untouched: no footprint code is written, and the
footprint has never been observed.**

The next step should stop inferring placement from its side effect. The game
has a placement state — `func_020370B4` takes it as its argument — and reading
*that* says directly whether an icon tap entered placement, where the footprint
buffer can only say whether something painted. Probing the state is a smaller
question than driving the UI blind, and it is the kind of question this
project's decomp can answer.

## Fourth pass: asked the game directly, and the answer is no

Three passes inferred placement from its side effect. The game holds the answer
outright, and `func_020398B8` — the view tick — shows where:

```asm
ldrb r0, [r4, #0x78]     ; r4 = the view
cmp  r0, #0
beq  <skip>
add  r0, r4, #0x6C       ; the placement state is the view's OWN sub-struct
bl   func_02037EC0       ;   at +0x6C, not a separate object
```

So `view+0x78` is the placement flag and `view+0x6C` is the state
`func_020370B4` paints from — and docs/286 already read that state's layout:
`+0x04/+0x05` origin tile, `+0x10` mode, `+0x20/+0x21` footprint size, `+0x24`
the whole-site verdict `func_02037EC0` stores back. All of it is six byte reads
from an address this file already knows (`kViewPtr = 0x02152034`).

The probe now reports it, and the whole `--build-flow` run says:

```
[lb_fp] ... active=0 mode=72 origin=0,0 size=0x0 verdict=100
[lb_fp] ... active=0 mode=72 origin=0,0 size=0x0 verdict=100
```

**`active=0` throughout.** Placement is never entered — stated by the game's
own flag rather than deduced from an empty buffer. That is the difference
between "nothing painted, cause unknown" and a settled fact, and it is worth
the tick it cost: the previous three passes could not have distinguished a
failed tap from a culled footprint.

The `mode=72 verdict=100` values are stale bytes in an inactive sub-struct and
should not be read as meaningful while `active=0`.

### Where the flag is set — not in the ARM9 static image

Every `strb rX,[rY,#0x78]` in `section_static_02000000.bin` is four sites. Two
write through `r13`, so they are stack locals. The other two are not the view:
`func_02034A04`'s `r0` holds **pointers** at `+0x6C` and `+0x70`
(`ldr r3,[r0,#0x6c]; cmp r3,#0`), while the view has an embedded struct there —
a different object, a `SpriteUIEngine` by its callers.

So nothing in the ARM9 static image sets the view's placement flag, which puts
the placement-entry logic in an **overlay**. That is consistent with everything
else: the build list is UI, and 91 overlays carry this game's UI code.

## Conclusion for the footprint bug

**The harness cannot currently enter placement, and this is now measured rather
than suspected.** Four passes have taken `--build-flow` from "nothing tapped"
to "build list open, eleven icons identified and driven left to right with the
list held open" — and the flag still never sets.

**The footprint bug is therefore parked as needing either a manual play session or a
separate piece of work: finding the overlay that owns placement entry and what
input reaches it.** No footprint code has been written, and none should be
until the fix can be observed.

What this pass leaves behind is not nothing: a probe that answers the
question directly and costs the game nothing when off, the placement state's
location and layout, and a build flow that gets four steps further than it did.

## CORRECTED — placement IS reachable; the delivery was wrong

This page concluded that placement could not be entered by the harness, on the
strength of `view+0x78` reading `active=0` for whole runs. The flag reading was
right. The conclusion drawn from it was too broad: it was not the harness that
could not enter placement, it was **the way the icon taps were being
delivered**.

The build-list icons sit at native `y~52`, `x 7..223` — entirely inside the DS
touch panel. They were being tapped with `click6`, which sends window
coordinates through `frontend_input`. `tap()`'s own docstring already records
why that is the wrong tool there:

> `frontend_input action=touch` ... takes WINDOW coordinates and runs them
> through the enhanced mapping, which converts to the 448-wide surface and then
> REJECTS anything outside the centred 256-column band ... Raw touch works.

`click6` is the right tool for a **band** coordinate, which a raw touch cannot
carry. For a native one it is the longer way round, and docs/307's symptom —
`hudquads 18 -> 18`, a tap changing nothing at all — is exactly what a
mis-delivered tap looks like.

Tapping icons with raw `touch` when they are inside `0..255 / 0..191`:

```
build-flow:   icon (116,52): hudquads 18 -> 14, footprint 22

[lb_fp] cells=88 box=0,12..31,25  placeable=24 blocked=0 other=64
        cam=265,144  touching-edge=y
        active=1 mode=0 origin=2,2 size=16x15 verdict=1
```

**`active=1`.** Placement mode, from the game's own flag, with 24 cells painted
`0x4F` and the site's verdict reading valid. The capture shows the footprint
grid on screen with its checkmark button above it.

## What this unblocks

docs/306's design for the footprint fix was justified but unverifiable, and
docs/315 filed it as needing a play session. **It does not.** The harness can now enter
placement, the `[lb_fp]` probe reports the footprint's extent and the game's own
verdict, and the capture shows the result — which is everything needed to
measure a fix.

Worth noting already, before any fix: the first two active frames read
`box=0,12..31,25`, spanning the buffer's full width and running to row 25.
docs/286 predicted exactly this — cells are culled at 32x24 — and
`touching-edge=y` says the box reaches the cull boundary on this very first
observed placement.

## The footprint does not follow the cursor

With placement live, the obvious next measurement is to move the cursor into a
band and watch the cull. Three deliveries were tried and the footprint reported
`box=0,12..31,25 placeable=22` — identical — at all five positions each time:

* **click6 at each position.** Wrong for a reason this file had already
  written down ten lines earlier: a click in the field during placement is a
  PLACE. The first click committed the building and the remaining four
  positions were reading an idle buffer (`cells=64`, the uncleared rows
  24..25). My own note said so and I did it anyway.
* **`moveto` at each position.** Placement stayed live this time, but nothing
  moved. A DS touchscreen has no hover, so a pointer with no button has no
  stylus position to give.
* **One continuous drag** — press once, visit all five while held, release at
  the end. Also nothing.

The capture explains it. The footprint grid sits at a fixed spot on the field
with the confirm checkmark above it, unchanged across the whole sweep. **The
ghost is anchored where it was created**, so the game's model is "place the
ghost, then confirm", not "the ghost follows the stylus".

Which means moving it requires touching **the ghost itself**. From the capture
it sits at native ~(142,123); the sweep pressed at (128,96) and missed it.

## Where the footprint fix stands now

**Reachable and observable**: placement enters, `[lb_fp]` reports the box, the
0x4F/0x3F split and the game's own verdict, and the capture shows the grid.

**Not yet driven into a band**, which is the measurement docs/286 and docs/306
are both waiting on. The next step is specific rather than exploratory: press
on the ghost at its reported position and drag from there.

## The footprint's own box, and four ways that do not move it

The reported `box` was useless for this: it spans every non-inert cell, and
rows 24..25 are permanently `0x0000` because `func_020370B4` clears `0x600`
bytes — 24 rows — while the row table builds 26. That junk is 64 cells across
the full width, so the box read `0,12..31,25` regardless. **That is why five
cursor positions looked identical.**

`[lb_fp]` now reports the footprint's own bounding box over placeable|blocked
cells only, and flags it when it reaches the cull edge:

```
active=1 mode=0 origin=2,2 size=16x15 verdict=1  FOOTPRINT 14,12..19,15
```

Cells 14..19 by 12..15 — six by four — not clipped. A cell is 8 px, so the
ghost sits at native `112..159 x 96..127`, centre `(136,112)`. The harness
computes that rather than guessing it, and confirms: `ghost centre reads
(136, 112)`.

Pressing exactly there and dragging still leaves `FOOTPRINT 14,12..19,15`
unchanged. So four deliveries have now failed to move it:

1. click at each position — commits the build instead;
2. `moveto` — no hover on a touchscreen;
3. drag held from an arbitrary point;
4. drag held **from the ghost's own centre**.

**The ghost does not move by pointing at it.** Whatever repositions it is not a
stylus gesture on the field, which leaves the d-pad, a second tap semantics
this has not reproduced, or the possibility that in this mode the game fixes
the site itself and the player only confirms or cancels.

## Status

**Placement is reachable and fully observable** — `active`, `mode`, `origin`,
`size`, the game's own `verdict`, the `0x4F`/`0x3F` split and now the
footprint's box with a cull-edge flag. That is a complete instrument, and it
did not exist two passes ago.

**What is not yet reproduced is a placement in a band**, because the ghost
cannot be moved by any input tried. The footprint fix still cannot be verified, and
the obstacle is now precisely stated rather than general.
