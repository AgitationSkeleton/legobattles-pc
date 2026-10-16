# 262 — What actually limits the frame rate, and why 60 is not a rendering problem

The goal: a stable 60 fps minimum, preserving visual, audio and netplay fidelity.

## Banked: the scheduler rendezvous, +8%

`kIterCap` is the fixed cost of a scheduler round — next-event selection, the
two CPU slice entries and exits, and five device ticks. At melonDS's 64 system
cycles a frame pays it about 8750 times.

Four runs per arm, on a 1920x1080 display:

```
64     34.4  32.6  33.6  34.1     mean 33.7   max 34.4
256    37.0  36.5  35.9  36.0     mean 36.4   min 35.9
```

Non-overlapping. Now the default. An earlier one-run-per-arm A/B of this was
**retracted as noise and deserved to be** — the harness spread 29-38 fps then.
It only resolves now because the spread is about 2 fps.

**512 was tried and rejected.** It gains nothing further (36.9, 37.8, 35.8 —
the same as 256) and one of its four runs collapsed to **9.4 fps**, with
`emu_ticks` at 46 ms and no halt logged. Unexplained, and a setting that
occasionally drops to 9 fps fails "stable" whatever the cause.

Fidelity at 256: no halt, desync or assert in any run, map coverage identical
to the 64 arm (23418/24576). The scheduler's own comment records that a wider
round once desynced the IPCSYNC handshake, so this is the first thing to
suspect if wireless play regresses; `NDS_ITER_CAP=64` restores the old
behaviour exactly.

## Tried and reverted: skipping device ticks with nothing due

The display's state only changes at an hblank or line boundary and the SPU's on
a 1024-cycle grid, and both are absolute-timestamp catch-up, so a tick below
its own deadline is a no-op by construction. Skipping those looked like free
money.

```
cap 256 alone        37.0  36.5  35.9  36.0     mean 36.4
cap 256 + devskip    37.4  36.6  35.7  35.5     mean 36.3
```

**No gain.** Reverted rather than carried — the change was safe and principled
and bought nothing measurable, and unneeded complexity in the scheduler is a
liability. Recorded so it is not tried a second time.

## The number that reframes the goal

Vanilla rendering — Enhanced off, so no widened surface, no band, no widemap,
no fog extension, the stock 256x192 presented as the cartridge draws it:

```
40.5   47.6   48.1 fps
```

**Even with every enhancement off, this does not reach 60.** The adaptive
rendering costs roughly 6-12 fps; the remaining ~15-25 fps of deficit is in the
emulator core and no amount of work on the band, the fog or the compositor can
recover it.

## So where the core's time goes

gdb sampling of the MAIN thread only (the process carries ~45 mostly-idle
NVIDIA driver threads that bury it otherwise), 50 samples:

```
scheduler_run_round    84%  on stack
runtime_dispatch       56%
tier3_run              26%
lb_adaptive_fb_inner   10%
```

and the leaf histogram is flat — the largest single leaf is 6%. There is no hot
spot left to cut. The cost is structural: every indirect branch goes through
`runtime_dispatch`, and a quarter of the thread is the armv4t interpreter
running guest code that has no recompiled bank.

`dispatch_stats` puts numbers on it: 41.7M dispatches, and **270M**
`cache_hit_absent` — because tier 3 calls `nds_has_bank()` on *every interpreted
instruction* to notice when it can hand back to a compiled function, and while
inside a tier-3 region that lookup always answers "absent".

## The honest assessment

Getting from ~36 to a stable 60 needs the emulated execution to be roughly 40%
faster, and the two places that can come from are:

1. **Eliminating tier-3 interpretation** — lifting the RAM-resident code so
   there is a compiled bank for it. Worth ~26% of the main thread if complete.
2. **Reducing dispatch cost or count** — 56% of the thread passes through it.

Both are recompiler-level work rather than port-level tuning, and neither is a
change that can be measured in an afternoon. What is *not* worth another
iteration is shaving the rendering path: vanilla already proves the ceiling is
elsewhere.
