# 217 — the probe that could not click

> **RETRACTED IN PART — see docs/218.** This document's central negative
> result, that a click on a unit outside the native 256 columns does not
> select it, is WRONG. It does, and the touch extension is what makes it
> work: with the player's base parked at screen x −61, 4 of 12 clicks across
> its sprite select it with the extension on and 0 of 12 with it off, and the
> top screen names the selection as "Eagle Command 1500/1500".
>
> Three things hid it, and each is worth reading here rather than only in
> docs/218: the harness could not click at all (the finding below); the
> entity
> position the port reports is not the clickable one, so even IN-band clicks
> on a building missed; and a CLAMPED click lands at native x 0, so a target
> whose hit area covers 0 is hit whether or not the extended coordinate does
> anything.
>
> Everything else below — the delivery chain, the addresses, the patch
> encodings — stands, and is what the working feature is built on.


**Goal this serves.** The requirement that has stayed open longest: touch
input has to work beyond the original aspect, for clicking, selecting units,
commanding units, and clicking and dragging.

## The finding

`tools/lb_exp.py --click-ext` has never clicked anything. Not the unit outside
the band, and **not its own control either**.

```c
bool nds_frontend_debug_touch(uint16_t x, uint16_t y, bool down) {
    if (!g_input_debug.active || x >= kScreenWidth || y >= kScreenHeight)
        return false;
    ...
    event.button.x = (g_input_debug.bottom_content_left + x) * kWindowScale;
```

It takes **guest** coordinates, refuses anything outside 0..255 / 0..191, and
places what it accepts through the **vanilla stacked layout**. The probe passes
**window** pixels — 412, 421 — so every call returned false and pushed no event.

Both halves are wrong for the one question the enhanced layout exists to answer.
A click in a widened column has no guest x inside 0..255 *by definition*, so it
can never be expressed; and a click that is accepted is placed by a layout the
enhanced path does not use.

So docs/214's *"selecting a unit out there does not work"* was not a measurement
of selection. It was a measurement of a probe that could not click.

## The fix

`action="click"` on `frontend_input`: window pixels, signed, pushed unchanged,
so the enhanced layout's own mapping is what gets exercised. A motion event goes
first, because ImGui tracks the cursor from motion and `lb_overlay_event`
swallows a click it believes is over a panel — a button event with no motion
before it is judged against wherever the cursor last happened to be.

## Three more things the probe was getting wrong

* **The selection detector read the wrong number.** It parsed the count of
  elements *moved*, which is zero whenever anchoring is off or the window is
  narrow. docs/212 introduced `hudquads` as the detector precisely because the
  in-field HUD does not exist until something is selected.
* **The deselect never happened**, so the out-of-band click was tested against
  a selection the control had already made — `hud_before=5`. Two targets were
  tried and measured before one worked: the field's bottom centre is where the
  HUD panel is (5 → 6), and empty ground does not deselect either, because in
  this game a click on the ground with a unit selected is a **move order**. The
  deselect icon is the only thing that deselects.
* **The y was mapped through 192** on a surface that is now up to 256 rows
  (docs/215).

## What the working probe says

```
click-ext CONTROL: in-band sx=35 window=(412,421) -> hud=5 SELECTED
click-ext: after deselect hud=0
click-ext: hero sx=-61 ... surface x=7 -> window (27,441)  hud_before=0
click-ext: attempt 0 -> hud elements=0
click-ext: attempt 1 -> hud elements=0
click-ext: attempt 2 -> hud elements=0
```

* An in-band click **selects**.
* The deselect **works** — and it works by clicking the icon at its **anchored**
  position, which is an independent confirmation that `lb_hud_unmap_touch`
  (docs/212) does what it claims.
* A click on a unit at screen x −61, in the left widened column, **does not
  select it**. With `LB_TOUCHEXT=1 LB_WIDEMAP=1` and all five touch patches
  reporting applied, it still does not.

**And the coordinate genuinely arrives.** Draining the watch ring *at the click*
rather than at the end of the run — the resting writes put `0x0215376A = 128`
seventy times per drain, so by the end the click's own value is long evicted —
shows the guest's own touch struct receiving exactly what was clicked:

```
re-aim sx=-45 -> touch struct saw ('0215376A', 65490)     = -46
re-aim sx=-37 -> ('0215376A', 65498)                      = -38
re-aim sx=-29 -> ('0215376A', 65506)                      = -30
```

So delivery is not the problem. One run did report a selection at sx −13, on an
attempt whose click position had not been re-aimed while the hero walked; with
re-aiming it does not reproduce, and it should be read as the hero's sprite
still covering a stale click point rather than as out-of-band selection working.

`--click-ents` settles it without a walking target at all: it clicks every
entity the port reports out of band, deselecting between each.

```
bp=133 SpaceCriminalLeader  sx=-77  hud 0 -> 0  not selected
bp=82  E_Engineer           sx=-21  hud 0 -> 0  not selected
bp=90  E_EagleCommandBase   sx=-21  hud 0 -> 0  not selected   <- a STRUCTURE
```

A base does not move, so that last row is the clean measurement the hero could
never give.

**This paragraph used to read "docs/214's conclusion survives". It does not**
-- see docs/218. The rows above are real, and what they were missing is that
a building's clickable pixels are not where its reported position is, so a
grid that never lands on the sprite reports "not selected" for a reason
that has nothing to do with the boundary.

## And the consumers are enumerated

`LB_WATCH=2153760:2153790 LB_WATCH_RW=r LB_WATCH_NOTPC=203dd14` with
`--drainwatch`, during a tap that selects a hero at screen x 147:

```
READ pc=020A87FC n=6  ('0215376C', 95)    the Y
READ pc=020A8800 n=6  ('0215376A', 147)   the X
READ pc=020A880E n=6  ('0215376C', 95)
READ pc=020A8812 n=6  ('0215376A', 147)
READ pc=0203DE9C n=4  ('0215376A', 147)   the drag reader docs/214 patched
READ pc=0203DEA0 n=4  ('0215376E', 147)
```

**Two of the four are `ldrsh` and two are `ldrh`**, and the difference matters:

```asm
020a87f8  ldr   r1, =0x0215376c
020a87fa  movs  r0, #0
020a87fc  ldrsh r2, [r1, r0]        ; Y, SIGN-extended
020a87fe  ldr   r1, =0x0215376a
020a8800  ldrsh r1, [r1, r0]        ; X, SIGN-extended
...
020a8808  bl    func_020aa8e4       ; gets the SIGNED pair

020a880c  ldr   r0, =0x0215376c
020a880e  ldrh  r1, [r0]            ; Y, ZERO-extended
020a8810  ldr   r0, =0x0215376a
020a8812  ldrh  r0, [r0]            ; X, ZERO-EXTENDED
020a8814  strh  r0, [r5, #0x20]     ; stashed on the object
020a8816  strh  r1, [r5, #0x22]
020a881c  bl    func_020aa0a4
```

The `strh` itself is harmless — storing 65490 writes the same sixteen bits as
storing −46 — so the damage, if any, is in whatever reads `[r5+0x20]` back. If
that reader is also unsigned, an out-of-band x is 65490 to it and fails every
bounds test.

That is the narrowest the question has been. Not patched here, because "these
two are `ldrh`" is not yet "this is what rejects the click": `func_020aa8e4`
already receives the correctly signed pair on the line above, so the signed
path exists and something else may be doing the rejecting. The next measurement
is what reads `[r5+0x20]`.

## The general lesson, recorded because it has cost this project twice

A probe whose setup fails **silently** is worse than no probe. Both failures
here reported success-shaped output: `nds_frontend_debug_touch` returns `false`
and the harness ignored it, and the deselect click returned `ok` while
deselecting nothing. `--click-ext` now prints what happened after the deselect
and says outright when the test below cannot distinguish a new selection.
