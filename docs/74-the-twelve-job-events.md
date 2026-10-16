# The twelve job events

**Status: the AI's slot-0 predicate is decoded for all six jobs. There is a
shared twelve-value event vocabulary, and the six jobs agree on five of the
twelve codes — one never claimed by anyone, three always claimed, one always
conditional.** 103 of 103 checks pass. Annotation unchanged at 54.1%: every
function here was already named from the RTTI symbols, and this pass says so
rather than inflating a tier.

## The matrix

`docs/73` identified slot 0 as a predicate over a byte code without decoding it.
Decoding all six:

| job | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
| --- | --: | --: | --: | --: | --: | --: | --: | --: | --: | --: | --: | --: |
| `JobAttack` | 0 | 1 | 1 | *c* | 1 | 1 | 0 | 1 | 1 | 1 | 1 | 1 |
| `JobAttackTaskForce` | 0 | **3** | 1 | *c* | 1 | 1 | 0 | 1 | 1 | 1 | 1 | 1 |
| `JobContractBuild` | 0 | 0 | 1 | *c* | 0 | 0 | 1 | 1 | 0 | 0 | 1 | 1 |
| `JobDefend` | 0 | 1 | 1 | *c* | 1 | 1 | 1 | 1 | 0 | 0 | 1 | 1 |
| `JobRepair` | 0 | 1 | 1 | *c* | 1 | 0 | 0 | 1 | 0 | 0 | 1 | 1 |
| `JobScout` | 0 | 0 | 1 | *c* | 1 | 1 | 1 | 1 | 0 | 0 | 1 | *c* |

*c* = conditional, the arm calls something rather than returning a constant.

**Five codes have unanimous answers across all six jobs:**

```
code 0    every job returns 0        nobody ever claims it
code 2    every job returns 1        everybody always claims it
code 3    every job is conditional   the one code that needs thinking about
code 7    every job returns 1
code 10   every job returns 1
```

That unanimity is the evidence the vocabulary is real and shared rather than six
private switch statements that happen to have twelve arms.

## The return is not a boolean

`JobAttackTaskForce` returns **3** for code 1 where every other job returns 1 or
0. So slot 0 yields a **small enum**, not a bool — 0 and 1 dominate, and at least
one arm produces a third value. Any port that types this as `bool` loses
information at exactly one call site, which is the kind of thing that produces a
divergence a hundred frames later in a lockstep game (`docs/25`).

## What the profile says about each job

* **`JobAttack` and `JobAttackTaskForce` are near-identical** — they differ only
  in that one return of 3. The task-force variant is a coordinated version of the
  same responder, which matches `docs/73`'s size finding: 2904 bytes of tick
  against 1716 for the same event profile.
* **`JobContractBuild` is the most selective** — it declines codes 1, 4, 5, 8 and
  9, the largest decline set of any job. The AI's builder ignores most of what
  the combat jobs react to.
* **`JobRepair` declines code 5**, alone among the jobs that accept code 4. It is
  the only job with that combination.
* **Codes 8 and 9 are claimed only by the two attack jobs.** Everything else
  returns 0. Whatever those two events are, they are attack-specific.

## Three ways I got this wrong first

This pass produced a wrong matrix twice before producing the right one, and each
failure is worth recording.

**1. A vtable slot's low bit is the instruction set, and I ignored it.**
`JobAttackTaskForce`'s slot 0 has bit 0 clear — it is **ARM**, the only slot-0 in
the six that is. Disassembling it as Thumb produced plausible-looking garbage:

```
0208E498  ands  r0, r7          <- nonsense
0208E49A  .byte 0x2d, 0xe9
```

against the truth:

```
0208E498  push  {r3, r4, r5, lr}
0208E49C  ldrb  r2, [r1]
```

The project has known this since `docs/07`. **Take the mode from the corpus, do
not assume the majority.** Five of six slots being Thumb is exactly the
distribution that makes an assumption survive long enough to mislead.

**2. A jump table is not the whole function.** Three jobs — `JobAttack`,
`JobDefend` and `JobScout` — check specific codes and return **before** reaching
the table. `JobScout` special-cases codes 4 and 5. Decoding only the table gave
`JobScout` a 0 for code 5 when the function returns 1 for it. **The table is the
default path, not the behaviour.**

**3. Pattern proximity is not dataflow.** A generic extractor that took "the
nearest `cmp` with an immediate" as the table bound reported `JobAttack`
distinguishing codes 0 through 75 — it had latched onto an unrelated comparison
elsewhere in the function. Anchoring instead on the **dispatch instruction**
(`add pc, rX` in Thumb, `addls pc, pc, r2, lsl #2` in ARM) and working backwards
gave twelve every time.

That third one generalises the pitfall this project keeps re-learning. `docs/72`
put it as *match the register, not the constant*; the same rule for control flow
is **anchor on the instruction that does the work, not on one that looks nearby.**

## The two dispatch idioms

Worth having written down, because the codebase uses both:

```asm
Thumb   cmp   r1, #0xb
        bhi   default
        adds  r1, r1
        add   r1, pc              ; table of signed halfword offsets at pc+6
        ldrh  r1, [r1, #6]
        lsls  r1, #0x10
        asrs  r1, #0x10
        add   pc, r1              ; target = (this addr) + 4 + offset

ARM     cmp   r2, #0xb
        addls pc, pc, r2, lsl #2  ; table of B instructions at pc+8
        b     default
        b     arm_for_code_0
        ...
```

The ARM form is a **branch table**; the Thumb form is an **offset table**. Both
appear in this function pair, in the same class hierarchy, because the compiler
picked per translation unit.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 54.1% | 2,501 listed + 2,541 derived, 97 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
