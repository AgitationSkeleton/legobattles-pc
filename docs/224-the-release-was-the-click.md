# 224 — The release was the click

**Goal within the project:** the expanded field of view on the touch screen
must be just as interactable as the original aspect: tapping or commanding a
unit, click-dragging to select multiple units, and so on.

This fixes a real bug in the port's touch mapping, and then uses the working
control it buys to settle a question docs/218 got wrong.

## Two deliveries, one target, different outcomes

The probe reads the touch registers while the pen is held, so "what arrives" and
"what happens" are separate measurements:

```
tap=(147, 147, 95)  SELECTED
frontend=(146, 146, 94)  nothing
```

`slot`, `+0A`, `+0C` — the x this port publishes for the patched cartridge to
read, and the struct's own current x and y. The two deliveries put the same
point in front of the game, to a pixel. One selected and one did not.

So the difference was never in what arrives while the pen is **down**. It is what
arrives when it **lifts**.

The debug server's `touch` command releases at the same x and y it pressed at.
The enhanced mapping released like this:

```cpp
if (!down || <outside the field rect>) {
    lb_touch_clear_extended();
    nds_set_touch(0, 0, false);      // <-- at the origin
    return;
}
```

The cartridge acts on the release edge, so every click through the frontend was
read as a tap at the top-left corner of the screen. Not "dropped", which is what
every previous investigation assumed — *delivered, at the wrong place*.

## The fix, and the second bug it caused

A release is now delivered at the position it was released at, and the extended
coordinate is kept across it — the extended coordinate is the entire out-of-band
feature, and clearing it on release would hand the game the *clamped* x on
exactly the frame it reads one, turning a click in the widened band into a click
at native 0.

Keeping it broke the other delivery immediately: the next raw `touch` was
measured arriving as **−14**, the x of the deselect click before it, instead of
the 147 it asked for. A published extended coordinate outlived the click it
belonged to.

The rule that fixes both: **an extended coordinate is good for exactly one
delivery.** `lb_touch_set_extended` marks it fresh; `lb_touch_note_native`, which
every `nds_set_touch` runs through, clears it if the delivery did not bring one.
The release path re-asserts its own, so it survives; the debug `touch` command
brings none, so it gets the native x. Measured after: `tap=(123, 123, 63)`,
`frontend=(122, 123, 63)`, and the frontend click **selects**.

## A probe fault worth recording

The calibration loop skips an offset whenever the deselect leaves something
selected. Its `deselect()` clicked surface x 55, which is neither place that icon
exists — the cartridge's hit test has it at native 4..28 (surface 72..96) and
this port draws it at surface 4..28 after anchoring. 55 falls between them.

So every deselect failed, every offset was skipped, and the run reported "no
frontend offset selects the hero" **without having tried one**. That is what
docs/222 recorded as "twelve calibrated offsets all failed", and it was wrong for
this reason. `--build-flow` had already measured the icon at native (4,168)-(28,192);
tapping its centre takes the quad count from 7 to 0.

## RETRACTION: docs/218 is wrong

With the control working, the comparison docs/217 and docs/218 were both trying
to make can finally be made. Same unit — the **builder**, which stands still,
unlike the hero that walks and confounded every earlier attempt — same camera
row, same delivery, same offset, one camera position:

```
control  CLICK+(0,0) at (123,63)    hud 0 -> 6  SELECTED
band     CLICK+(0,0) at (-37,63)    hud 0 -> 0  nothing
         band delivered slot = -38
```

The coordinate **is** delivered out of band — the slot holds −38 — and the game
does not act on it.

docs/218 reported "4/12 select" for out-of-band clicks and `lb_patches.cpp` cites
that to justify defaulting the touch extension on. That result was the clamp
artifact docs/218 itself warned about: its target was the **base**, whose hit area
covers native x 0, and a click that clamps to 0 hits it no matter where it was
aimed. The measurement was real; the conclusion drawn from it was not.

**The extension stays on.** It is not doing harm — the control above proves in-band
behaviour is unchanged — and delivering the true coordinate is a precondition for
any fix. What changes is the claim: out-of-band clicking is **not** working, and
docs/218's comment in `lb_patches.cpp` has been corrected in place.

## Also refuted here: the drag control

docs/222 concluded that a delivered drag around a unit selects nothing in the
native aspect either. One run after this fix showed an in-band drag selecting
(hud 0 → 5) and the next, same build, showed nothing — the difference being where
the **hero** had walked to between its position being read and the drag being
made. That is the same confound, so drag-select is back to unmeasured, and the
probe now targets the builder for this reason.

## What is next

docs/217's question, now with a control that works: `func_020a8794` reads the
touch x twice — `020A8800` is `ldrsh` and feeds the rectangle builder, `020A8812`
is `ldrh`, zero-extends, and stashes at `[obj+0x20]`. A negative x survives the
first and becomes 65498 through the second. Find what reads `[obj+0x20]` back.
