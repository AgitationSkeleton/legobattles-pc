# 355 — The row workers take chunks

Written for whoever works on the compositor or its threading next.

Status: **built, verified, deployed** 2026-09-24 00:34 (`c819acaf`,
`build/LegoBattles.exe` sha256 `f024994b...`, settings kept). Section 4 is an
open design decision, not something done.

docs/353 found the widened composite's caller waiting 1-2.8 ms a frame for
row workers that the OS started up to 3 ms late under desktop load, and docs/354 made each row cheaper. This makes the waiting cheaper: the
rows are now handed out in 8-row chunks by default, so threads that did start
take the rows of those that did not.

## 1. Why this was left off before, and why that was wrong

docs/349 tried `LB_ROWCHUNK=16` at 2560x1440 and recorded "MEASURED, AND LEFT
OFF": adaptive_ticks 4.416 ms with fixed bands against 4.474 ms chunked. That
was two separate runs. On the test machine the load moves emulation time 12.7
to 15 ms between runs (docs/353 s6), several times any effect a partition could
have, and docs/353's own three-and-three A/B was inconclusive for the same
reason. The reading is retracted in place in lb_wide2d.inc.

## 2. Measuring it so that the load cannot decide

`LB_ROWAB=K` makes the pool alternate, inside one run, between fixed bands
and K-row chunks, 300 composites of each in turn. It times the whole row
phase of each composite and reports both every 600 composites. Both modes
see the same load, a pair at a time. On the reference replay:

| window | K | fixed bands | chunks | chunks faster |
|---|---:|---:|---:|---:|
| 3440x1440 | 8 | 3.30 ms | 2.95 ms | 13 of 13 pairs |
| 3440x1440 | 16 | 3.13 ms | 2.95 ms | 13 of 13 |
| 3440x1440 | 32 | 3.19 ms | 3.06 ms | 10 of 13 |
| 1920x1080 | 16 | 2.12 ms | 1.90 ms | 11 of 13 |

(Each row is a separate run, so the columns compare within a row, not down the
table.) Smaller chunks win more at the widest window, where a late thread
holds up the most pixels. **8-row chunks are the default now**, a 10-11%
shorter row phase. `LB_ROWCHUNK=0` restores the fixed bands, and `LB_ROWCHUNK=K`
sets another size.

Output cannot depend on the partition: every row is the same body on one
thread. The one per-thread row state that once outlived a call, the HUD
cover mask, is reset on every call since docs/352. Without that fix,
chunking would have moved the stale mask to random rows.

## 3. Verified

With 8-row chunks as the default, on the reference replay:

- **Layer maps and composites** at 1280x720 (`LB_LAYERDUMP=1285`) against the
  build before docs/354: identical at every dump inside the replay
  (1285-5140) and at 7710. 6425 is past the recording's end. `lb_thinholes.py`: 0.
- **`LB_FASTROW=check` at 3440x1440**, now with rows moving between threads:
  4,000,000 rows compared, 0 pixels differ (fast 16.00 against general
  21.29 ns/px).
- **The exactness gate:** identical at all six frames.

## 4. Open decision: overlapping the composite with the next frame

With 354 and this, the widened composite at 3440x1440 is ~3 ms of wall time
that sits on the frame's critical path, after emulation (11-13 ms under
load) and before presenting. It can be taken off that path entirely by
compositing frame N on the row threads while the emulator runs frame N+1.
**That shows every frame one frame later than now: +16.7 ms of display
latency.** That trade is a design decision, not yet taken.

What the composite reads, and so what would need a copy per frame:

- **Cheap:** engine A's BG VRAM (bank E, 64 KB), the palettes, OAM and the
  2D registers -- well under a millisecond of copying -- and the 3D raster,
  which is already double-buffered (`g_wide_3d_frame`).
- **The work:** this port's own per-frame state -- the widemap terrain, fog
  and shroud tables built for this frame's camera (three of ~150 KB), the
  HUD boxes, the band and pairing values. The next frame's scan rewrites
  them mid-emulation, so each needs a second buffer. A missed one would show
  as exactly the kind of one-frame glitch this port has spent weeks removing.

It removes ~3 ms at the widest window and nothing at the narrow ones. The
emulation itself (11-13 ms, single-threaded) would then be the whole
critical path.
