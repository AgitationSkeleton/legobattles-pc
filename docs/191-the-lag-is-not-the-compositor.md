# 191 — The lag is not the compositor

Status: **measured.** Redirects the performance work entirely. The wide field was
blamed for a frame rate it barely affects.

## Play mode needed its own driver

`lb_warp.py` cannot drive the port once the frontend is running: it advances the
emulator with `run_cycles`, which the debug server refuses — *"run_cycles
unavailable in play mode: the frontend owns execution"*. Every measurement in
this project so far had therefore been taken headless, where frame rate does not
exist.

`tools/lb_play.py` drives by input events and real-time waits instead, and can
report frame rate with the frontend's own per-phase breakdown. That gap is why
the wide field shipped broken twice: a `.ppm` dump cannot show lag, a mismapped
click, or a HUD in the wrong place, which were exactly the faults reported.

## The numbers

In gameplay, verified by capturing the screen at measurement time — the first
attempt compared a run in a menu against a run in a battle and produced a
nonsense result (the wide field appearing to *double* the frame rate):

    wide field OFF   26.1 fps    emu_ticks 0.351 ms/frame
    wide field ON    21.5 fps    emu_ticks 0.354 ms/frame

`emu_ticks` per frame agreeing to three decimals across the two runs is the check
that the workload really was the same.

So the wide field costs about **4.6 fps, roughly 18%** — real, worth reducing,
and *not* the reported problem. **The baseline is 26 fps**, which is 43% of the
DS's own frame rate, with every enhancement switched off.

The instrumented phases — present, adaptive, upload, draw, swap — sum to about
**0.4 ms of a 38 ms frame**. Over 99% of the frame is inside emulation and
invisible to the frontend's instrumentation.

## Where it goes

`dispatch_stats`, in gameplay:

    ARM9   cache_hit            16,598,548
           cache_hit_absent    108,318,144
           cache_slow_lookup       946,069
           crs_hit               3,956,437
           crs_miss             18,136,222
           crs_scan_iters      123,672,439

**CORRECTED.** The reading below was wrong, and `dispatch_stats.h` says so
plainly: `cache_hit` and `cache_hit_absent` are BOTH fast hits -- the cached
slot answering *present* or *absent* without a search -- and `cache_slow_lookup`
is the full search. So:

    fast (present)      16,598,548   13.19%
    fast (absent)      108,318,144   86.06%
    SLOW full search       946,069    0.75%

**The dispatch cache is not the problem: 99.25% of lookups take the fast
path.** Reading `absent` as a miss inverted the result of the one measurement
this document exists to report.

What does stand out is the call-return stack: **82% of returns miss** (18.1M
against 3.96M hits) and `crs_scan_iters` divided by total returns is **5.6
entries examined per return**. `profile_report.cpp` computes exactly that
average, which is a fair sign the depth cost is known to matter.

The original, incorrect reading follows.

Two ratios stand out. The dispatch cache reports **6.5 absent lookups for every
hit**, and the call-return stack **4.6 misses per hit**, with 123 million scan
iterations behind those misses — roughly seven scan steps per miss, every miss.

Read carefully: these are the counters' own names, and what "absent" means
precisely is worth confirming in the dispatch source before building on it. What
is not in doubt is the shape — the hot path is dominated by lookups that do not
find what they want, and by a scan that runs a hundred million times.

## What this means for the work already done

The dirty-rectangle rework of the wide field (and the STATIC-texture fix that
followed it) reduced that feature's marginal cost, which was worth doing. But it
was aimed at a frame budget the compositor barely participates in, and no amount
of further work there reaches 26 fps.

Optimising a component before measuring which component is slow is the mistake
here, and it was avoidable: the frontend has exposed a per-phase breakdown the
whole time.

---

# FOUND: the per-instruction hook, and it is 41%

The counters above describe composition, not time, and reading them led nowhere:
the dispatch cache turned out to be 99.25% fast, and the call-return scan is 5.6
entries on an average return -- a couple of million comparisons a second, which
is not a 38 ms frame.

`runtime_arm.cpp` was more useful than the statistics. The recompiled banks gate
a **per-instruction hook** on `g_runtime_insn_trace`, on by default. The hook
bumps a retired-instruction counter and checks an armed flag -- cheap in itself,
but it is a cross-translation-unit call on *every emulated instruction*, so it
cannot inline.

Measured, five 5-second samples per arm, same scene, state confirmed by
screenshot at measurement time:

    hook ON    43.3  41.5  42.7  42.0  42.7      mean 42.4 fps
    hook OFF   59.9  59.7  59.8  60.0  59.7      mean 59.8 fps

**41% faster, and full 60 fps** -- the DS's own frame rate.

It is now off by default in this port, `LB_INSN_HOOK=1` to restore it. The
counters it maintains (`insn9`/`insn7`) exist for `run_to_event`, traversal and
selftest, none of which run while someone is playing, and the hook has no
control-flow or cycle effect, so clearing it changes timing only.

## The measurement mistake that hid this

Earlier samples of the same build ranged 21 to 37 fps and I treated them as
comparable. They were single 6-second samples taken immediately after the warp.
Five consecutive samples *within* one run vary by **±0.3 fps** -- the measurement
is precise; the runs differ, because a sample taken right after a level load
catches work that is not steady-state gameplay.

That variance is why "wide field OFF 26.1, ON 21.5" was reported as an 18% cost.
Those two numbers are one noisy sample each and cannot support that conclusion.
The wide field's true cost has not been measured and is not known.
