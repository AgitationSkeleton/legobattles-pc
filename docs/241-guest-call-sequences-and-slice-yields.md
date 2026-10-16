# 241 — Why a five-call guest sequence cannot be issued from an override

**Goal within the project:** bug-fix toggles around the
hotwire-through-a-vehicle bug. Hotwire-transport **mode 1, "destroy
the occupants"**, was implemented on 2026-09-07 and `docs/239` flagged it as
compiled-but-never-run. Testing it produced a clear, reproducible failure and a
real piece of understanding about this runtime.

## What was observed

A self-test issues one `WaitingToDieCommand` for a live enemy unit. Across three
runs it failed identically:

```
[lb_killtest] issuing WaitingToDie for 022DDA70 (cat 0), pool=20 commands=29
[lb_call]     UNWOUND at push (020EC8DC)
[lb_killtest] returned UNWOUND, commands 29 -> 29 (+0), pool 20 -> 20
```

Step 1 (`kArrayInit`) always succeeds. Step 2 (`push`) always unwinds. Never a
different step, never a success.

## What "UNWOUND" actually means — read, not assumed

From `runtime_arm.h`:

> `runtime_slice_yield()` — *"Cooperative slice-yield signal, checked **ONLY at
> backward branches** (loop tops, which are dispatch entries). Lets the scheduler
> preempt a guest spin to run the other core, with a clean re-dispatch on resume.
> When it returns true it also arms `runtime_unwinding()`."*
>
> `runtime_unwinding()` — *"True while the host stack is unwinding for a
> slice-yield (**not a real guest return**)... the call-return stack is
> saved/restored per-CPU across the preemption, so **the guest return
> survives**."*

So an unwind is **not a failure**. It is cooperative preemption, and the
interrupted guest call resumes later.

That also explains why it is deterministic here. `func_020EC8DC` opens by calling
`func_020ECA5C`, a grow/reserve — a **loop**, therefore a backward branch,
therefore a yield point that is hit on essentially every call. Retrying cannot
help: every attempt reaches the same loop and yields at the same place.

## Why this matters beyond one setting

**A multi-call guest sequence issued from inside an override is structurally
fragile in this runtime.** Any callee containing a loop can yield, the host frame
unwinds, and a C function that was half-way through a five-step sequence loses
its place — while the guest call it started is still pending and will resume.
State the caller holds (a stack pointer it moved, a vector it initialised) is
then out of step with what the guest does next.

The occupant sweep beside it survives this because it makes **one** call per
occupant and is explicitly re-entrant: it bails on unwind and lets re-entry redo
the sweep, "which is a no-op for occupants already moved". Five chained calls
have no such property.

## The honest status of mode 1

**BUILT, and not reliable as written.** The recipe is right — it was read from a
live call site (`func_02046530`) and independently matched by the move issuer
(`func_020A8F28`), and `func_020EC8DC`'s argument convention was verified from
its own disassembly (handle in r1 as a 32-bit value, spilled and read as two
bytes). What is wrong is the *delivery*: five chained guest calls from an
override cannot complete atomically.

## The way forward, in preference order

1. **Reduce it to one guest call.** Build the command object in guest memory
   directly — the layout is largely known (`+0x14` container, `+0x18` data,
   `+0x1C` count, tag `0x17` via `CommandBase`) — and call only the submit,
   `func_020EB910`. One call, no chain to lose.
2. **Make the sequence resumable.** A small state machine that records which of
   the five steps completed and continues from there on the next override entry,
   the way the occupant sweep is re-entrant.
3. **Do not** simply retry. Three runs show the yield is deterministic, not
   sporadic.

## What was corrected along the way

Four faults, all in the test rather than the game, each recorded because each
looked like a result:

* the test ran from a **vblank hook** with no guest call stack, so the first call
  unwound at step 1 — nothing to do with the kill path;
* its forward declaration sat **inside the anonymous namespace** while the
  definition was outside, which the linker reported only as `ld returned 1 exit
  status`;
* it treated the **first partial attempt as final**, when the sweep beside it
  already assumes unwinds are normal;
* and it had **one opportunity per run**, because `set_owner_override` fires on
  entity spawn and a passive Free Play battle spawns about one unit every few
  minutes.
