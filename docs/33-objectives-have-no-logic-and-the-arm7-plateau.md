# Two negative results: objectives carry no logic, and the ARM7 has plateaued

**Status: both findings are negatives, and both are more useful than the
positives they replace.** `docs/32` named the thirteen objective classes; this
looks inside them and finds that the classes contain almost nothing. And the
ARM7's unreached 30%, open since `docs/17`, is now characterised even though it
is still unreached.

## An objective's vtable holds no completion logic

The obvious next question after `docs/32` was *how does each objective decide it
is complete?* `ObjectiveBase` has 3 vtable slots and every concrete objective
has 5, so slots 3 and 4 looked like the answer.

They are not. Reading them:

```arm
Battles::ManualCompletionObjective::virtual_03:   movs r0, #0 ; bx lr
Battles::ManualCompletionObjective::virtual_04:   movs r0, #0 ; bx lr

Battles::TimerObjective::virtual_03:              adds r0, #0x18 ; b func_0204B75C
Battles::TimerObjective::virtual_04:              adds r0, #0x18 ; b func_0204B750

Battles::DestroyMarkedUnitsObjective::virtual_03:
    r0 = this + 0x20 ; if (func_0204B75C(r0) == 0) func_0204B7D4(this + 0x24)
```

and following the helpers, they are four generated thunks over two bases:

```
func_0204B750(x) = func_0204B830(x, 1)      func_0204B7C8(x) = func_0204B830(x, 4)
func_0204B75C(x) = func_0204B854(x, 1)      func_0204B7D4(x) = func_0204B854(x, 4)
```

The `1` and `4` are **element sizes**, and both bases do the same thing: test a
container, and if it is not yet allocated, add the size to a running total at
`[0x02154468 + 4]` and grow it.

**Slots 3 and 4 are memory accounting**, not gameplay — the up-front budgeting
pass a DS title does before committing an allocation. `ManualCompletionObjective`
returns zero because it owns no containers; `TimerObjective` reserves for one at
`+0x18`; `DestroyMarkedUnitsObjective` reserves for two, at `+0x20` and `+0x24`,
one of 1-byte elements and one of 4-byte.

And slot 2 is worse than uninformative:

```arm
Battles::ObjectiveBase::virtual_02:  bx lr
```

A single `bx lr`, shared by all thirteen subclasses — none overrides it.

So of an objective's five vtable slots, **two are destructors, one is a no-op,
and two are memory accounting. There is no virtual that evaluates completion.**

### What that means

**Objectives are passive.** They do not poll the simulation and they contain no
condition logic. Something else decides when one is satisfied — and the obvious
candidate is the event machinery `docs/31` found: `func_02045898` dispatches map
events by id through `Battles::Map::GetEventWithId`, and `docs/15` recovered
`EVNT` and `TRIG` chunks in every map file.

That reading is **not yet demonstrated** and is recorded as a hypothesis. What
*is* demonstrated is the negative: the completion logic is not in these classes.

This is worth stating plainly because the inviting mistake was right there. Five
slots, two of them unique to each subclass, sitting on classes called
`DestroyMarkedUnitsObjective` and `TimerObjective` — "slot 3 is `IsComplete`"
writes itself. It would have been wrong, and nothing about the vtable shape says
so. Only reading the functions does.

## The ARM7 has plateaued at 69.5%

`docs/17` reached 69.5% of the ARM7 and left the remaining 30% open, and
`docs/18` eliminated the IPC handler table as the explanation. Re-running every
technique the scanner has:

| run | functions | coverage |
| --- | ---: | ---: |
| `--auto --scan-data` | 304 | 67.4% |
| `--auto --scan-data --expand` | 330 | **69.5%** |
| the existing corpus | 334 | **69.5%** |

**Exactly the same figure.** Call-graph walking, literal-pool code pointers and
a full data-pointer scan converge on the same 69.5%, which means the corpus was
already at the limit of what these methods can reach. The tier-3 scan found only
108 words anywhere in the image pointing into code, of which 42 open with a
plausible prologue — a thin seam, already mined.

Broken down by section, the picture is sharper than "30% missing":

```
static  0x02380000..0x023801E8    484 of 488 bytes    99.2%   (one 4-byte gap)
wram    0x037F8000..0x038074B0  43,408 of 62,640     69.3%   (222 gaps)
```

The gaps are numerous and small — 222 of them, averaging ~87 bytes — with the
largest around 1.5 KB.

### What is in them

Not padding. Reading into the largest gaps finds literal pools followed by real
ARM function prologues:

```
0x038043A4:  push {r3, lr} ; mov r0, #0x8000 ; bl #0x37fe5a8
0x038053A8:  push {r4, lr}
```

So the unreached bytes are **code that no reachable path calls**. Walking
forward from three of those entries:

| entry | functions reached | bytes | already in the reached set |
| --- | ---: | ---: | ---: |
| `0x038043A4` | 27 | 2,976 | **92%** |
| `0x038053A8` | 6 | 1,084 | 36% |
| `0x03802974` | 1 | 96 | 0% |

The first is the telling one. An unreached entry point whose walk spends 92% of
its bytes in *already-known* code is not a hidden subsystem — it is **a function
nobody calls that itself calls the live code**. That is exactly the shape of a
library entry point linked in and never used.

### The reading, and its limits

The ARM7 is where the SDK lives (`docs/14`): sound, touch, SPI, Wi-Fi. DS titles
link the ARM7 SDK statically and the linker does not strip unreferenced
functions, so a large fraction of unreachable-but-valid code is the expected
outcome, not an anomaly. Three things point that way together: every technique
plateaus at the same number, the gaps hold well-formed functions, and the
unreached entries call *into* the live code rather than forming separate
machinery.

**This is a hypothesis, not a proof**, and is labelled as one. It could
still be reached by a mechanism not modelled here — most plausibly a function
pointer written into ARM7 memory by the ARM9. `docs/18` checked the IPC handler
table and eliminated it, but that is one mechanism, not all of them.

What changes practically is the target. **69.5% is probably close to the ceiling
for "code this game runs"**, and chasing the remaining 30% is likely chasing
dead library code. `docs/30`'s "what would move each number" row for the ARM7 is
updated accordingly: the honest next step is not more scanning but identifying
which SDK libraries are linked in, which would let the dead regions be labelled
rather than reached.

## Pitfall: a plateau is a result, and it needs the control

Running the scan three ways and getting 69.5% twice is what makes this a finding
rather than a shrug. A single run stalling at 69.5% says nothing — it could be a
seed problem, a mode problem, or a bug. Three techniques with different failure
modes converging on the same number is evidence about the *binary*, not about
the tool.

The same discipline caught the opposite case in `docs/32`, where a figure that
looked fine turned out to be measuring the wrong quantity. Agreement between
methods is the check in both directions.
