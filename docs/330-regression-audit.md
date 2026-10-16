# 330 — The last week's commits, against the reported regressions

On 2026-09-19 the deployed build was reported as much less stable than four
days earlier: laggier, more jittery, with regressions all around, borderline
unplayable.

This audits that claim against the commits, rather than accepting or dismissing
it. The headline is not what was expected at the start.

## The window

Four days earlier is 2026-09-15. The last commit that day is `d4ca9431` at 17:48;
there are no commits on the 16th or 17th. So the range under test is

    d4ca9431 (09-15 17:48)  ..  b5b302fd (09-19 23:16)

**87 commits. 28 of them touch `port/` at all**; the other 59 are docs, tools
and harness, which cannot change what a player sees.

## FINDING 1 — the deployed build was not the code being written

This is the one that matters, and it is mechanical rather than interpretive.

| binary | dated | has `LB_HOSTTERRAIN` (`5263b07b`) | has `WHOLE-MAP` (`1b47b09f`) |
|---|---|---|---|
| `build/LegoBattles.exe` (the deployed build) | 09-19 **21:44** | **yes** | **no** |
| `port/runner/build/nds_runner.exe` | 09-19 **22:52** | yes | yes |

`5263b07b` made `lb_field_locate()` succeed on the reported level for the first
time, which **switches the whole host-field renderer on** there — a render and a
shroud pass over the entire map, every frame. `1b47b09f` clips both to the
visible window and is the thing that makes that affordable. The deployed build
has the first and not the second, and had been in that state since 21:44 because
`LB_NO_DEPLOY=1` is set while the deployed build may be in use and nothing ever
cleared it.

**Measured, on the recorded route of the reported session, at a 1855x988
window, both timed in the SAME frame over 14,040 frames** (`LB_FIELDBENCH=1`,
`--demo demos/user-combat-2026-09-14.lbdemo --window-size 1855x988`):

    map 2304x1536   WHOLE-MAP mean 2.587 ms | CLIPPED mean 0.430 ms
                    worst 3.745 ms

**2.157 ms per frame** of pure waste in the deployed build. At the ~37 ms frame
that 26.9 fps implies, that is 5.8% — worth roughly +1.6 fps. Real, and not the
cause of 26.9 fps; see Finding 3.

This is the **third** recorded occurrence of the same failure. On 2026-09-12 the
deployed build was six hours behind the fixes. Earlier still, `build.sh`'s own
comment records a reviewer judging a binary 233 commits stale. The deploy step
is opt-out and the opt-out is sticky, which is the wrong default for a step
whose omission is invisible.

**Fixed now**: rebuilt, smoke-tested over two 300 s replays (0 dispatch misses,
0 overrides failing, clean exit), and deployed. `build/LegoBattles.exe` is dated
2026-09-20 08:37 and `WHOLE-MAP` is present in it.

## FINDING 2 — `HEAD` did not link from a clean checkout

`5263b07b` committed `apply_hooks.py`'s declaration and call of
`lb_open_session_log()` in `main.cpp` and left the only definition of it
uncommitted in the working tree. Every commit from `5263b07b` to `b5b302fd`
fails at link for anyone who clones. Local builds kept working because the file
was on disk.

Fixed in `d4de0b33`, which also carries the function itself — the port reopening
its own stderr on `build/logs/session-<stamp>.log`, because a player launches
the exe directly and a process started that way has nowhere for stderr to go.
That is why the 09-19 crash (docs/324) left one stray `dispatch_misses.log` and
nothing else.

## FINDING 3 — nothing in the window explains "laggier", and three things contradict it

Taking the 28 runtime commits in order and asking only "does this add per-frame
cost to a build that was deployed":

* **`9d99c731` (09-18 11:11)** removed a log flood — 74,860,509 bytes at ~180
  MB/min on a per-call path — and measured **58.5 fps against the route's
  previous best**, with the flood itself *"worth 4-10 fps by itself"*. Faster.
* **`7eaf4fe2`, `84330baa`, `d2534656` (09-19 01:03–01:31)** took the ARM7 from
  42.9% interpreted to **0%**. Faster, and its side effect was confirmed by
  ear in play: unit sounds are playing.
* **`53de0a57` (09-18 13:11)** replaced a per-vertex `lb_band_left()` call with
  a value latched once per frame. Fewer calls, not more.

The two commits that *do* add per-frame work — `5263b07b` and the fix that
tames it — **both landed after the reported play session**.
`build/dispatch_misses.log` from that session is dated 09-19 **11:54**;
`5263b07b` is 22:14.

So the build that was judged does not contain the regression previously
suspected, and everything measurable in the window moves the other way.

### What does explain it, then

Two candidates, both outside the commit log, and both already documented:

1. **The ITCM interpreter lottery** (docs/322). ARM9 ITCM falls back to the
   interpreter in roughly **1 run in 4**, and when it does the game drops to
   **17–19 fps** for that whole session. Two sessions four days apart are two
   draws, not a comparison. This is the best available explanation for the
   instability specifically — the complaint is about variance, and there is
   a measured source of exactly that variance.
2. **The window size** (docs/327). The 3D is rasterised in software at 928x494
   — 4.7x the pixels the DS ever drew — because
   `nds_gpu3d_use_compute_renderer` gates on `render_width > 448` and
   `render_width % 64 != 0`, and 928 fails both. Cost follows the surface. A
   window enlarged between sessions drops the frame rate with no code change at
   all, and `--window-size` did not work until 09-19, so no measurement before
   that date describes any size but 1024x384.

Neither is a commit, which is why a commit-by-commit audit could not have found
them and why this section exists.

## Regressions introduced in the window, with their fate

| # | introduced | what | found | fixed |
|---|---|---|---|---|
| 1 | `5263b07b` | host field switched on with no visible-rect clip: **+2.157 ms/frame** | same day | `1b47b09f`, deployed 09-20 |
| 2 | `5263b07b` | call site committed without its definition — `HEAD` does not link | this audit | `d4de0b33` |
| 3 | `53de0a57` | viewport and vertices placed by different bands during a resize: 64–80 px | in play: a sprite nudged as the camera moved | `5af03a58`, 9 h later |
| 4 | before the window | `sel_rect_override` passed cpu=1 for an ARM9 address, so it never called the guest and the selection rectangle was never built — **strictly worse than the inversion it was added to fix** | `9d99c731` | same |
| 5 | before the window | press-x written two bytes past a 0x10-byte allocation at 11 of 14 construction sites — **live in every build deployed so far** | exhaustive scan, `92aa4bca` | same |

Five, of which two were found only because something else was being looked at.
Four of the five were self-inflicted. That is the honest shape of the week.

## Improvements in the window, with the defect each answers

| commit | defect | evidence |
|---|---|---|
| `7eaf4fe2`+`84330baa`+`d2534656` | unit sounds | ARM7 42.9% → 0% interpreted; **confirmed by ear in play** |
| `3232a312`+`4e0e19d5` | vertical reach | **confirmed in play**: units can reach the top of the map now |
| `963c54e0` | clicks stop at the native edge | two patch tables collided and had disabled **all thirty** touch sites; the symptom was `movhi r5,#0xFB` pinning every out-of-range click to column 251 |
| `53de0a57` | sprites stretch on pan | mechanism matched exactly; **not visually verified** |
| `5af03a58` | resize nudges | usedx/renderx disagreement 64–80 px → 0 on 48 of 51 pairs |
| `92aa4bca` | memory safety | heap overwrite at 11 sites |
| `83fc2a79`, `0bbe860f` | drag inversion; phantom HUD button | |
| `9d99c731`, `b74c86cd` | music | repointed at match load |

Two fixes in the window confirmed in play, six harness-only.

## Two instruments that read as findings and are not

Recorded here rather than acted on, because both nearly entered this document
as regressions.

* **`[lb_hostterrain] sampler: 0 calls, 0 hits`**, in every log for two days.
  It counts `lb_hostterrain_pixel()`, the per-pixel API that `1b47b09f`'s
  predecessor **replaced** with `lb_hostterrain_row()` — 98,304 calls a frame
  through an `extern "C"` boundary, hoisted to one call per output row. The
  counter is correct and the function is dead. Delete the line or move the
  counter; as printed it says the subsystem never runs.
* **`band 0/672 kept, native 0/256 kept`** at 1855x988, against `109/384` in
  yesterday's 1280x720 logs. `lb_hostterrain_note` samples **one output row,
  `y = 96`, hard-coded**. At 988 rows tall that is near the top of the picture;
  at 720 it was not, and the runs are also on different levels (2304x1536
  against 1536x1024). It is not evidence that host terrain contributes nothing
  at that size. The row should be a fraction of the surface height, not
  a constant.

## What this changes about how the week is described

An earlier write-up of 2026-09-19 said that turning host terrain on is why the
reported session ran at 26.9 fps. **That is wrong and is retracted.** The
reported session predates the commit by ten hours, and on that level the host
field costs 2.587 ms of a ~37 ms frame. docs/327 said the same thing the same
day — *"saves ~1.0 ms of a ~37 ms frame — under 3%, and
not the cause"* — and the two were never reconciled. The cause of 26.9 fps is
the software 3D rasteriser, as docs/327 says.

What remains true, and is worse than what it replaced: **a 2.157 ms/frame
regression sat in the deployed build for the whole time its fix sat, built, in
the directory next to it.**
