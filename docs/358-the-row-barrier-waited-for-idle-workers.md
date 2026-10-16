# 358 — The row barrier waited for workers that had nothing left to do

Written for: whoever works on the frame's timing at wide windows next.

Status: **built, verified exact, deployed** 2026-09-24 03:40 (`54151c67`,
runner built 03:28, `build/LegoBattles.exe` sha256 `21ec0003...`, settings
kept). The composite's worst cases are gone; the widest window's frame tail
is not. That needs section 6's savings or the pending pipelining decision.

## 1. The widest window today, on a quiet machine

A replay at the widest surface this port makes: 1720x720, which is a
3440x1440 window at the default pixel scale 2. It is reproduced as
`LB_WINDOW=1720x720 LB_PIXEL_SCALE=1`, since the frame cost follows the
surface, not the window. The deployed
build, `43483414`, CPU load 2% before the run:

| | ms per frame |
|---|---:|
| emulation | 12.02 |
| present (of which the composite 2.72) | 3.88 |
| idle (pacing) | 0.61 |

60.27 fps, 11 audio underruns in the whole run. But 197 of the last 363
frames took more than 16.7 ms of work, 4 more than 20 ms, and the present
cadence has a standard deviation of 2.1 ms (0.5-0.8 ms at 1280x720 and
1920x1080). The frame fits with nothing to spare, which is why docs/353
measured 44-56 fps here under a heavier desktop load.

## 2. The main thread, sampled

`LB_SAMPLE=1`, same run conditions, the last 60 s (92,239 samples, 60.1 fps
with the sampler attached):

| | share | per frame |
|---|---:|---:|
| emulation (`run_slice`) | 54.2% | ~9.0 ms |
| the widened composite (the main thread's own rows) | 16.4% | ~2.7 ms |
| engine A's native scanlines | 7.1% | ~1.2 ms |
| engine B's native scanlines (the top screen, shown in the panel) | ~3.8% | ~0.6 ms |
| blocked in kernel waits | 4.8% | ~0.8 ms |
| the pacing sleep (`ZwDelayExecution`) | 2.8% | ~0.5 ms |

Engine A's 7.1% is three things:

- **the 3D snapshot copy**, 2.3% (~0.38 ms). `render_engine_line` copies
  each 1720-px row of the 3D raster into `g_wide_3d_frame` as its scanline
  passes, and the 528 rows below the guest's 192 in one go at line 191:
  about 5 MB a frame.
- **the native composite nobody sees**, ~3.9% (~0.64 ms): BG decode, OBJ,
  the compose, the write into the native frame. In a widened battle frame
  the widened composite replaces it on screen.
- the widemap scan (`lb_widemap_scan`, ~0.3%), which is the port's own and
  needed.

The kernel waits are `ZwWaitForSingleObject` (3.6%) and
`ZwWaitForMultipleObjects` (1.2%): the composite waiting at its barrier, and
`GetLine` waiting for the rasteriser (docs/353 s2-3).

## 3. The barrier

The composite hands its rows to ten persistent workers (`LbRowPool`,
src/lb_wide2d.inc), 8 at a time since docs/355. Each worker, on waking, took
chunks until none were left and then decremented a pending count; the
caller took chunks too and then waited for the count to reach zero.

**So the caller waited for every worker to wake and check in, including a
worker that woke after every chunk was taken and did nothing.** docs/355's
chunks moved a late thread's rows onto the threads that had started, but not
the late thread's start off the frame. Under load that start is 0.8-3.2 ms
(docs/353 s3).

Now, with chunks, the caller waits for the rows instead:

- One 64-bit word holds the call's generation (high half) and the next
  unclaimed row (low half). A claim is a compare-exchange that fails if the
  generation has moved on.
- A worker adds each finished chunk's rows to a done count, and the caller
  returns when it reaches the row count.
- A worker that wakes late finds no rows left, or a newer generation, and
  goes back to sleep. It never calls a finished call's row body (a
  `std::function` temporary that is gone once `run()` returns). The body is
  only called for a chunk claimed in its own call, and that call cannot
  return before every claimed chunk is done.

`LB_ROWBAR=0` restores the pending-count barrier.

**The priority.** The frontend runs the emulation thread at
`SDL_THREAD_PRIORITY_HIGH` (`THREAD_PRIORITY_HIGHEST` on Windows), and the
row workers ran at normal priority. That thread waits for them, so they now
run at the same priority, set on each worker by `lb_thread_priority_high`
(src/lb_prof.cpp). `LB_ROWPRIO=0` leaves them at normal priority.

Neither change can move a pixel. Every row is still computed by the same
body on one thread, the output does not depend on which thread or when, and
since docs/352 no per-thread row state outlives a call.

## 4. Verified

On the build with the rows barrier and the raised workers, and again on the
final build (render thread back at normal priority, s5):

- **The exactness gate:** identical at all six frames, both builds.
- **Layer maps and composites** inside the replay: 8 of 8 byte-identical at
  1473x830 (against a build before docs/354), both builds, and 8 of 8 at
  1280x720 (against the docs/354 build). `lb_thinholes.py`: 0.
- **Vanilla** (`--adaptive-widescreen none`), final build: 240 of 240
  presented and native frames identical to `exp-van-hudmask`.

## 5. Measured

`LB_POOLAB=bar|prio` switches one setting off and on inside one run, 300
composites of each in turn, and times the whole row phase of every
composite: the wake, the caller's own chunks, and the wait. It reports the
mean, the 95th percentile and the worst of each setting every 600
composites. Both settings see the same moment of the replay and the same
load, a pair at a time. This is the widest surface (642 rows on ten
threads). "Load" is `burn.py 6`, six processes spinning at normal priority,
started just before the run. 11-12 pairs a run, the harness's resize sweep
at the end left out.

**The barrier** (workers at normal priority in this build):

| | mean, all workers / all rows | p95 | worst per block |
|---|---|---|---|
| quiet | 2.51-2.70 / 2.50-2.58 ms | 2.8-3.2 / 2.8-2.9 | 2.9-3.7 / 2.9-3.2 |
| loaded | 3.28-3.64 / 3.30-3.44 ms | 3.8-4.7 / 3.9-4.1 | 4.5-10.0 / 4.5-9.0 |

Close to neutral. Under this load the workers are not starting late. The
whole thread slows instead: emulation went from 11.4 to 15.5 ms a frame at
HIGHEST priority, which is the burners sharing the physical cores (two
logical CPUs per core), not the scheduler. The rows barrier stays
because it is exact and cannot cost anything: in the rare pair where a
worker is late, the wait is gone.

**The priority** (rows barrier on):

| | mean, normal / high | p95 | worst per block |
|---|---|---|---|
| quiet | 2.49-2.72 / 2.49-2.55 ms | 2.8-3.3 / 2.8-2.9 | **2.9-7.9 / 2.9-3.4** |
| loaded | 3.33-3.65 / 3.27-3.35 ms | 3.8-4.7 / 3.8-4.0 | **4.8-24.8 / 4.2-5.4** |

At normal priority, 8 of 11 quiet blocks had a composite of 4.7-7.9 ms,
and loaded blocks reached 19.0 and 24.8 ms: the desktop's own threads
taking a worker's CPU mid-chunk. At high priority no quiet block exceeded
3.4 ms, no loaded block 5.4 ms, and the loaded mean is lower in 11 of 11
pairs. A second pair of runs repeated it (3 of 11 quiet spikes that time,
up to 7.7 ms; loaded up to 19.3 ms; high again at most 3.2 and 5.4 ms and
lower in 11 of 11 loaded pairs).

**The whole frame.** The same probe also times each frame, from one
composite's start to the next, and charges it to the setting of the
composite that began it. The frame's emulation ran after that composite,
so a starved render thread would show there. Frames over 20 ms, summed over
11 pairs:

| setting switched | quiet, off / on | loaded, off / on |
|---|---:|---:|
| workers' priority (normal / high) | 29 / 36 | 1,235 / 1,250 |
| render thread's priority (normal / high) | 38 / 60 | 1,526 / 1,644 |

Paired by block, the workers' priority changes nothing measurable in the
frame (t 0.6 quiet, 0.2 loaded). Raising the render thread as well makes
the loaded frames worse (paired t 2.5, p ~0.03), and quiet leans the same
way. So the workers stay at high priority, which is measured better for
the composite and no worse for the frame. The render thread stays at
normal: `LB_RENDERPRIO=1` raises it, and `LB_POOLAB=render` measures it.

**What the frame metric says about the widest window.** Even quiet, the
95th-percentile frame is 17.6-20.0 ms per block, and 0-14 frames in 300 go
past 20 ms. That tail is the frame's own work, not a late thread: emulation
~11.5-12 ms plus the present ~3.9 ms, against the DS's own period of
16.715 ms. The frontend's pacing aims at that period, adjusted by up to 2%
to hold the audio queue, and a late frame restarts the schedule
(docs/349). This change does not reach that tail. Section 6's two savings
(~1 ms) do, and so does the pipelining still awaiting a decision (docs/355
s4, ~2.7 ms).

## 6. Two savings found and not taken

Both are on the emulation thread, so on the frame's critical path at every
widened size.

**The native composite nobody sees (~0.64 ms a frame, s2).** Skipping it
looks free in a widened battle frame, but it is not safe as a simple skip,
for a reason in the frame's timing:

- The composite for frame N runs at the 263->0 wrap (frontend.cpp runs the
  scheduler to the frame boundary, then composites). By then the guest's
  VBlank handler has already written frame N+1's registers. So whether the
  widener takes frame N is decided by N+1's configuration. When it refuses,
  it shows frame N's native picture through `lb_menu_fill`.
- So whether frame N's native lines will be needed is unknowable while they
  are being drawn. Every exit from the battle configuration (the pause
  menu, the end of a battle) makes exactly that frame need them.
- Other readers: the debug server's `framebuffer` command (the harness's
  colour hint in `in_gameplay`; its real test is structural), the
  battlefield panel (`lb_field.cpp`), display capture (the guest can read
  it back), the black-band diagnostic and the frame dumps.

A skip therefore needs a fallback for the transition frame, a request path
for the other readers, and knowledge of how the game leaves a battle, such
as whether it has faded to black by then, before it can be called invisible.
`--pause-cycle` exercises one such exit. That is its own pass.

**The 3D snapshot copy (~0.38 ms at this width).** The same bytes could be
copied by the row workers in parallel at line 191, when every row of the
raster is finished. The rows must be complete first. `SoftRenderer::GetLine`
waits on one shared semaphore in a loop, one token posted per rendered row.
With two threads waiting at once, one can take the token the other needed.
After the last row nothing is posted until the next frame renders, so that
thread sleeps for a whole frame. docs/348 met the same hazard where the
composite reads the live raster, and solved it the same way: one wait on
one thread for the last row, then the workers fan out. The copy would need
the same. `GPU3D::GetLine` also returns one shared `ScrolledLine` when the
3D layer's X scroll is non-zero or the frame was aborted, so
that case must stay serial. About 0.25 ms to gain.

Both are next, the copy first: it is exact by construction, and the skip
needs the decomp pass on how a battle is left.

**Both done since** (2026-09-24): the copy is gone, and the snapshot keeps
pointers into a double-buffered raster (docs/359, ~0.36 ms). The native
picture is skipped and completed exactly at the fade that ends a battle
configuration (docs/361, ~0.65 ms).
