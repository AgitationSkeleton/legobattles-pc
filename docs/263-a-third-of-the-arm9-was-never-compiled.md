# 263 — A third of the ARM9 was never compiled

The goal: a stable 60 fps minimum, preserving visual, audio and netplay fidelity.

```
                baseline (d05eb511)      re-lifted
fps             36.95 36.54 35.95 36.04  59.06 58.14 58.82 59.21
                mean 36.4                mean 58.8          +62%
emu_ticks       22.332 ms/frame          10.925 ms/frame    -51%
present_ticks    5.501 ms/frame           5.412 ms/frame    unchanged
```

Four runs per arm, 1920x1080 window, same route.

## The number that found it

`lb_arm9.toml` declares 9,156 entry points. `authoritative_entry_points = true`
makes an exact `size` mandatory on every one of them — config.cpp refuses the
config otherwise — and each size is read from the ds-decomp corpus. They total
**412,770 instructions**.

The finder lifted **279,959**.

132,811 instructions — 32% of the game's ARM9 code — had no compiled bank, so
the runtime ran them through the armv4t interpreter. Of 21,882,484 Tier-3 ROOT
entries in one freeplay run, **21,869,173 (99.94%) were at a PC with no
dispatch row at all**, spread over 22,760 addresses in 368 contiguous regions
of the cartridge's own static image.

## The rule that did it

`FunctionFinder::discover_one()` walks linearly from the seed and stops at the
first unconditional terminator:

```cpp
if (ins.is_return || ins.is_indirect) break;
if (unconditional_branch)             break;
```

It does that whether or not the seed carried an `exact_size`. The declared size
was only ever the loop's upper bound (`pc < exact_end`), never a lower one, so
everything after a function's first `bx lr` / `pop {pc}` / unconditional `b`
was dropped.

`func_020267f8` is the clearest case. The decomp declares 0x7A8 bytes; the
emitted validation record was **428**, and the dispatch rows ran
0x020267F8..0x020269A0 and then jumped straight to 0x02026FA0. The 0x600-byte
hole between them holds 0x02026ACC, 0x02026ADC and 0x02026D30 — 2.7 million
interpreter entries in one run.

The comment at the break site states the reasoning it was written under:

> 3. An unconditional branch (B with cond=AL) — we'll reach the target by
>    adding it to the worklist.

True of the **target**. Not true of the instruction that *follows* the
terminator, which is reached only from elsewhere in the same body — a backward
conditional branch, a jump table, a landing pad. In discovery mode, stopping is
correct: the finder cannot tell code from the literal pool that usually follows
a return. With an exact size in hand it does not have to guess, and stopping is
simply wrong.

## The fix

`port/recomp/apply_finder_patch.py` — a patch over a **copy** of ndsrecomp,
same contract as `apply_hooks.py`; `$NDSRECOMP` stays read-only.
`port/recomp/build.sh` builds it, and `port/lift.sh` prefers that binary when
it exists (`LB_RECOMPILE` overrides).

Both breaks become conditional on `exact_end == 0u`. When the decomp has
declared the extent, a terminator ends a **basic block**, not the function.

`ins.is_undefined` and `addr_in_data_range(pc)` still break the walk, which is
the safety net: decoding past a terminator means decoding literal pools, and
249 ARM9 walks now truncate on an undecodable word rather than running away.

## Emission, and the audit gate

```
ARM9 dispatch rows      164,778  ->  409,316     (declared total 412,770)
uncovered hot PCs        22,760  ->      872
regions                     368  ->       18
generated C               214 MB ->   551 MB
```

The 18 remaining regions are ARM7 WRAM, ARM7/ARM9 BIOS and ARM9 ITCM — not the
main image. `nds_recompile --audit` still reports `codegen NOT-IMPLEMENTED ops:
(none)` and the same undefined-encoding set (1 ARM, 18 Thumb), which is the gate
that says the extra decoded bytes did not produce anything the codegen cannot
lower.

Shards went from 13 MB to 43 MB each. **Build at LB_JOBS=3, not 6** — six
parallel `cc1` on 43 MB translation units gets OOM-killed, and that failure
presents as `build FAILED` with no compile error in the log.

## Fidelity

Measured, not assumed:

```
Tier-3 total hits   33,177,049  ->  ~514,000     -98.4%
Tier-3 ROOT         21,882,484  ->  ~248,000     -98.9%   (predicted 257K)
map coverage        23418/24576 and 23808/24576  IDENTICAL to baseline
shroud              716/768, edgemasks=6         identical
halt / dispatch-miss / assert / desync           0 across all four runs
```

The map-coverage figures matter most: they are the guest's own fog record, so
an instruction lifted wrongly in the newly-compiled 32% would move them.

## Two retractions

**The coverage dump's top line was never evidence.** `0x02026170` at 6.46% is
kind=CALL, and `coverage_note(..., TIER3_COVERAGE_CALL, ...)` fires on *every*
call the interpreter makes, whether or not the target has a bank — the loop
breaks out to Tier 1 on the next iteration anyway. Only ROOT rows indicate a
missing bank. CALL was 10.7M of the 33.2M hits and 99.4% of it had a row.

**docs/262's "those are return sites inside func_020267f8, which size=0x7a8
already covers"** read the decomp's size and concluded the lift agreed with it.
The lift did not. The decomp was right; the recompiler's own function was 428
bytes. That retraction blocked this fix for a while.

## Where the budget stands now

```
emu_ticks     10.925 ms
present_ticks  5.412 ms   (adaptive 3.509)
              ---------
               16.34 ms   -> 61.2 fps of capacity, measured 58.1-59.2
```

The DS's own rate is 59.826 Hz, so this is essentially full speed — but the
margin is about 0.35 ms per frame, which is not much for "stable, no
stuttering". Presentation is now 33% of the frame and is the next place to
look; `LB_BANK_OPT` is still at -O2 and has never had a proper four-run A/B.
