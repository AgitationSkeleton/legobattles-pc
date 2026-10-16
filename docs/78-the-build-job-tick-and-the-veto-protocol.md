# The build job's tick, and what the twelve codes are

**Status: `JobContractBuild`'s tick is a five-state machine on `job+0x18`, and
reading it names `docs/74`'s twelve event codes. They are proposed **squad
modes**, and slot 0 is a **veto**: a running job is asked whether the squad may
switch to mode N before the switch happens.** Annotation 54.3% → 54.4%; **111 of
111 checks pass**.

## The tick

```c
void JobContractBuild::Tick(void) {
    this->ticks_30++;                      /* u16, every tick, unconditionally */
    if (this->state_18 > 4) return;
    switch (this->state_18) { /* five arms */ }
}
```

`docs/77` established states **1 active, 3 cancelled, 4 dead**. The tick adds
**0** (the entry state) and **2**, and writes 1, 2 and 4 itself — so the machine
is five-valued and the job drives most of it from here.

Two counters, and they are different things:

```
job+0x30   u16  ticks, incremented every single tick, zeroed on a state change
job+0x36   u16  attempts, incremented on failure, compared against 2
```

`+0x36 > 2` is the give-up test, so **three attempts** before the state advances.

## The cadence is a modulo

```asm
ldrh r0, [r4, #0x30]      ; the tick counter
movs r1, #0xa             ; 10
blx  #0x211fdb4           ; __aeabi_idivmod -> quotient r0, remainder r1
cmp  r1, #0
bne  skip
bl   func_0208FDF4        ; the periodic work
```

State 0 does its periodic work when `ticks % 10 == 0`; state 2 uses `% 20`. At
the 30 Hz lockstep tick rate (`docs/25`) that is **every third of a second** and
**every two thirds of a second**.

`func_0211FDB4` is `__aeabi_idivmod` — signed, sign handled by
`eor`/`and #0x80000000` up front, quotient in `r0` and remainder in `r1`. **115
call sites in the ARM9.** Worth naming once: any Thumb code doing division in
this game goes through it, and a decompilation has to emit `%` and `/` in a way
that produces this call rather than an inline sequence.

## The veto protocol

The heart of the state-0 arm:

```c
squad = this->squad_4;
if (squad->mode_10 != 7 && squad->mode_10 != 1) {
    if (squad->job_38) {
        u8 proposed = 7;
        if (squad->job_38->vtable[0](squad->job_38, &proposed)) {
            squad->prev_mode_14 = squad->mode_10;
            squad->mode_10      = proposed;
            squad->counter_C    = 0;
        }
    } else {
        squad->mode_10   = 7;          /* no job to ask */
        squad->counter_C = 0;
    }
}
```

That call is **`docs/74`'s slot 0**, invoked with a byte code — and the code is
the value about to be written to `squad+0x10`.

**So the twelve codes are squad modes, and slot 0 is a veto.** The sequence is:

1. Something wants the squad in mode N.
2. The squad's **currently running job** is asked, through slot 0, whether that
   is acceptable.
3. Only on a non-zero answer does the mode actually change.
4. A job with no owner job attached skips the question entirely.

This retro-explains `docs/74` completely. Its matrix of which codes each job
returns 1 for is a **matrix of which squad modes each job will tolerate while it
runs**, and the invariants read properly now:

* **Code 0 is refused by every job** — no running job will ever allow mode 0, so
  mode 0 is only reachable on a squad with no job.
* **Codes 2, 7 and 10 are allowed by every job** — universally safe modes.
* **Code 3 is conditional in all six** — the one mode whose acceptability depends
  on the job's own state.
* **Codes 8 and 9 are allowed only by the two attack jobs**, so those modes are
  reachable only while attacking.

The build tick proposes **7** in state 0 and **2** in state 2 — both in the
"every job allows it" set, which is why `JobContractBuild`'s own vetoes never
block its own transitions.

## The squad mode fields

```
squad+0x0C  u16  counter, zeroed on every mode change
squad+0x10  u32  current mode          (the twelve codes)
squad+0x14  u32  previous mode         (saved on change)
squad+0x38  ptr  current job           (docs/77)
```

The previous mode is only written when the change is **accepted**, so a vetoed
proposal leaves no trace — a port that saves the previous mode before asking
would diverge.

## The guard, a third time

`if (mode != 7 && mode != 1)` before proposing 7. **Do not re-propose a mode the
squad is already in**, and treat 1 as equally settled. That is the third guarded
transition in this codebase after `Objective_MarkComplete`'s `!complete`
(`docs/71`) and the squad destructor's `!= 4` (`docs/77`).

Three subsystems, three authors' worth of code, and the same discipline: **check
before transitioning**. For a port it means these are not idempotent by
accident — they are idempotent on purpose, and dropping the guard changes
observable behaviour (an extra event, an extra saved previous-mode, an extra
notify).

## Tips and pitfalls

* **A jump table on a state field is worth decoding before reading the body.**
  Five arms told me the machine's size before I read a single branch.
* **Two `u16` counters in one object are rarely the same kind of thing.** `+0x30`
  counts time, `+0x36` counts failures, and they are seven bytes apart.
* **A modulo against a tick counter is a cadence, and the divisor is a design
  constant.** 10 and 20 ticks, a third and two thirds of a second.
* **Identify the compiler runtime once.** `__aeabi_idivmod` has 115 call sites;
  every one of them was noise in earlier call-graph work.
* **A predicate called with a value that is then stored is a veto, not a
  query.** The give-away is that the same byte is the argument *and* the value
  written on success.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **54.4%** | 2,501 listed + 2564 derived, 124 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
