# 204 — Where the frame actually goes

Status: **measured, with a correction to the measurement itself.** The port runs
at 38–46 fps in a Free Play battle. This document says where the time goes and
what the widening does and does not cost.

## The harness was lying, by a factor of 100

`fps.txt` reported the frontend's phase breakdown like this:

    42.23 fps  (254 frames in 6.01s)
       emu_ticks          0.216 ms/frame
       present_ticks      0.020 ms/frame

0.216 ms to emulate an entire DS frame is not a plausible number, and it should
have been challenged the moment it appeared. It sent this investigation through
three innocent suspects in turn — the frontend compositor, the audio pacing, and
the scheduler — on the theory that "97% of the frame is unaccounted for".

The counters are **SDL performance-counter ticks, not nanoseconds**. The harness
divided by 1e9, assuming a 1 GHz counter. Windows reports **10 MHz**. Every
phase printed 100x too small. `frontend_stats` has always returned `freq` in the
same response; the harness simply never read it.

Corrected, at 37.88 fps (26.4 ms/frame):

    (counter frequency 10000000 Hz)
       emu_ticks         24.097 ms/frame     <- 91% of the frame
       present_ticks      2.177 ms/frame
       adaptive_ticks     1.396 ms/frame
       swap_ticks         0.399 ms/frame
       draw_ticks         0.104 ms/frame
       upload_ticks       0.070 ms/frame
       drain_ticks        0.003 ms/frame

**The emulator is the bottleneck, plainly and by a wide margin.** The frontend,
the compositor and the audio pacing together are under 3 ms.

This is the second time in this project a measurement tool produced a confident
wrong answer. The rule stands: a number that implies something impossible is a
bug in the instrument until proven otherwise.

## What the emulation time is made of

`NDS_PROFILE_SCHED=1` with the `profile` debug command, over a full run:

    arm9_ns        56.0%      ~13.5 ms/frame
    devices_ns     23.9%      ~5.8 ms/frame   (display 13.2%, spu 1.6%,
                                               sysev 1.3%, rtc 1.2%, wifi 1.1%)
    arm7_ns        13.1%      ~3.2 ms/frame
    next_event_ns   2.6%
    switch_ns       0.2%

To reach 60 fps the frame must come down from 26.4 ms to 16.7 ms — about
**7.4 ms out of the 24.1 ms of emulation**, or a 1.8x speedup of the emulator.

## Where the ARM9 time goes, and what it is not

Several plausible-looking culprits were measured and cleared:

* **The call-return stack.** `crs_scan_iters` is 222 M, the largest counter in
  the dispatch stats, with a 71% miss rate. But 40.1 M lookups at ~5.5 iterations
  each over ~96 s of wall time is well under 1% — a linear scan of a stack
  averaging seven deep is not the problem.
* **The trace ring.** `runtime_trace_event` is called on every call, return and
  cancel, but it early-outs on `g_runtime_deep_trace` and is off in play.
* **The per-instruction hook.** The generated banks emit the inlined form,
  `if (g_insn_hook_armed) runtime_insn_slow();`, not an unconditional call.
* **The ARM9/ARM7 timing ternary.** Already folded: banks are compiled with
  `NDS_STATIC_CPU`, which makes `g_nds_active` a compile-time constant, so
  `arm9_cycle_combine` vs `arm7_cycle_combine` is resolved at build time.
* **Threaded 3D.** `NDS_3D_THREADED` already defaults on in interactive mode.
* **Audio pacing.** `drain_ticks` is 0.003 ms/frame. The frame loop never waits
  on the audio queue; the earlier `SDL_Delay(1)` 15.6 ms theory is dead.

What remains is the emitted shape itself. Every guest instruction emits:

    g_cpu.R[15] = <pc>;
    if (runtime_should_yield()) return;
    ++g_insn_count[g_nds_active];
    if (g_insn_hook_armed) runtime_insn_slow();
    uint32_t _code = runtime_code_cycles(<pc>);
    ...
    runtime_tick(arm9_cycle_combine(...));
    if (runtime_unwinding()) return;

`runtime_should_yield`, `runtime_code_cycles`, `runtime_tick` and
`runtime_unwinding` are all **out-of-line, cross-translation-unit calls**. At
468 M ARM9 instructions per run that is roughly 1.9 billion calls, and the cost
is not only the call overhead — each one is opaque to the optimizer, so guest
state cannot stay in host registers across an instruction boundary.

Their bodies are small and dominated by a fast path:

* `runtime_unwinding()` is `return g_unwinding;` — one global load.
* `runtime_tick()` adds to a counter and checks a cached IRQ-pending flag.
* `runtime_should_yield()` returns false after two predicates when
  `g_cpu_fast_poll` is set and the hint is clear.
* `runtime_code_cycles()` classifies the PC's region.

The obvious next step is to give the generated banks inline fast paths for these
— guarded on `NDS_STATIC_CPU` so only bank translation units are affected and
the runner's own callers keep the out-of-line functions. The blocker is that
several of the globals involved (`g_unwinding`, `g_deferred_cycles`) live in
anonymous namespaces and would have to be given external linkage first.

**Not attempted yet.** It is emulator-core surgery on shared runtime code, and
it should be done one helper at a time with a gameplay run after each.

## What the widening costs

Measured by A/B on `display.adaptive_width`, same build, same route:

| adaptive_width | fps    |
|----------------|--------|
| 448            | 42.23  |
| 256 (native)   | 45.72  |

**About 8%.** `adaptive_ticks` is 1.40 ms/frame directly. The port is ~46 fps
even at native DS width, so the widescreen work is not what makes it slow, and
turning it off would not get anywhere near 60.

The shroud work added to this: the per-frame scan reads 32x24 tiles from two
layers, and the terrain diff (docs/203) runs only on report frames. Neither is
visible against a 24 ms emulation cost.

## -O3 on the generated banks: no measurable gain

The banks are the hot code — 56% of emulation is ARM9 and that is almost all
recompiled bank C — and they build at `-O2`, so `-O3` is the obvious thing to
try. `LB_BANK_OPT` was added to make it an A/B rather than an argument; it
defaults to `-O2`, so an unset environment builds exactly as before.

Rebuilt 293 bank objects at `-O3` and measured three runs against two:

    -O2   42.27 fps  21.484 ms emu
          41.91 fps  21.697 ms emu

    -O3   43.61 fps  20.814 ms emu
          44.59 fps  20.332 ms emu
          41.46 fps  21.836 ms emu

Means differ by about 3%, and **the ranges overlap** — the slowest `-O3` run is
slower than both `-O2` runs. At this sample size the difference is not
distinguishable from run-to-run noise, which has been wide all session (37.9 to
44.6 fps on identical builds).

So: no measurable gain, and nothing to justify the longer build. The default
stays `-O2` and `LB_BANK_OPT` remains available for anyone who wants to re-run
the comparison with more samples.

Worth saying plainly: even taking the 3% at face value, 42 fps needs **1.8x** to
reach 60. Compiler flags are not the lever, and neither is anything else tried
so far — the per-round scheduler cost bought 2% (`NDS_ITER_CAP`), and the
per-instruction helpers were estimated at ~10% before the work of inlining them.
The cost is structural: every guest instruction carries a PC store, a yield
check, a retired-instruction counter, a memory-timing lookup, a cycle
accumulation and an unwind check, several of them out-of-line calls that also
prevent guest state living in host registers. Reaching 60 fps means changing
that accounting model, not tuning around it.

## Inlining the per-instruction predicates: ~5%, and it is real

Every emitted guest instruction calls `runtime_should_yield()` and
`runtime_unwinding()`, both out-of-line and in another translation unit — about
a billion calls a run — and both are dominated by a fast path. Giving the
generated banks inline versions, guarded on `NDS_STATIC_CPU` so only bank
translation units see them and the runner's own callers keep the ordinary
functions:

    baseline   42.27 fps  21.484 ms emu
               41.91 fps  21.697 ms emu

    inlined    44.31 fps  20.469 ms emu
               44.44 fps  20.403 ms emu
               44.15 fps  20.506 ms emu

**+5.3% fps, -5.2% emulation time.** Unlike the `-O3` comparison, the ranges do
not overlap, and the inlined runs sit within 0.1 ms of each other — a much
tighter spread than anything else measured this session, which is itself part of
why the result is believable.

Semantics are unchanged. The inline `should_yield` reproduces the existing fast
path exactly and defers to the real function whenever that path does not settle
it; `runtime_unwinding` was already a single global read. Verified by a gameplay
run: the route reaches a battle, the fog table learns, and the widened columns
render.

Two things this needed, both worth knowing before repeating it:

* `g_unwinding` lived in an anonymous namespace and had to be given external
  linkage.
* The banks compile as **C**, so every global the fast paths touch needs C
  linkage on its DEFINITION too. Without that the failure is at link time —
  `undefined reference to g_cycle_cap` — because the bank objects reference an
  unmangled name the C++ definition never provides.

Still to try on the same principle: `runtime_tick` and `runtime_code_cycles`,
the other two per-instruction calls. `runtime_tick` needs `g_deferred_cycles`
out of its anonymous namespace; `runtime_code_cycles` is the most involved,
since it classifies the PC's region.

Even so: 5% against the 1.8x needed. This is the right direction and it is not
close to sufficient on its own.

## runtime_tick: inlining it made things WORSE

The obvious follow-up was to inline `runtime_tick` the same way. It is the third
per-instruction call and its fast path is two global updates plus a cached IRQ
check, so it looked like the same shape of win.

    predicates only          44.31 / 44.44 / 44.15 fps    20.469 / 20.403 / 20.506 ms
    predicates + tick        44.14 / 43.43 fps            20.545 / 20.867 ms

**Slightly worse, consistently.** `runtime_tick`'s body is much larger than a
single global read or two cheap predicates, and expanding it at every one of the
hundreds of thousands of instruction sites in the generated banks costs more in
code size — and instruction cache — than it saves in call overhead.

Reverted. The lesson generalises and is the useful part: **not every
per-instruction call is worth inlining, and size is the discriminator.** The two
predicates are a global load and a couple of compares; `runtime_tick` is a
multi-statement body with a conditional call inside it. Only measurement
separates them, which is why this was done one change at a time rather than all
four at once.

`runtime_code_cycles` is larger still — it classifies the PC's region through
several branches — so on this evidence it is a poor candidate and is not worth
attempting without a reason to expect otherwise.

## Where the frame rate stands

    baseline (start of session)   ~42.1 fps   21.59 ms emu
    inlined predicates            ~44.5 fps   20.13 ms emu

About **+6%**, all of it from inlining two small predicates in the generated
banks. Everything else tried measured as noise or worse: `-O3` on the banks,
a 4x coarser scheduler rendezvous (2%), and inlining `runtime_tick`.

60 fps needs 16.7 ms a frame against the ~20.1 ms of emulation now measured,
so roughly another **1.2x on the emulator alone** — and that is after the
frontend's ~2.5 ms, which would also have to fit. The remaining structural cost
is the per-instruction accounting itself: a PC store, a retired-instruction
counter, a memory-timing lookup and a cycle accumulation on every guest
instruction, none of which a faster call sequence removes. Reaching 60 means
not doing that work per instruction — block-level cycle accounting rather than
per-instruction — which is a change to the recompiler's model, not to this port.

## 2026-09-13 — the trace guard, tried and reverted

The "not attempted yet" list above proposed inline fast paths for the
per-instruction helpers. `runtime_trace_event` looked like the easiest of them:
it is emitted on every memory access, its first statement is
`if (!g_runtime_deep_trace) return;`, and unlike the helpers this document
names as blocked, `g_runtime_deep_trace` is already `extern uint32_t` in
`runtime_arm.h` — so hoisting the guard needed no linkage surgery at all.

Measured, 512x384, same route, `emu_ticks` being the stable figure:

    without   44.07 fps   20.614 ms/frame
    with      41.72 fps   21.818 ms
              40.96 fps   22.223 ms
              41.50 fps   21.940 ms

**Worse, consistently.** Reverted. Memory accesses are far more numerous than
the two predicates that did pay off, and a load plus a branch at every one of
them costs more in code size than the call it removes — the same outcome as
`runtime_tick`, for the same reason.

Two of the three candidate helpers have now been tried and both lost. The
remaining one, `runtime_code_cycles`, has a body larger than either, so on this
evidence it should be expected to lose as well. The per-instruction-inlining
idea is close to exhausted; a 1.8x emulator speedup will have to come from
somewhere else.

## Re-measured on the current build

Unchanged in substance. At 512x384, 44.07 fps:

    emu_ticks         20.614 ms/frame    (91%)
    present_ticks      2.012
    adaptive_ticks     0.922             (the widening: 4%)
    swap/draw/upload   0.381 / 0.080 / 0.134

And the widening's full cost, measured like-for-like at 996x720:
25.06 ms/frame with it against 22.18 ms with `LB_WIDEMAP=0` — **2.9 ms**, which
includes the per-frame shroud scan and fog checks that run inside the vblank
hook and are therefore counted in `emu_ticks` rather than `adaptive_ticks`.

The compute (OpenGL) renderer was briefly suspected, because it refuses every
width this port uses: `kComputeMaxWidth` is 448 and the width must be a
multiple of 64, so of 306/386/484/498/512/626/786/946/996 not one qualifies and
every session falls back to the threaded software rasteriser. It is **not** the
bottleneck: `gpu3d` totals 42 ms across 4756 frames. The 3D work in this game
is negligible and moving it to the GPU would buy nothing.
