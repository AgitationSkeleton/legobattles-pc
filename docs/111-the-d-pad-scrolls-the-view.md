# The D-pad scrolls the view, and the registers were never going to show it

> **Corrected by [docs/114](114-the-engines-were-inverted.md).** The table below
> naming engine A as the top screen is inverted: POWCNT1 bit 15 is clear, so
> engine A drives the BOTTOM screen and draws the field. The D-pad finding is
> unaffected -- it was measured on screen 1 regardless of which engine draws it.

## The finding

Holding a direction and measuring the **pixel shift** of the field between
captures, by correlating the engine B framebuffer against a baseline:

```
hold Left:                       hold Up:
  after  4M cycles: dx= -1 dy=+0   after  4M: dx=-25 dy= -4
  after  8M cycles: dx=-15 dy=+0   after  8M: dx=-25 dy=-28
  after 12M cycles: dx=-47 dy=+0   after 12M: dx=-25 dy=-60
  after 16M cycles: dx=-67 dy=+32  after 16M: dx=-25 dy=-60
```

Monotonic, on the correct axis, with the other axis pinned, at 92-100% pixel
match. The `dx=-25` during the Up hold is the residue of the preceding Left
hold, which is exactly what a persistent camera should do.

**The D-pad scrolls the field view.** `docs/109` said so originally, then
retracted it; that retraction was itself wrong and is hereby withdrawn. The
original observation stands.

## Why four separate sweeps said "nothing scrolls"

Two independent mistakes, and they happened to agree.

**1. The wrong engine.** The game field is on **engine B**. Engine A is the
*top* screen -- the unit portrait, the minimap, gold and population. Confirmed
by capturing both and looking at them, which is what should have happened at
the start. This session repeatedly watched `04000010..0400001F`, which is
engine A's scroll block; engine B's is at `04001010`.

**2. The wrong instrument.** Watching engine B's registers as well changes
nothing, because they are constant too:

```
idle / Right / Left / Up / Down / L / R / drag:
    BG1=(0,0)  BG2=(0,0)  BG3=(376,472)     -- identical under every input
```

The field's BG scroll registers **do not move when the view scrolls**. Scrolling
is implemented by **rewriting the tilemap**, exactly as `docs/109` measured with
its 32-column refresh. So no scroll-register measurement could ever have
detected scrolling, on either engine, and the four techniques spent on this
were all measuring a quantity that is constant by design.

The lesson is narrow and worth keeping: *a register that holds still is not
evidence that the thing it names holds still.* Verify the instrument against a
known-positive before trusting a negative from it. One screenshot at the start
would have shown the field was on the other engine.

## What is now available

* **A way to move the view**: hold a D-pad direction. Roughly 4 pixels per
  million cycles, so ~2 pixels per frame.
* **A way to measure it**: correlate successive engine B framebuffers for the
  best `(dx, dy)`. At 92-100% match on a pure translation this is unambiguous,
  and unlike the register watch it measures the thing actually asked about.
* Buttons reach the simulation normally: A, B and X each change ~99% of the
  bottom screen, L changes nothing, Y opens something that is not a translation
  (2% match under any shift).

## Correction to this session's negatives

Every "no input scrolls the view" result recorded earlier -- the button sweep,
the drag tests, the screen-edge touch tests, and the D-pad retraction in
`docs/109` -- was measured through the register instrument and is void. They are
not evidence of anything. The tilemap findings in `docs/109`, which were
measured through watches on the tilemap itself rather than through registers,
are unaffected.

## Next

The camera position can now be found the straightforward way, which failed
before only because nothing was ever moving: diff main RAM across a D-pad hold
and keep the words that change monotonically with the measured pixel shift.
