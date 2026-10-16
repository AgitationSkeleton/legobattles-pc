# 304 — The walk reported extents it had never read, and the switch arms were in them

docs/303 closed the ARM7's largest undeclared region by finding the IRQ vector
table. That left 58,477 interpreter entries in seven regions, and the obvious
next move — a fuller rescan — does not work:

```
remaining ARM7 ROOT hits after the IRQ fix: 58,477
   covered by the current corpus              0    0.0%
   covered by a FULL rescan (--scan-data)  13,573   23.2%
```

A rescan with every heuristic the tool has still misses three quarters of it.
So the gap is not "not enough seeds".

## A declared function whose call site was never read

`0x03803E60` is the target of a `bl` at `0x03803D20`. That call site is **inside
the declared extent of `0x03803CE8`** (`0x3803CE8..0x3803D50`), in both the
committed corpus and a fresh scan — and `0x03803E60` is in neither. A walk that
covered the call site did not record the call.

Disassembly says why:

```
0x03803CEC  cmp   r2, #0
0x03803CF0  bne   #0x3803D48
0x03803CF4  sub   r0, r0, #4
0x03803CF8  cmp   r0, #5
0x03803CFC  addls pc, pc, r0, lsl #2      <- the switch
0x03803D00  b     #0x3803D48              <- default (add did not execute)
0x03803D04  b     #0x3803D40              case 0
0x03803D08  b     #0x3803D48              case 1
0x03803D0C  b     #0x3803D1C              case 2
0x03803D10  b     #0x3803D48              case 3
0x03803D14  b     #0x3803D34              case 4
0x03803D18  b     #0x3803D28              case 5
...
0x03803D20  bl    #0x3803E60              <- never decoded
0x03803D2C  bl    #0x38053A8              <- never decoded
0x03803D38  bl    #0x38048E0              <- never decoded
0x03803D44  bl    #0x27F4FD4              <- never decoded
```

`add` is not a branch mnemonic, so `walk` reads straight past the switch to the
default `b` at `0x03803D00`, ends the block there, resumes at `0x03803D48`, and
returns. The arms between are never decoded.

**And this is invisible in the output**, which is the part worth recording.
`walk` returns `high`, a *high-water mark*, not the set of addresses it read.
`high` reaches `0x03803D50` through the default branch, so the function is
reported with a full, correct-looking extent of `0x68` while more than half of
its body was never looked at. docs/83 warned that the high-water mark can
*overrun*; this is the same fact biting from the other side, and it is worse,
because an overrun is at least visible as overlap in the tool's own summary.

The consequence for the port is direct: the recompiler lifts the whole declared
extent, so the `bl` at `0x03803D20` **is** compiled — it just calls an address
nobody declared, which resolves to nothing and drops into the interpreter. That
is precisely the ROOT at `0x03803E60`.

## The fix

`add pc, pc, rN, lsl #2` is an exact idiom, not a pattern guess. The
instruction lands at `(its address + 8) + rN*4`, so the arms begin at the
instruction after the default branch, and the preceding `cmp rN, #K` gives the
case count. `walk` now enqueues the arm addresses; each is a `b` it already
knows how to follow. Without a `cmp` bound it follows the run of unconditional
branches and stops at the first slot that is not one.

Measured from a single seed at `0x03803CE8`, which previously found only
itself: 20 contiguous runs, 4,304 bytes, and all four arm targets —
`0x03803E60` (0x1DC), `0x038048E0` (0x1B4), `0x038053A8` (0x2B8), `0x027F4FD4`
(0x1A0, in the mainram section).

Across the full scan:

```
                                  executable covered   remaining ROOT hits
full rescan, before                      79.1%              23.2%
full rescan, with the switch rule        92.3%              96.2%
```

| region | hits | before | after |
|---|---|---|---|
| `0x038043A4..0x0380488C` | 22,963 | 0% | 96% |
| `0x03801D50..0x03801ED4` | 10,269 | 12% | 95% |
| `0x03803A48..0x03803B70` | 6,749 | 73% | 97% |
| `0x03803E60..0x03803F24` | 6,534 | 0% | 100% |
| `0x03804030..0x038041FC` | 5,574 | 0% | 97% |
| `0x03802730..0x03802C10` | 2,884 | 6% | 80% |
| `0x03803CB4..0x03803CC8` | 2,410 | 0% | 100% |

Region 2, the largest, is the **SPI driver** — `0x038043A4` spins on
`0x040001C0` (REG_SPICNT) waiting for the busy bit. On the DS that bus carries
the touchscreen, the firmware flash and power management, which is why it is
hot every frame.

## What was NOT taken: 334 rows with no evidence behind them

A full `--scan-data` merge would have added 439 rows. Every one arrived labelled
tier 3 — and that label is itself coarser than it looks, because a callee
inherits its seed's tier, so a genuine `bl` target discovered downstream of a
prologue guess is recorded as a guess too.

So the rows were re-judged on evidence rather than on the label. Two kinds
count, and each is a fact rather than a heuristic:

* the address is the target of a **direct `bl`/`blx` from inside a declared
  extent** — an instruction that exists in the cartridge, not a pattern;
* the function **contains an address the interpreter actually executed**, from
  two captures' coverage.

Iterated to a fixed point (5 rounds, so that only `bl`s from already-justified
extents count), that is **105 rows, not 439** — and it reaches *exactly* the
same coverage:

```
evidence-only fixed point   585 rows   92.1% of remaining ROOT hits
all-439 merge               919 rows   92.1%
```

**The 334 prologue-guess rows buy nothing measurable**, so they are not taken.
The project does not take guessed rows; here that also happens to cost nothing.

The merge stays additive — `existing rows changed: 0` — and both ARM7 configs
gain entries and lose none: `lb_arm7_wram` 324 -> 409, `lb_arm7_mainram`
153 -> 173, 525 lines inserted, 0 deleted.

## A corpus that lied about its own provenance

`lb_scan`'s TSV writer emitted a literal `1` in the tier column for every
walked function. So a corpus built with `--scan-data` — which is how
`out/eur/arm7_corpus.tsv` was built, per docs/17 — claimed all 469 of its rows
were call-graph certain. Nothing consumes the column, so correcting it to the
real discovery tier is free, and a corpus that misreports its own provenance is
exactly what the project's evidence-only rule exists to prevent.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** `walk` never decoded PC-relative switch arms, and its
high-water-mark extent concealed that. One worked case is shown instruction by
instruction; the fix recovers all four of its `bl` targets.

**ESTABLISHED:** the additive merge changes no existing row, and the
evidence-only subset matches the full merge's coverage at a quarter the size.

**ESTABLISHED:** region 2 is the SPI driver, from `REG_SPICNT` at
`0x040001C0` and its busy-bit spin.

**ESTABLISHED (measured):** the runtime interpreted share falls hard.

```
run        shroud  fps      cache_hit   absent     absent%   ROOT hits
a7valid1     19    53.32    4,389,942  3,304,104    42.9%      157,847
a7irq1       19    50.98    4,408,343  1,921,556    30.4%       58,477
a7sw1        13    57.27    4,521,738    181,928     3.9%        8,800
```

**ARM7 `cache_hit_absent` 42.9% -> 3.9%, and native-to-interpreter boundaries
157,847 -> 8,800, down 94.4%** across the two changes.

The length control holds and rules out the obvious objection: `cache_hit` is
within 3% across all three runs, and the last run's is the *highest* of the
three, so the ARM7 did more native work, not less. The lower `[lb_shroud]`
count is a shorter linger, not a shorter measurement.

**No fps claim.** 57.27 is the best of the three but this harness swings far
more than that on identical code.

**NOT ESTABLISHED:** any effect on the silent unit sounds (item 2), the flicker
(item 7) or the framerate (item 9). All three stay unclaimed until played.

## What is left

The remaining 8,800 entries are still 100% absent from every dispatch table.

**CORRECTION.** This section originally said the corpus covered them and blamed
the recompiler's tail-dropping. It does not, and the recompiler is innocent:
the prediction was measured against the 919-row full merge that was *declined*,
not the 585-row evidence corpus that shipped. `0x03803CB4` has no corpus owner
at all. The real cause — an additive merge preserving a stale extent for
`0x03803C7C` — is docs/305.
