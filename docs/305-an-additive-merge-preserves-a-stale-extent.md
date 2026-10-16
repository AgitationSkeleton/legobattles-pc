# 305 — An additive merge keeps stale extents, and 537 candidates were not entries

docs/304 closed the ARM7 down to 8,800 interpreter entries and ended with a
claim about what was left: *"the corpus covers them but the lift emitted no
dispatch entry — that is the recompiler's own tail-dropping."*

**That was wrong**, and checking rather than acting on it saved a hunt for a
recompiler bug that does not exist. Every one of the remaining pcs has **no
corpus owner at all**:

```
pc            hits   corpus owner   in config   in dispatch table
0x03803CB4    2430   NONE           n           n
0x03803CC8    2430   NONE           n           n
0x038028E4    1506   NONE           n           n
```

The error was measuring against the wrong file. The "100% covered" prediction
in docs/304 was computed against the 919-row full merge — the set that was
explicitly *declined* — and not against the 585-row evidence corpus actually
installed. Two different files, one of them never shipped.

## The real fault: a merge that protects stale rows

`0x03803CB4` is the first arm of this switch:

```
0x03803CA8  cmp   r1, #3
0x03803CAC  addls pc, pc, r1, lsl #2
0x03803CB0  b     #0x3803C90          <- default, a backward loop branch
0x03803CB4  b     #0x3803CC4          case 0   <- 2,430 hits
0x03803CB8  b     #0x3803CDC          case 1
0x03803CBC  b     #0x3803CCC          case 2
0x03803CC0  b     #0x3803CD4          case 3
0x03803CC4  bl    #0x3804048
0x03803CC8  b     #0x3803C90                   <- 2,430 hits
```

docs/304's switch rule handles exactly this shape. So why was it not applied?

**Because the enclosing function was already in the corpus.** `0x03803C7C` was
carried at size `0x38`, which ends at `0x03803CB4` — precisely the first arm.
The old walk had stopped at the unconditional `b` on the line above and
recorded its high-water mark there. `--merge` keeps existing rows *byte for
byte*, so it kept the stale extent, and the new rule's result was thrown away
for the one function that most needed it.

The safety property was doing harm. Fixed by making the merge **grow only**:
`high` is raised solely by instructions the walk actually decoded, so a larger
extent means more real code was read and adopting it is strictly better;
shrinking is the direction that can un-lift working code and is still refused.

Measured across the whole corpus, that is **one row**: `0x03803C7C`,
`0x38 -> 0x68`, +48 bytes, and `rows SHRUNK: 0`.

## The save-flash driver, and why no static rule finds it

`0x038028E4` is the other half of the residual:

```
0x038028E4  ldr  r1, [pc, #0x34]    = 0x040001A2   REG_AUXSPIDATA
0x038028E8  mov  r2, #0
0x038028EC  strh r2, [r1]
0x038028F0  sub  r2, r1, #2         = 0x040001A0   REG_AUXSPICNT
0x038028F4  ldrh r1, [r2]
0x038028F8  tst  r1, #0x80
0x038028FC  bne  #0x38028F4                        <- busy-bit spin
```

The cartridge **backup-SPI driver** — the save flash. It is a leaf: no `push`,
so `lb_scan`'s prologue check rejects it, and it is held only in function
pointers at `0x03802820`, `0x03802B6C` and `0x03802D6C`, all in undeclared
code, so no call graph reaches it either. It is invisible to every static rule
the tool has.

It is not invisible to the machine. `--seed-from-coverage` now seeds from a
run's `tier3_coverage.txt` — every ROOT pc the interpreter was entered at.
That is observation, not a heuristic: the cartridge's own code ran there.

## 537 of 561 candidates were not function entries

Seeded that way, the scan offers 561 new rows and they reach **100%** of the
residual. They were not taken as a block, because a ROOT pc is not guaranteed
to be a function *entry* — docs/302 was written about exactly that mistake.

Splitting them by what actually backs each one:

```
                                                   rows   residual covered
is a direct BL target (a real call destination)      22        0.0%
not BL'd but a stored function POINTER                2       44.7%
neither -- ROOT pc only, may be mid-function        537      100.0%
```

**Two rows carry 44.7%**, and 537 carry the rest. Those 537 are mid-function
re-entry addresses in undeclared code — declaring each as a "function" would
mean 537 lifted fragments starting mid-stream, to chase the last half of a
3.9% residual. That is not a decomp; it is papering over one.

So the merge took **24 rows**: the 22 call destinations and the 2 stored
pointers, each a fact in the cartridge rather than a pattern. Corpus 585 ->
609; `lb_arm7_wram` 409 -> 418 entry points, `lb_arm7_mainram` 173 -> 188. The
single deletion in the diff is the `0x03803C7C` size line, as intended.

The proper fix for the remaining pcs is to declare their **enclosing**
functions, which means identifying where those start — real work, not a loop
tick's shortcut. It is recorded here rather than guessed at.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** docs/304's closing claim was wrong; the residual has no corpus
owner, and the cause was an additive merge preserving a stale extent.

**ESTABLISHED:** the grow-only merge changes exactly one row and shrinks none.

**ESTABLISHED:** `0x038028E4` is the cartridge backup-SPI (save flash) driver,
from `REG_AUXSPICNT`/`REG_AUXSPIDATA` and the busy-bit spin.

**ESTABLISHED:** 537 of the 561 coverage-seeded candidates have no evidence of
being function entries, and they are not taken.

**ESTABLISHED (measured): the ARM7 now runs entirely as lifted native code.**

```
run        shroud  fps      cache_hit   absent     absent%   ROOT hits
a7valid1     19    53.32    4,389,942  3,304,104    42.9%      157,847
a7irq1       19    50.98    4,408,343  1,921,556    30.4%       58,477
a7sw1        19    57.27    4,521,738    181,928     3.9%        8,800
a7ptr1       17    59.61    4,559,246          0     0.0%             0
```

The prediction was 44.7%; the result is all of it. Declaring those 24 entries
also brought their extents over the mid-function re-entry pcs, so the 537
candidates that were refused turned out not to be needed at all — which is the
better argument for refusing them than the one made above.

A zero deserves more scepticism than a number, so it was checked four ways:

* the coverage file is **not empty** — 426 entries, 73,417 hits, all ARM9;
* **zero ARM7 rows of ANY kind**, not merely zero ROOT rows;
* the ARM7 is busy — `dispatch_total` 4,611,280, `cache_slow_lookup` 3,696, so
  lookups did happen and every one resolved;
* the ARM9's `cache_hit_absent` is still 517,709, exactly as it should be,
  because this port patches guest bytes — so the counter is alive, not stuck.

The run is a real match: 17 `[lb_shroud]` lines and `peak 74/2048 polygons`,
with the highest `cache_hit` of the four, so the ARM7 did *more* native work
than any earlier run, not less.

**ARM7 interpreter entries across the three changes: 157,847 -> 0.**

**No fps claim.** 59.61 is the best of the four and the trend is in the right
direction across all four runs, but this harness swings far more than that on
identical code, and one run each is not an A/B.

**No sound claim.** The missing-sound fix stays unclaimed until heard. What is now true is
that the ARM7 sound driver runs as compiled native code instead of an
interpreter, which was the mechanism docs/301 proposed; whether that was the
cause of the silence is a question only a play session answers.
