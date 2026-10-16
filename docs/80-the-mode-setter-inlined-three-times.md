# The mode setter, inlined three times

**Status: the squad mode-change block is read end to end. It is one routine
emitted three times, differing in a single byte offset; it stages the proposed
mode before asking the veto; and it is fronted by two guards, one of which makes
mode 1 permanent.** Annotation unchanged at 54.4%; **116 of 116 checks pass**.

## Three copies, one instruction apart

`docs/79` found three 12-entry mode tables inside `Squad_Tick` that partition the
modes identically. They are identical for a reason: comparing the three regions
byte for byte, **the first 42 bytes match exactly**, and the first difference is
one instruction:

```
copy 1   ldrb r0, [r6, #0x13]
copy 2   ldrb r0, [r6, #2]
copy 3   ldrb r0, [r6, #1]
```

Everything else — both jump tables, all the arms, the counter rule — is the same
code emitted three times. **`r6` is a staging record carrying three mode slots**
at `+1`, `+2` and `+0x13`, and each copy of the routine drives one of them.

For a decompilation that matters: this is one inlined helper, not three
behaviours, and it should be written once and instantiated three times or the
byte-matching effort will fight it.

## The full sequence

```c
cur = squad->mode_10;
if (proposed == cur) return;           /* guard 1: already there */
if (cur == 1)       return;            /* guard 2: mode 1 is sticky */

if (squad->job_38) {
    r6->slot_13 = proposed;            /* STAGE it before asking */
    if (!squad->job_38->vtable[0](job, &code))   /* the docs/78 veto */
        return;
    if (proposed <= 11)
        switch (proposed) { case 2,3,5,7,8,10,11: squad->prev_14 = cur; }
    n = r6->slot_13;                   /* read the staged value back */
    if (proposed != 10) squad->counter_C = 0;
    squad->mode_10 = n;
} else {
    /* no job: a different 11-entry table, no veto asked */
}
```

Three things here are not obvious from either earlier document.

**The proposal is staged before the veto is asked.** `r6->slot_13 = proposed`
happens *first*, and the applied value is read back out of the record
afterwards rather than kept in a register. A port that passes the value straight
through would behave identically only if nothing between those two points can
touch that byte — and the veto is a virtual call, so that is not obviously true.

**Mode 1 is permanent.** `if (cur == 1) return` sits ahead of everything,
including the veto. Once a squad reaches mode 1 this path can never move it
again. That is the strongest statement any of the twelve modes gets, and it
explains why mode 1 is one of `docs/79`'s five modes with no saved previous:
there is no return path because there is no exit.

**The direction of the 12-entry table.** It is keyed on the **incoming** mode
and what it stores is the **outgoing** one. `docs/79` stated the split correctly
but described it ambiguously; corrected there in place.

## The no-job path

When `squad+0x38` is null there is no job to ask, and a **different** 11-entry
table runs — the second-level tables `docs/79` left as the stated next step. All
three copies partition it identically again, into three groups this time:

| modes | action |
| --- | --- |
| 1 | call `func_0209C4D0(squad)`, then apply |
| 5, 8, 10 | record the previous mode, then apply |
| 0, 2, 3, 4, 6, 7, 9 | apply directly |

Mode **11 is outside this table** — the bound is 10 — so it takes the
out-of-range path.

Note the group `{5, 8, 10}`: those are exactly the three modes `docs/79` found
being proposed from **one site each**. The rarest three modes are also the three
that need their predecessor recorded on the jobless path.

## Nearly retracting a correct claim

Mid-pass I read `cmp r5, #0xa` as testing the *outgoing* mode and was about to
correct `docs/79`'s statement that entering mode 10 preserves the counter. Before
editing I traced where `r5` comes from — twenty-four instructions back, it is the
**proposed** mode, staged and compared. `docs/79` was right and the correction
would have introduced an error.

**The rule that saved it is the one that has now worked repeatedly here: trace
the register, do not read the offset.** The same discipline that caught a real
mistake in `docs/76` prevented a false one here, which is the more useful half —
a correction to a correct claim costs twice, once in the wrong edit and again in
the trust placed in the wrong version.

## An error of mine, and the check it produced

I landed a tier R name for `0x02099602`, having read `bl #0x2099602` as a call to
a function. **It is not a function** — the check `every hand-written row names a
real function` caught it immediately, and the address turns out to be **interior
to a known function**: a secondary entry point, not a missing symbol. The row is
withdrawn.

That is worth generalising, because the project claims the ARM9 `.text` segment
is 100% covered and this is an **independent way to test that claim** — not by
counting bytes but by asking whether every call lands somewhere the corpus knows:

```
38,759  bl/blx targets inside the ARM9 text range
    18  are not corpus function starts
    13  of those are interior to a known function
     5  unexplained
```

**Thirteen calls into the middle of known functions.** That is not corruption —
it is **secondary entry points**, a real feature of this codebase and one a
decompilation has to handle: you cannot emit one label per function and expect
the call graph to reconstruct.

Of the five unexplained, one is `0x020EB940`, sitting **exactly** at the end of
`func_020EB910` with `func_020EB942` starting two bytes later — a one-instruction
boundary question worth a look. The other four all lie past the last corpus
function, scattered across 60 KB, with one call site each: the signature of
**literal-pool words decoded as `bl`**, the same false-positive class `docs/70`
found when sweeping the overlays.

So the byte-count claim survives an independent test with **13 explained
anomalies and 5 that look like decode artefacts** — and the test is now check
116.

## Tips and pitfalls

* **Byte-compare before interpreting repetition.** Three identical-looking blocks
  were one inlined helper, and the diff was a single `ldrb` offset.
* **A guard ahead of a veto outranks the veto.** Mode 1's stickiness is
  unconditional; no job gets asked.
* **Staging through memory is observable.** A value written to a record, passed
  through a virtual call, and read back is not the same as a value held in a
  register.
* **Verify before retracting, not just before asserting.** Retractions deserve
  the same evidence bar as claims.
* **The rarest values often share a rule.** `{5, 8, 10}` are the three
  single-site modes and the three that the jobless path treats specially.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 54.4% | 2,501 listed + 2569 derived, 129 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
