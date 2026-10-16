# The twelve squad modes, and the 7/5 split

**Status: every site that changes a squad's mode is located — 48 of them in 13
functions — and the squad's own tick splits the twelve modes into the same two
groups in three separate places. The split is *whether the previous mode is
remembered*, and the seven "returnable" modes are exactly the seven that are ever
proposed.** Annotation 54.4% → 54.4%; **112 of 112 checks pass**.

## Finding every mode change

`docs/78` showed the mode-change shape: read `squad+0x10`, write it to `+0x14`,
then store the new value. That pair is a precise signature — an `ldr` from
`+0x10` immediately followed by an `str` to `+0x14` **on the same register
pair** — and searching for it is exact rather than approximate:

```
48 save-previous sites in 13 functions
proposed modes:  2 (x8)   3 (x8)   5 (x1)   7 (x24)   8 (x1)   10 (x1)   11 (x2)
```

**Mode 7 is half of all proposals.** Modes 5, 8 and 10 are proposed from exactly
one site each.

The thirteen functions include every job tick already read (`JobContractBuild`,
`JobDefend`, `JobRepair`, `JobScout`, `JobAttack`) plus `func_020988F6` at 3378
bytes, which reads `squad+0x38` **49 times** and `+0x10` **35 times** — the
squad's own tick.

## Three dispatches, one partition

`Squad_Tick` contains **six** jump tables, in three pairs: a 12-entry table
followed by an 11-entry one, three times over. The 12-entry tables are the
interesting ones, and all three partition the modes **identically**:

```
modes 2, 3, 5, 7, 8, 10, 11   ->  one arm
modes 0, 1, 4, 6, 9           ->  the other
```

Three independent tables agreeing on a 7/5 boundary is not a coincidence, and
the arms say what the boundary means:

```asm
; the seven-mode arm
ldr  r0, [r4, #0x10]      ; save the current mode
str  r0, [r4, #0x14]      ; ...into previous
; the five-mode arm falls in here, having skipped the two lines above
ldrb r0, [r6, #0x13]      ; the incoming mode
cmp  r5, #0xa
str  r0, [r4, #0x10]      ; apply it
beq  keep_counter
movs r0, #0
strh r0, [r4, #0xc]       ; reset the counter
keep_counter:
```

**The split is whether the outgoing mode is recorded.** The table is keyed on the
**incoming** mode and what gets stored is the **outgoing** one — `docs/80`
establishes the direction. So arriving in mode 2, 3, 5, 7, 8, 10 or 11 leaves a
trail in `+0x14`; arriving in 0, 1, 4, 6 or 9 does not, and there is nothing to
return to.

## The two lists match

The seven modes that get their previous value saved are **exactly** the seven
modes that are ever proposed at the 48 sites. Two lists produced by completely
different means — a signature search across the whole corpus, and three jump
tables inside one function — agreeing member for member.

That is the third time this argument has settled something here: `docs/72`
matched the build-limit classes to the category names, `docs/77` matched the job
kinds to the counter's keys *including their shared gap*, and now this.

`docs/78` found that **code 0 is refused by every job**. It is also one of the
five that leaves no previous mode. The two facts fit: mode 0 is an entry state,
not an order.

## Mode 10 keeps its counter

`cmp r5, #0xa` guards the reset of `squad+0x0C`. **Every mode change zeroes that
counter except a change to mode 10**, which preserves it.

One mode out of twelve with a different rule about a counter is precisely the
kind of thing that survives a careless port as a one-line difference and shows up
as a desync much later. It is a single comparison in the ROM and it needs to be
one in the reimplementation.

## A measurement that lied

My first pass at sizing the twelve arms computed each arm's length as **the
distance to the next distinct target address**, and reported `~4B` and `~0B`.
That is meaningless here: the arms **fall through into each other**, so the
seven-mode arm is two instructions that then continue into the five-mode arm's
code.

**Arm sizes by address arithmetic are only valid when every arm branches away.**
The fix was to read the arms rather than measure them — four instructions, and
the fall-through was obvious immediately.

## Tips and pitfalls

* **A two-instruction signature beats a pattern.** `ldr +0x10` then `str +0x14`
  on the same registers found 48 sites with no false positives; the loose
  approaches earlier in this project found thousands.
* **Two lists agreeing member-for-member is the strongest evidence available**
  when neither can be checked directly. Third time here.
* **Jump-table arms that fall through cannot be sized by subtraction.**
* **One exception in a twelve-way table is the whole finding.** Mode 10's counter
  is the only asymmetry in the entire mechanism.
* **Count the proposals, not just the values.** Mode 7 being 24 of 48 says the
  mechanism mostly exists to drive one transition.

## Open, and stated as open

**What each mode means individually is not established.** The 7/5 split, the
proposal counts and mode 10's exception are all read from the ROM; the names are
not. The next step is the 11-entry second-level tables in `Squad_Tick`, which
dispatch the per-mode work and whose arms do *not* all fall through — those will
size properly and their contents should name at least the common modes.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **54.4%** | 2,501 listed + 2568 derived, 128 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
