# 264 — The last millisecond, and the gates before deploying

docs/263 took the port from 36.4 to 58.8 fps by lifting the 32% of the ARM9 the
finder had been dropping. This is the rest of the way to the DS's own 59.826 Hz,
and the checks that had to pass before any of it reached the player.

## First: is the shortfall budget, or is it the limiter?

At 58.8 fps the mean frame already fit — `emu 10.68 + present 5.38 = 16.06 ms`
against a 16.71 ms target, with `drain_ticks` (the limiter idling) at 0.82 ms.
A mean that fits and a rate that misses is either jitter or a limiter that
cannot hit its own target, and those want opposite fixes.

Answered without building anything, by running the **same binary** at a smaller
window so the adaptive surface shrinks from 946 to 626 wide:

```
1280x720    59.73 59.81 59.49 59.83     total 13.64 ms   drain 3.04
1920x1080   59.31 58.79 58.75 59.13     total 16.06 ms   drain 0.82
```

**59.83 is native.** The limiter reaches it exactly when given slack, so the
limiter is fine and the shortfall is budget. Cutting presentation cost was
therefore worth doing, and this cost four runs rather than a 40-minute build.

## A wrong turn, recorded because it was stated

Reading the compositor, it looked like an unthreaded per-scanline loop worth
~2.5 ms, with `nds_gpu3d_wide_line()` — which scrolls into a shared global and
returns a pointer to it — as the blocker to fixing that.

**It was already threaded.** `lb_parallel_rows` in `lb_wide2d.inc` splits the
row loop across workers, with the per-row scratch moved inside the parallel body,
`lb_hud_scan` hoisted out ahead of the fan-out, and an `LB_THREADCHECK=1` that
re-composites the same frame single-threaded and memcmps. Its comment already
records why two separate runs cannot answer that question. The 3.5 ms was the
*threaded* cost, not a serial one waiting to be parallelised.

Staged `LB_AD_NOOBJ` / `LB_AD_NOBG` cost probes were dropped: they anchored on
upstream text that the threading hook rewrites, so they would have failed the
build loudly or priced a different program.

## What actually cost the frames: a constant whose reason had expired

```c
// The emulator core and the 3D render thread want cores too, and
// the gain flattens early on a loop this memory-heavy.
if (workers > 6) workers = 6;
```

Swept at 946x516, from one worker up to the test machine's core count, reading `adaptive_ticks`, which is
reproducible to three decimals here where fps is not:

```
LB_THREADS     1       6       8      10      12
adaptive    8.269   3.523   2.999   2.727   2.760 ms
emu        10.320   10.56   10.77   10.80   10.73 ms
```

The knee is 10, and 12 is *worse* than 10 — that is the emulator thread being
crowded off. Four runs per arm, 6 against 10, **non-overlapping on all three**:

```
            fps                       adaptive                  drain
 6   58.83 58.65 59.15 58.97      3.523 3.523 3.510 3.491   0.923 0.737 0.729 0.776
10   59.73 59.36 59.46 59.27      2.728 2.725 2.700 2.715   1.325 1.368 1.405 1.395
```

`emu_ticks` **overlaps** (10.555-10.791 against 10.743-10.798), and that is what
retires the comment rather than contradicting it. It was written when
`emu_ticks` was 19.7 ms and the core really was the larger claim on the machine;
docs/263 took it to 10.7 ms, and the cap then cost frames. The default is now
`hardware_concurrency - 2`, which leaves those two threads their cores and takes
the rest. `LB_THREADS=N` still overrides.

`LB_THREADS=1` measures 48.58 fps with `drain_ticks` at **0.002 ms** — no slack
at all. That is what this path costs unthreaded, and it is why misreading it as
serial would have been so misleading.

## The gates

**Threading correctness.** Not cross-run image hashes: two runs of the same
build differ in 32 of 36 presented surfaces, because the harness captures on
wall-clock and a faster build is simply further along in a live battle. Same
frame, both code paths, memcmp:

```
[lb_threadcheck] 946x516: 2520 frames, 0 differed (MUST be 0), worst 0 of 488136
[lb_threadcheck] 996x546: 2760 frames, 0 differed (MUST be 0), worst 0 of 543816
```

2,760 frames across eight surface sizes, zero differing pixels.

**Netplay**, an explicit requirement. Two instances over local
wireless:

```
desync: sample 1 in sync across 2, 16 entities
desync: sample 2 in sync across 2, 16 entities
desync: the world changed in 1 of 2 comparisons, so this was a moving match
desync: 2 sample(s), 2 compared two live battles, IN SYNC throughout
```

The moving-match line matters: a frozen battle would pass a sync check while
proving nothing.

**Vanilla**, the standing deploy gate. `"enhanced": false`, identified by the
presented surface rather than the banner (docs/256):

```
59.82 / 59.91 fps   emu 9.099   present 1.137   drain 6.41   presents 256x192
```

**Default configuration.** Measured with no environment override at all,
because three false "it works" readings in this project came from testing a
harness config instead of the deployed one:

```
59.08 59.63 59.44 58.96 fps    adaptive 2.725 2.701 2.714 2.704
```

`adaptive_ticks` matches the forced `LB_THREADS=10` arm, so the new default is
genuinely in effect.

## Where it lands

```
                 before          after           native
enhanced         36.4 fps        59.3 fps        59.826 Hz
vanilla          47.6 fps        59.9 fps
emu_ticks        22.332 ms       10.8 ms
present_ticks     5.501 ms        4.6 ms
drain (idle)      0.5 ms          1.4 ms
```

One caveat that does not show up at these window sizes: on an ultrawide
3440x1440 display the adaptive surface is ~1706 wide rather than 946, so the
compositor costs roughly 1.8x what these numbers show. The worker-count fix
matters more there than here, and `LB_THREADS=N` is the runtime knob if it
still falls short.

## Audio is the sensitive instrument, not fps

Audio fidelity is a goal and it had never been measured — the runner
prints `underruns` only in the `[sdl] soak` summary at clean shutdown, and this
harness kills the process, so grepping a run log finds nothing and that silence
reads exactly like a pass. `frontend_stats` has carried the counter all along;
`tools/lb_exp.py` now records it in `fps.txt`, total and per-window.

```
headroom 3.2 ms   (1280x720,  59.82 fps)     0 and 3 underruns / 6 s
headroom 1.4 ms   (1920x1080, 59.3 fps)      2 and 7
headroom 0.002 ms (LB_THREADS=1, 48.6 fps)   89 and 89
```

The queue holds `kAudioQueueFrames` = 2048 frames, about **63 ms**, and the
audio device consumes at exactly real time. An underrun is therefore not a small
hiccup: it means the emulator produced less than real time for long enough to
drain 63 ms. **Buffering more cannot fix it.** It reaches zero only when the port
genuinely sustains 59.826 Hz, which makes it a far more sensitive test than fps —
59.3 against 59.83 looks fine on the rate and starves the queue anyway.

Against the old build this is a large improvement, not a regression: that one ran
at 36.4 fps, and even the 48.6 fps arm here produces 89 underruns per six
seconds.

## The direct-present path cannot help this title

`nds_gpu2d_direct_present_frame_active()` bypasses the CPU compositor entirely,
and `direct_frames` is 0. The per-class counters are all zero too, but that is
NOT evidence about why: they only increment under `if (profiling())`, which
needs `NDS_PROFILE_GPU`. Reading `direct_scene_class()` instead answers it
outright, and LEGO Battles fails two of its tests independently:

```c
if (extra_bg_mask != 0u) return NDS_GPU2D_DIRECT_EXTRA_BG;
if ((nds_powercontrol9() & 0x8000u) == 0u) return NDS_GPU2D_DIRECT_SCREEN_ROUTE;
```

The title's `DISPCNT` is 0x2F08, so BG1/BG2/BG3 are all enabled and
`extra_bg_mask` is 7 — the direct presenter handles a 3D-only scene, and this is
a 2D tilemap RTS with three text BGs. Independently, it "currently owns the
physical top window only", and this title widens the BOTTOM screen with engine A
pinned to it.

So the direct path is not a tuning switch that happens to be off; it is built
for a different kind of scene. Making it apply would mean writing a GPU
compositor for text BGs, OBJ and 3D together. Recorded so it is not costed a
second time.

## What 3440x1440 actually costs — measured, not extrapolated

An earlier draft of this file said this "could not be measured here", because
no test display was that wide. That was wrong, and the way around it
is cheap: **the compositor's cost depends on the adaptive SURFACE, not the
window**, and the surface is `window / magnification`. Setting magnification to
1.0 and opening a 1740-wide window produces the same wide
surface a 3440x1440 monitor produces at magnification 2 — the same pixel count
through the same code.

```
surface        fps     emu     present  adaptive  upload  swap   drain   underruns/6s
946x516      59.63   10.809     4.607     2.701   0.296  0.531   1.293        2-7
1728x768     48.64   11.742     8.740     5.426   0.531  0.679   0.001       77,76
```

**~49 fps at 3440x1440, with the audio queue starved.** Two things
the linear estimate got wrong, in both directions:

  * The compositor threads BETTER at size, not worse: 2.72x the pixels for
    exactly 2.0x the time. The estimate of ~6.6 ms was pessimistic; it is 5.4.
  * `emu_ticks` is NOT constant across surface size — 10.809 to 11.742. The 3D
    rasteriser renders at the widened width, so emulation scales with the
    surface too. Nothing in the estimate accounted for that.

The budget needs 20.48 ms to fit in 16.71, so ~3.8 ms has to go. Note also that
`adaptive + upload + draw + swap` is 6.735 of the 8.740 `present_ticks`: about
2 ms of the presentation phase is unaccounted for at this size and about 1 ms at
946x516, so it scales and has never been attributed. That is the first place to
look next, ahead of shaving the compositor further.

This is still a large improvement at that resolution rather than a regression —
the old build spent 22.3 ms in emulation alone at the SMALL surface, before any
of this scaling applied.

**The knob that works today, with no further build:** Magnification
(`pixel_scale`, the settings slider, 1.0-8.0). The surface is window /
magnification, so 3 gives a ~1147-wide surface instead of ~1720 — close to the
946x516 case measured above, which sustains native — at the price of chunkier
pixels. `LB_THREADS=N` also exists, but the default `hardware_concurrency - 2`
is already the measured knee.
