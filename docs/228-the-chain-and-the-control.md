# 228 — The selection chain, and why the probe cannot finish it

**Goal within the project:** a click
in the widened band selecting a unit, as one inside the native aspect does.

This records two things: how far the chain was traced, and why the automated
probe cannot close it. The second is the more useful of the two.

## The chain, as far as it goes

Each link measured, not inferred:

1. **The coordinate arrives.** The enhanced mapping publishes the true x, and it
   reaches the cartridge's own touch struct: `0215376A pc 0203DD1C` writes
   `-38` for a click at native −37 (docs/224).
2. **Every reader sign-extends.** A read watch on `0215376A` names all of them:
   the three this port already patches, plus `020A8800` (`LDRSH`) and
   `020A8812` (`LDRH`, whose zero-extension is harmless because the value goes
   straight into a `STRH` that truncates to the same bits) (docs/225).
3. **The selection rectangle is built correctly.** `func_020AA8E4` uses `LDRSH`
   throughout and writes signed values to `+0x10`/`+0x18`.
4. **The entity IS in the gathered list.** The gather rect was never being
   widened — a real bug, fixed — and fixing it did not make an out-of-band click
   select (docs/226's A/B).
5. **A successful selection writes the entity pointer exactly once**, at
   `pc 02027330`, inside `FUN_020272c0`. That routine walks a set and appends a
   node per entry: `puVar4[2] = iVar3` is the write the watch caught.
6. **The set comes from `FUN_020e2a04`**, which is *not* a coordinate test. It
   iterates a list the caller already holds, filters by ownership, and caps the
   result at 8 — the selection cap.

So the coordinate decision is upstream of (6), in whatever fills that list.

**A dead end recorded so it is not retried:** `DAT_020e2a88` resolves to
`02157FC4`, putting a candidate count at the static `0215802C`. Watching it
showed `0 → 1` by `pc 020EC8F0` on *every* frame, including clicks that selected
nothing. It is a temporary vector being constructed, not the set of entities
under the touch.

## Why the probe cannot finish it

Every step above needed a **control**: a click that reliably selects in band, so
that a band failure means something. That control has never become reliable, and
the failures are not random noise but four distinct instrument faults, each
found only because its output was impossible:

| fault | how it showed |
|---|---|
| the watch never drained | `bus_debug_watch_copy` copies without resetting the count, so a value-filtered watch returned keypad writes that predated the filter |
| reads recorded as writes | the default records both, so "written by pc X" named a function that only *reads* the field and compares it |
| sampling skew read as divergence | in the MP sampler, the instance read first is older; 7 of 18 "divergences" were that |
| stale target position | `park()` releases a key and the camera keeps drifting, while the position log prints about once a second |

Each was fixed. The control still failed, in a different way each run:

```
sweep picked dy=+12, trials 4 of 10       sweep picked dy=-12, trials 0 of 3
sweep picked dy=+36, trials 0 of 3        sweep found NO offset at all
```

Identical code, four different answers about where a stationary target's
clickable pixels are. The builder's band is about ten native pixels tall, so
that target was always marginal — but the **base**, stationary and roughly 46×57,
failed the same way, which rules out target size as the explanation.

## The honest conclusion

**The synthetic click path is not accurate enough to support this measurement.**
Not "out-of-band clicking is broken" — that would be a claim about the game, and
every band result rests on a control this unstable.

What is safe to say is narrower and still useful: with a control that did fire,
in band selected 4–7 of 10 while the left band selected **0 of 10** across
several runs, with a clean clamp control. That is real evidence something is
different about the band. It is not enough to name a cause, and this project has
already retracted one conclusion (docs/218) drawn from exactly this kind of
evidence.

## What finishes it

The demo recorder (docs/221), which exists for precisely this. A recording
replays the player's real inputs frame-exactly, so it has no aim offset to
calibrate, no camera to settle, and no synthesised event to go astray. One
recording of a tap on a unit out in the widened band answers in a single replay
what six rounds of probes could not:

* if it selects, the fault is in the synthetic path and every band result above
  is void;
* if it does not, there is finally a repeatable case to single-step, with the
  chain above already traced to link 6.

`demos/route-free.lbdemo` is committed as a working fixture, so a recording made
once replays against every future build.
