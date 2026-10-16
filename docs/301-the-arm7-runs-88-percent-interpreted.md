# 301 — The ARM7 runs 88% interpreted, and it is the sound processor

Chasing the missing-sounds defect (item 2) downstream of loading turned up something structural
that is worth its own page, because it bears on more than sound.

## The measurement

`dispatch_stats.json` is written per run and keyed by CPU. From a full replay of
a recorded combat session:

```
            dispatch_total     cache_hit_absent      share
ARM9         321,747,493          9,009,497          2.8%
ARM7          18,267,269         15,998,206         87.6%
```

`cache_hit_absent` was verified against the code rather than read off its name.
In `cached_lookup_live`:

```c
if (slot.fn) { ++stats.cache_hit; return &slot; }
++stats.cache_hit_absent;
return nullptr;
```

A null return is the Tier-3 interpreter. So the counter is exactly "a cached
lookup that found no lifted function", and **the ARM7 executes roughly seven of
every eight dispatches interpreted**, against one in thirty-five on the ARM9.

That is consistent with the declared coverage: 9,156 ARM9 entry points against
469 for the ARM7 across all three of its regions (153 mainram, 3 static, 313
wram).

## Why this matters for item 2

**On the DS, the ARM7 runs the sound driver.** The ARM9 sends commands over the
IPC FIFO; the ARM7 services the sixteen hardware channels on a timer, in real
time. An interpreter is perhaps an order of magnitude slower than lifted code,
and a driver that misses its deadline does not fail loudly — it drops or fails
to start voices.

That shape matches the symptom: *some* sounds play (the ones that get serviced),
most do not, and nothing errors. It also fits everything measured so far —
docs/299 found no allocation failure, and docs/300 found every bank id mapped
and requested. A voice that is requested, allocated and then never started by an
overloaded driver leaves exactly that evidence trail.

## What is established, and what is NOT

**ESTABLISHED:** the ARM7 is 87.6% interpreted by dispatch count, verified
against the counter's implementation, on a real match.

**NOT ESTABLISHED:** that this causes the missing sounds. This is a candidate
mechanism with the right shape, not a demonstration. Nothing here shows a voice
being dropped, or a deadline being missed, or that lifting the ARM7 sound driver
would restore the silent units.

The distinction matters because the fix is expensive. Lifting the ARM7 properly
is the kind of work that should follow evidence, not a plausible story — this
project has learned that lesson repeatedly in smaller ways.

## The cheaper test that should come first

Before any lifting: find which ARM7 functions the interpreter actually spends
its time in. 469 declared entry points against 88% absent dispatches means the
undeclared set is large, but the *hot* part of it may be small. If the sound
driver's inner loop is a handful of functions, declaring those is a much smaller
change than lifting the ARM7, and it would test the hypothesis directly.

## Possible bearing on other items

Item 9, an unstable framerate of about 39-45 fps, was already suspected to share
a cause with the sound defect, which points the same way. Item
7, intermittent flicker, is also consistent with an ARM7 missing deadlines,
since it drives more than audio. Neither is established here; both are recorded
so that a future ARM7 measurement looks for them.

## The cheap test, run: the hot set is small and mostly undeclared

`tier3_coverage` already records per-address interpreter entries with cpu, pc,
caller, kind and hits. From the same replay:

```
total interpreted hits 2,638,497
   arm7   1,057 entries   1,346,031 hits   51.0%
   arm9   1,247 entries   1,292,466 hits   49.0%

top ARM7 addresses
   0x037FB4F4  162,209  6.15%      0x037FB718  142,709  5.41%
   0x037FB684  145,006  5.50%      0x037FD648  142,709  5.41%
   0x037FB63C  145,006  5.50%      0x038044A8   46,526  1.76%
```

**The top twelve ARM7 addresses are 34.8% of ALL interpreted work**, and six of
them cluster in ~640 bytes around `0x037FB4F4..0x037FB774`. So the undeclared
set is large but its hot part is tiny — which is the answer the cheap test was
asked for, and it makes declaring a handful of functions a far smaller change
than lifting the ARM7.

## Two hypotheses killed on the way, both by checking

**"The ARM7 banks are never registered."** Plausible, and wrong. Neither
`lb_arm7_*` nor `lb_arm9_*` appears in the runner's own sources, so their
absence proves nothing — registration is elsewhere. The binary settles it:

```
lb_arm9_tfunc   3870      lb_arm7_wram    999
lb_arm7_mainram  504      lb_arm7_static   18
```

All banks are compiled in. The ARM7 is lifted; there are simply not enough
declared entry points to cover what runs — 469 against the ARM9's 9,156.

**"The finder is dropping ARM7 tails."** The finder patch is applied to the
recompiler itself, so every bank including the ARM7 gets it.

## The one thing that does not fit, stated rather than smoothed over

`0x037FB4F4` (size `0x13C`) and `0x037FD648` (size `0x10`) **are declared in
`lb_arm7_wram.toml`**, and both still appear in the interpreter's coverage —
together 11.6% of all interpreted hits. A declared, lifted function reached
indirectly should resolve through the dispatch table.

Both are `kind = 3`, `TIER3_COVERAGE_INDIRECT`. Whether an indirect ARM7 target
resolves differently, or the coverage records the pc where the interpreter was
already running rather than a failed lookup, is **not established** and is the
first thing to settle before declaring anything — because if declared functions
are interpreted anyway, adding more declarations fixes nothing.

## Next step

Settle the indirect question above. If declared entries do resolve, run the
lift's `--audit` mode over the ARM7 sections to get candidate addresses with
exact sizes (`authoritative_entry_points` requires them), and declare the hot
cluster first. Twelve addresses covering a third of all interpreted work is a
cheap experiment with a measurable outcome.

## RESOLVED: the anomaly was my reading of the `kind` column

The previous section flagged `0x037FB4F4` and `0x037FD648` as declared yet
interpreted, and said that had to be settled before declaring anything. It is
settled, and there is no anomaly — the two rows do not mean what I took them to
mean.

Reading `tier3.cpp`:

* **`TIER3_COVERAGE_ROOT` (kind 1)** is recorded in `tier3_run`, at "every
  native-to-Tier3 boundary". This genuinely is *the interpreter was entered at
  this pc*.
* **`CALL` (2) and `INDIRECT` (3)** are recorded from **inside** the interpreter
  loop when it executes a branch, and they record the branch **target**. They
  mean "interpreted code branched here" — the target may be lifted and may run
  natively. They are discovery hints, not evidence of interpretation.

Both flagged addresses are `kind = 3`. **Declared functions are resolving
correctly**, and the case for declaring more is intact.

## The corrected hot list

Filtering to ROOT only:

```
kind 1 ROOT       2,100 entries   1,365,795 hits
kind 2 CALL         165 entries     773,890 hits
kind 3 INDIRECT      39 entries     498,812 hits

ARM7 ROOT: 990 entries, 661,382 hits
   0x037FB684   145,006   21.9%      0x038044A8   46,526   7.0%
   0x037FB718   142,709   21.6%      0x038044AC   21,521   3.3%
   0x037FB748    17,203    2.6%      0x03801EA8   14,067   2.1%
   0x037FB774    17,203    2.6%      0x03801EC8   14,067   2.1%
```

**Two addresses are 43.5% of every ARM7 native-to-interpreter boundary**, and
both are undeclared (`lb_arm7_wram.toml` has neither). The top ten are about
67%.

This supersedes the earlier "top twelve are 34.8% of all interpreted work"
figure, which mixed the three kinds and counted branch targets as interpreted
work.

## The next step is now small and specific

Declare `0x037FB684` and `0x037FB718` — and then the rest of the top ten — in
`port/config/lb_arm7_wram.toml`. `authoritative_entry_points` requires an exact
`size`, so the lift's `--audit` mode has to supply the extents first.

Two entry points covering 43.5% of ARM7 interpreter entries is a cheap
experiment with a measurable outcome: rerun, and both the ROOT hit count and the
`cache_hit_absent` share should fall. Whether that makes the missing unit sounds
audible is a separate question and stays unclaimed until heard.

## RETRACTED — the next step above is wrong, and so is the hot list's reading

**docs/302 supersedes everything from "The corrected hot list" onwards.**

`0x037FB684` is not a function. It is a **mid-function return point**:
`0x037FB67C mov lr, pc` / `0x037FB680 bx r1` sets `lr` to exactly `0x037FB684`,
which is why every row in that cluster shows `caller = 0x037FB684`. The
"caller" column is `R[14]` sampled at the boundary, not a call site, and
reading it as one is what made these look like undeclared functions.

`0x037FB718` is **arm #5 of an eight-entry trampoline table**, 0x10 bytes each,
tail-jumping to the common body at `0x037FB63C`. The real function is
`0x037FB63C`, size 0x8C.

Declaring either would have been wrong, and it would have fixed nothing:
classifying every ARM7 ROOT against the declared entry map gives **92.9% of
interpreter entries inside functions that are ALREADY declared**, and 0% on a
declared entry point. The undeclared gaps are 7.1% of the total.

The cause is the validation gate in `lookup_in()`, not the entry-point set.
See docs/302.

Also wrong: `--audit` does not enumerate candidate addresses with sizes (it is
validate-only), and it segfaults on every bank in both the stock and the
patched recompiler. The extent-finding step this document planned does not
exist.

**What survives from this document:** the dispatch-stats measurement (ARM7
87.6% interpreted, verified against `cached_lookup_live`), the `kind` column
correction (ROOT vs CALL/INDIRECT), and the two hypotheses killed in passing
(the banks ARE compiled in; the finder patch DOES reach the ARM7).
