# 225 — The left band does not select, and the right one is unproven

**Goal within the project:** the widened touch area being just as
interactable as the original aspect. This
records what is now measured, what is not, and the three ways this probe was
lying before it was fixed.

## The probe had to be fixed three times first

Every one of these produced a plausible number that meant nothing.

**Deep trace changes the answer.** The probe armed `deep_trace` so it could drain
the watch ring at the click. Tracing slows the guest, and a tap is delivered in
wall clock, so a 0.25 s press is a different number of guest frames with tracing
on than off. The in-band control selected **1 of 3** with tracing on and **3 of
3** without. Tracing is now opt-in (`--cw-trace`) and the comparison runs clean.

**A long press is not a tap.** Before that, the hold was a full second, on the
reasoning that longer is safer. The control selected 0 of 3. `--drag-unit`'s
click, which holds 0.2 s, selects reliably on the same builder at the same
coordinates.

**A dirty start scored as a failed click.** The success test is "the quad count
rose from below 3 to at least 3", so an attempt whose deselect did not take
begins already selected and is counted as a failure even when the click was
fine. Those attempts are now reported as `DIRTY START` and excluded from the
denominator.

None of these were visible from the outside. All three made the *control* fail,
which is the only reason they were caught at all — a probe with no control would
have reported them as facts about the band.

## The clamp control

A click outside the DS panel is also delivered **clamped** to the panel, so a
target near the boundary can be hit by the clamped copy no matter what the true
coordinate does. That is the artifact that made docs/218 wrong.

So every out-of-band trial now runs a second trial at the clamp point — native 0
for the left band, 255 for the right — with everything else identical. If that
selects too, the trial proves nothing.

## What is measured

The target is the **builder**: stationary, and a unit rather than a structure.
One camera position per trial, same delivery, same offset.

| trial | native x | selected | clamp control |
|---|---|---|---|
| in band | 123 | **3 of 3** | — |
| left band | −37, −38, −52 | **0 of 3, every run** | 0 of 3, clean |
| left band (base) | −39 | 0 of 3 | 0 of 3, clean |
| right band | 259 | 0, 1, 3 of 3 across runs | 0, 1, 2 of 3 — **confounded** |

**The left band does not select.** That is consistent across every clean run,
against two different targets, with a clamp control that never fires. It is not
a probe artifact.

**The right band is unproven either way.** The builder cannot be parked past
native 259 — the camera runs out of map first — and 259 is four pixels past the
clamp point of 255, close enough that the clamped copy sometimes hits it. The
runs where it "worked" are exactly the runs where the clamp control also fired.
A deeper target is needed and this map does not offer one.

## What the coordinate does on its way in

All measured here, so the delivery chain is not where the fault is:

- The enhanced mapping publishes the true x — `slot = -38` for a click at native
  −37 (docs/224).
- It reaches the cartridge's own touch struct: `0215376A pc 0203DD1C` writes
  `[16, 16, 16, -38]`.
- Every instruction that reads `0215376A` is now known, from a read watch:
  `0203DE4C`, `0203DE9C`, `0203DEFC` — the three this port already patches to
  sign-extend — plus `020A8800` (`LDRSH`, already signed) and `020A8812`
  (`LDRH`).
- `020A8812`'s zero-extension does not lose anything: the value goes straight
  into `STRH r0, [r5, #0x20]`, which truncates to 16 bits either way. The bits at
  `[r5+0x20]` are identical whichever load is used.
- `func_020AA8E4`, the rectangle builder, uses `LDRSH` throughout and writes
  correct signed values to `+0x10`/`+0x18`.

So the coordinate arrives, intact and correctly signed, everywhere this port can
see it. Something further in rejects it.

## Two dead ends, recorded so they are not repeated

`0x0200A350` and `0x020099E4`, which docs/214 suspected of being the bounds
check, are ARM function prologues (`E92D4070`, `E92D4038`). Nothing about them
says bounds check.

`LB_WATCH_VAL=122` over all of main RAM found every address that receives the
touch x. The only interesting one, `023116AA` written from `pc 021109BC`, is
`LDRB r5, [r1, -r12]` inside a byte-copy loop — a buffer that happens to contain
122, not the touch coordinate.

## How this gets settled

The evidence is consistent with a bounds test on the touch x somewhere in the
hit-test chain, but that is a hypothesis and it has not been located, and this
project has published three wrong conclusions from evidence this consistent.

The demo recorder round-trips now (docs/221). **A recording of a player tapping a
unit out in the widened band settles it in one step**: if it selects for a real
click, the fault is in the synthetic path and the replay shows exactly where; if
it does not, the replay is a repeatable case to single-step.
