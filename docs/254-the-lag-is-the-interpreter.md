# 254 — The lag is the interpreter, not the port's rendering

The problem: the game was very laggy and choppy, far below the target of a
consistent 60 fps minimum. Excessive logging or recording were suspects.

Two previous attempts to fix it moved nothing:

| change | before | after |
|---|---|---|
| cardinal fog-edge table (removed up to ~18M iterations/frame) | 38.7 fps | 38.5 fps |
| band fog-fill cache keyed on camera + visibility hash | 38.5 fps | 37.69 fps |

Both were chosen because a **counter** was large. That is the error this
document exists to record: `crs_scan_iters`, `cache_hit_absent` and the
per-cell fog scans all say how OFTEN something happens, and none of them says
how LONG it takes. Optimising by count produced two no-ops.

## What the frame timer said, and why it misled

`frontend_stats` reports:

```
37.69 fps  (227 frames in 6.02s)
   emu_ticks         23.012 ms/frame
   present_ticks      3.263 ms/frame
   adaptive_ticks     1.657 ms/frame
```

`emu_ticks` brackets exactly one thing (frontend.cpp:2484-2503): the
`while (... < next_frame) scheduler_run_round();` loop. So 23 ms of a ~26 ms
frame is inside the scheduler. That correctly rules the SDL presentation layer
out, and it is where the reading stopped before — "the emulator is slow" is not
an actionable statement.

`NDS_PROFILE_SCHED=1` splits the round up, and this is where it goes wrong:

```
sampled_rounds 20295   rounds 20477511   switches 1940426
round total 3.514 us/round
  arm9_ns     2.066 us/round   58.8%
  devices_ns  0.807 us/round   23.0%
  arm7_ns     0.363 us/round   10.3%
```

That looks like a flat profile with no hot spot — ARM9 execution is simply
expensive. **It is not a flat profile; it is an aliased one.** The sampler fires
on `(g_profile_rounds++ % 1009) == 0`, and the heavy per-frame work happens in
ONE specific round out of the ~8750 in a frame (kIterCap is 64 system cycles,
a frame is 560,190). A 1-in-1009 stride systematically under-samples anything
that is periodic at frame rate. Every conclusion drawn from this table about
*where* the time goes is unsupported.

## What a sampling profiler said

gdb is in `C:\msys64\ucrt64\bin`. Attach to the live process, take
`thread apply all bt`, aggregate the leaf frames
(`scratchpad/pmp.py`). Two passes, because the first one was read wrong:

**Pass 1 — all threads.** 62.4% of leaves came back as
`ntdll!ZwWaitForSingleObject` under `nvwgf2umx`/`nvoglv64`, and
`lb_adaptive_fb_inner` looked like 36% of the remainder. Both readings are
junk: the process has ~45 threads, nearly all of them idle NVIDIA driver
workers, and they bury the one thread doing the work. **Retracted:
`lb_adaptive_fb_inner` is not 36% of anything.**

**Pass 2 — main thread only** (the thread whose stack reaches `main` /
`nds_run_interactive_frontend`), 40 samples:

```
--- ON STACK ANYWHERE ---
  100.0%   nds_run_interactive_frontend
  100.0%   main
   85.0%   scheduler_run_round
   77.5%   runtime_dispatch
   52.5%   tier3_run
   10.0%   armv4t::Interpreter::step
   10.0%   nds_gpu2d_adaptive_framebuffer
   10.0%   lb_adaptive_fb_inner

--- LEAF ---
   10.0%   tier3_run
    7.5%   armv4t::Interpreter::step
    5.0%   runtime_dispatch
    2.5%   armv4t::ArmDecoder::decode
    2.5%   armv4t::instr_cycle_base
    2.5%   lb_adaptive_fb_inner
```

`tier3_run` is on the main thread's stack in **52.5%** of samples, with
`armv4t::Interpreter::step` and `armv4t::ArmDecoder::decode` beneath it.

## What tier 3 is

runtime_arm.cpp:1067-1072:

```c
// Tier 3: code copied into RAM at runtime (firmware boot, menu, and the
// ITCM-resident IRQ handler) has no static bank -- run the guest's OWN
// bytes through the interpreter (PRINCIPLES.md "the one exception"),
// never an HLE model.
if (bus_range_has_write_provenance(pc, thumb ? 2u : 4u)) {
    tier3_run(pc);
    return;
}
```

So: any PC whose bytes were **written by the guest** rather than loaded from a
statically-lifted bank is decoded and interpreted, one instruction at a time.
In a Nitro game that is precisely the performance-critical set — routines the
game copies into ITCM at boot, and the IRQ handler.

This also re-reads `cache_hit_absent = 270,000,665` correctly. It is not a
cache performing badly; it is 270 million confirmations that **there is no
recompiled function at this PC**, each one followed by an interpreted
instruction. The counter was a symptom of tier 3, and it was sitting in the
stats dump the whole time.

## Why this is the answer to "60fps minimum"

The port is a *recompiler*. Recompiled ARM9 code should retire on the order of
10⁸ instructions/second; an interpreter with a decode step per instruction runs
1-2 orders of magnitude below that. Over half the main thread is in the
interpreter. No amount of tuning in `lb_widemap.cpp` or `lb_adaptive_fb_inner`
(10% on stack, 2.5% leaf) can reach 60fps while that is true — which is exactly
why the two previous attempts measured 38.7 → 38.5 → 37.69.

`port/config/lb_arm9_itcm.toml` already exists, so ITCM is nominally lifted;
the next step is to find which PCs tier 3 is actually executing
(`tier3_coverage` on the debug server, or `--coverage-manifest`, which captures
the guest's own code pages for static lifting) and close the gap.

## The player's own hypothesis was half right

*"Not sure if it's due to excessive logging or recording or what"* — there was
excessive logging, and it was ungated.

`lb_widemap_fog()` is called **per pixel**. Inside it sat a block with no env
flag in front of it at all, so it ran in the shipped build:

```c
++g_fogserve_n[k];
if (fogs) ++g_fogserve_fog[k];
static unsigned last = 0;
unsigned all = 0;
for (int i = 1; i <= 4; ++i) all += g_fogserve_n[i];   // every pixel
if (all - last >= 200000u) { ...fprintf(stderr, ...); fflush(stderr); }
```

Its own counters report the call volume: LEFT 50M, RIGHT 96M, ABOVE 131M,
BELOW 262M — **540 million calls** in one Free Play run, each paying two
increments and a four-iteration re-sum, plus ~5000 `fprintf`+`fflush` pairs.
`ucrtbase!fwrite` (5.0% on stack) and `ntdll!ZwWriteFile` (2.5% leaf) in the
main-thread profile are this. It is now behind `LB_FOGSERVE`, and the band-kind
ladder feeding `fog_record` moved behind the `g_band_truth_on` flag that
`fog_record` itself already checked.

**Measured result: the log volume collapsed and the frame rate did not move.**

```
stderr per run   856,003 bytes  ->  65,905 bytes      (13x less)
fps before       29.3  31.5  33.8  34.8  37.69        mean 33.4
fps after        31.3  33.3  33.0  32.7               mean 32.6
```

So this is a correct cleanup of real ungated per-pixel work, and it is **not
the lag fix**. Claiming it as one would be the third counter-driven no-op in a
row. The one suggestive signal is spread — 8.4 fps before, 2.0 fps after — which
would match removing a variable-cost I/O path, and choppiness is a symptom of
jitter rather than mean. Four samples against five is not enough to assert it.

## What tier 3 is actually executing — and the fix that would have been wrong

With the coverage read on the harness's own connection, one Free Play run:

```
ROOT       22422 entries   21882484 hits   66.0%
CALL        2516 entries   10684586 hits   32.2%
INDIRECT    1111 entries     609979 hits    1.8%

50% of ROOT hits in the top 36 addresses
90% of ROOT hits in the top 791
```

`ROOT` is the dispatcher handing a PC to the interpreter, so those are the
addresses to explain. The top of the list, all ARM9:

```
0x02026ACC  905471   0x020F8F68  736802   0x02110768  418801
0x02026ADC  905469   0x020A16A0  698856   0x0208985E  404102
0x02026D30  905463   0x020A16AA  698856   0x021120E0  385105
```

The first reading of this was that `lb_arm9.toml` sets
`authoritative_entry_points = true` ("the recompiler must NOT run its own
speculative finder"), and that lifted functions jump from `0x020267F8` to
`0x02026FA0`, leaving a 1960-byte hole containing the three hottest addresses —
so the fix was to append `[[entry_point]]` stanzas for the top 36 and re-lift.

**That was wrong, and appending those entry points would have corrupted the
lift.** There is no hole. `lb_code.py` reports:

```
; func_020267f8  @0x020267f8  arm  size=0x7a8
```

0x020267F8 + 0x7A8 = 0x02026FA0. The function already covers every one of those
addresses. Disassembling them says what they really are:

```
0x02026ac8  bl   #0x2026170        -> func_02026170
0x02026acc  mov  fp, r0                    <-- "hot ROOT address"

0x020f8f64  b    #0x20f8f74
0x020f8f68  ldr  r1, [r1, #0xc]            <-- branch target, skipped by the b

0x02110760  b    #0x2110768
0x02110768  tst  r1, #1                    <-- branch target

0x021120dc  blx  r5
0x021120e0  cmp  r0, #0                    <-- return site of an indirect call
```

Every one is a **return site or an internal basic-block head inside a function
that is already lifted**. None is a function entry. They reach the dispatcher
because an indirect transfer (a return, a `blx` through a register, a computed
branch) targets them, `lookup_static` finds no function *starting* there, and
the runtime has no way to re-enter a compiled function mid-body — so tier 3
interprets the remainder. Declaring them entry points would tell the
recompiler that the middle of `func_020267f8` is a separate function, which is
false, and the project does not guess at code structure the ROM does not
show.

So the mechanism is: **every indirect transfer that lands mid-function costs an
interpreted tail.** `crs_miss = 32,881,354` against `crs_hit = 16,230,044` — a
67% miss rate on `runtime_call_should_return` — is how often that happens. A
CRS hit returns into compiled code; a miss does not. That ratio, not a missing
bank, is the thing to attack next, and it is not yet explained.

Free Play, 1920x1080, same route, back to back, **no code change between them**:

```
29.3  33.8  34.8  37.69 fps
```

That is a 29% spread on the baseline. An A/B of `NDS_ITER_CAP` in this investigation
read 29.3 (64) → 34.7 (256) → 34.3 (1024) and looked like an 18% win; it sits
entirely inside the baseline spread and **is not supported**. Nothing may be
claimed as a speedup from this harness without repeats on both arms. The spread
is itself a finding: the symptom was choppiness.

## Retractions

- `lb_adaptive_fb_inner` is ~10% on stack / 2.5% leaf, not the 36% the
  all-threads histogram appeared to show.
- `NDS_ITER_CAP=256` is not a measured 18% gain; it is inside the noise floor.
- The `NDS_PROFILE_SCHED` breakdown does not show a flat profile, it shows an
  aliased one, and must not be used to argue that ARM9 execution has no hot
  spot.

## RETRACTED: "any tier-3 command kills the debug server"

An earlier revision of this document, and the commit message of b8943d13,
claimed that `static_coverage` and `tier3_coverage` race `coverage_note()` on
the main thread and kill the debug server's listener — "reproduced three times
across three separate processes". **That is wrong.** There is no race and no
defect. Struck here rather than deleted, because it was the stated reason for
adding `--exe-arg`.

The debug server is **strictly single-client** (debug_server.cpp:1658-1698):

```c
if (listen(listener, 1) != 0) { ... }        // backlog of ONE
while (!fatal_backend_failure) {
    socket_t client = accept(listener, nullptr, nullptr);
    ...                                       // serve until this client hangs up
    CLOSESOCK(client);
}
```

`tools/lb_exp.py --keep` parks the harness's own `Dbg` connection and never
closes it, so it holds the only served slot for the life of the process. A
second client's `connect()` completes at the TCP layer into the one-deep
backlog but is never `accept()`ed — which presents as a `readline` timeout, not
an error. A third `connect()` finds the backlog full and is **refused**. That
is the exact sequence observed, and it is explained entirely by the connection
discipline. The command name was incidental; the first command sent happened to
be a tier-3 one each time.

Two facts that should have stopped the wrong conclusion earlier:

- The run that was "refused immediately" logged
  `[debug] play-mode surface on 127.0.0.1:19857` — the server was up and
  listening the whole time.
- `coverage_manifest_write()` calls `tier3_coverage_copy()` and `tier3_stats()`
  itself (coverage_manifest.cpp:330-333). If reading tier-3 state were unsafe,
  the `--coverage-manifest` at-exit path proposed as the *workaround* would
  race identically.

**The actual way to read tier-3 coverage** is to issue it on the harness's own
connection — the same place the run already dumps `profile`,
`dispatch_stats` and `event_counts` — rather than opening a second client.
`--exe-arg` is kept because passing runner flags is independently useful, but
it was not needed for this.
