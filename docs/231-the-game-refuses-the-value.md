# 231 — The game refuses the value, not the delivery

**Goal within the project:** interaction in the widened view — a click in the
widened band selecting a unit, as one inside the native aspect does.

docs/228 ended by saying the automated probe could not settle this. That is no
longer true, and the reason it was true is worth stating precisely: **the
experiment was fine and the detector was not.**

## The detector

Every earlier attempt inferred "selected" from the in-field HUD's quad count.
That detector needs the HUD to have been built and drawn, reads zero whenever
anchoring is off or the window is narrow, and gave four different answers to
the same question across four runs (docs/228's table).

There is a direct one. `FUN_020e2a04` iterates a list at `DAT_020e2a88 + 0x64`
with its count at `+0x68`. `DAT_020e2a88` is `0x02157FC4` in the EUR build —
read out of `out/eur/arm9/section_static_02000000.bin`, not guessed; the USA
image has different code at that address, which is worth knowing before anyone
reuses these numbers.

So the count is at **`0x0215802C`**. `--selwrite` confirms what it is by
comparing two clicks rather than reasoning about one:

```
empty ground (20,20)  -> 020EC7E0, 020EC808, 020EC810 all write 0
base         (123,15) -> ...the same three, PLUS
                         020EC8F0 writes 1        (the count)
                         020ECADC writes 0x02307FE4 (the list pointer)
```

**This retracts docs/228's dismissal** of that address as "a temporary vector
being constructed, not the set of entities under the touch". It looked that way
because the watch was recording reads as well as writes and there was no
empty-ground control to compare against.

"Did that click select something" is now a two-byte read. No HUD, no quad
census, no watch ring to drain, nothing inferred.

## The result the detector makes possible

`--bandselect` parks the player's base — stationary, large — at a known screen
x and clicks it, with a verified clean start before every attempt:

```
in band    x  60..190  -> SELECTED 4 of 4 clean attempts
left band  x -112..-16 -> SELECTED 0 of 4 clean attempts
```

and `--bandsweep` walks it leftwards a step at a time:

```
x~48  2/2      x~24  2/2      x~12  2/2      x~4  2/2
x~-4  0/2      x~-12 0/2      x~-24 0/2      x~-48 0/2      x~-80 0/2
```

**The cutoff is exactly screen x = 0.** Not approximately: `x~4`, whose park
window is −2..10, selects both times, and `x~-4`, whose window is −10..2, never
does. That is a sign or bounds test on the coordinate, and it is not about the
target, the band, the camera or the window size.

## Which half is broken

A band click could fail two quite different ways, and the count cannot tell
them apart: the game refuses the **coordinate**, or the band click never
arrives as a proper pen-down at all — a flag, a drag threshold, a clamp
somewhere in delivery. Those want opposite fixes.

`LB_TOUCH_FORCE=<n>` makes the port publish a fixed extended X while the pen is
down. So park the target at screen x = n and click **inside the native panel**,
where the delivery is known good, while the value the game sees is the
out-of-band one:

| published x | target parked at | click at | selected |
|---|---|---|---|
| +120 | x 114..126 | native 128 | **2 of 4** |
| −40  | x −46..−34 | native 128 | **0 of 4** |

Same delivery path, same click position, same target, same protocol. Only the
published coordinate differs, and the positive control proves the forcing
mechanism itself works.

**The game refuses the value.** Delivery is not the fault — which is consistent
with docs/230, where all 66 sampled columns across six window sizes delivered
the correct coordinate to the cartridge's own touch struct.

## What was excluded on the way

* **`LB_VIEW_WIDTH`.** docs/230 named `Set_View_Dimensions` as the next
  hypothesis: it is the third of docs/197's three mechanisms that clamp to the
  256-column view, and the only one still at `mov r1, #0x20`. Widening it to
  512 leaves the band at 0 of 4 **and degrades the in-band control to 2 of 4**,
  so it is neither the guard nor harmless. Recorded so it is not retried.
* **`FUN_0207eef8`, the world-space entity query.** It does clamp — `if (x < 0)
  x = 0;` and the same for y — but on **world** coordinates, after the camera
  has been added. With the camera mid-map a screen x of −40 is a world x in the
  thousands, so those clamps cannot be the cutoff. Also recorded rather than
  left as a plausible-looking suspect.

## A tool change that made the search tractable

A watch event named the PC that performed the access, which is repeatedly the
wrong granularity. The selection count is written from `020EC8F0` — inside a
generic vector push with **thirty-one** static callers — so "who writes the
selection" had a definite answer that neither the watch nor cross-references
could give.

The link register was sitting in the same register file the PC is read from,
and on ARM a `BL` leaves the caller's return address in it. Watch events now
carry it:

```
w pc=020EC8F0 lr=020E2797 n=4
w pc=020EC8F0 lr=020E296B n=4
w pc=020ECADC lr=0210E95C n=4
```

Both callers are inside one 1442-byte Thumb function at **`020E2408`**, which
Ghidra had not identified as a function at all. That is the click handler:

```c
iVar13 = (int)*(short *)(*DAT_020e273c + 0x5e);   // camera x
iVar14 = (int)*(short *)(*DAT_020e273c + 0x60);   // camera y
FUN_020f2a50(auStack_148, iVar13, iVar14);
local_134 = local_134 + sStack_144;               // corner + camera -> world
local_132 = local_132 + sStack_142;
...
FUN_020881e0(auStack_58, auStack_138, auStack_140, uVar7);   // query the world rect
```

`runtime_trace` already captured LR for its own entries on exactly this
reasoning; the watch ring simply never did.

## What is left

The guard is a test on the screen coordinate, somewhere between the event being
posted and `020E2408` building its world rect. The band click produces **no
watch events at all** on the selection range, so the refusal is upstream of the
append rather than a filter after it.

The next measurement is to watch the entity-pool reads during both clicks: if a
band click reads no entity records, the spatial cell lookup was handed the
wrong cell; if it reads them and keeps none, the rectangle test rejected them.
Those are different bugs in different functions, and one watch run separates
them.
