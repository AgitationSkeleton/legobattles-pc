# 353 — Where the frame goes at the widest windows

Written for: whoever takes on 60 fps at ultrawide sizes next.

Status: **measured, nothing changed in the picture or the game;** deployed
2026-09-23 22:11 (`2cf65af5`, sha256 `e02735cd...`, settings kept). New probes
(`LB_3DWAIT`, `LB_ROWTIME`), a profiler that can name the callers of a
library leaf, and two tool fixes. Section 6 records a decision still to be
made, with its A/B.

docs/352's window sweep found one size that does not hold 60 fps:
3440x1440 (surface 1536x642, the map's full width), 58.9 fps with 44 audio
underruns, where the frame's work is the whole 16.7 ms budget. 2560x1080
holds 60 with 0.1 ms to spare. This doc measures where that time goes. It
runs on the reference replay, under an ordinary desktop load (other
applications taking several cores). That load is the real-world condition,
and it is why the numbers below move between runs.

## 1. The main thread, sampled

`LB_SAMPLE=1` at 3440x1440, the last 8 s of the run (a battle, 12,575
samples, ~54 fps with the profiler attached):

| group | share |
|---|---:|
| runtime helpers (cycle accounting, dispatch lookups) | 26.2% |
| generated guest code | 20.7% |
| outside the image (see below) | 13.0% |
| gpu2d: the native 256x192 scanlines, both engines | 12.4% |
| the widened compositor | 11.7% |
| other runner (bus, timers, audio) | 9.9% |
| port code (lb_*) | 5.1% |

About half of the thread is emulation (47%): the lifted code and the
helpers it calls per block. The largest helpers are `runtime_code_cycles`
6.5%, `lookup_static_cached` 4.3%, `runtime_tick` 3.9%, `runtime_mem_cycles`
3.5%, `lb_should_yield` 2.1% and `runtime_dispatch` 1.9%.

**The unnamed leaves.** The profiler stops at the first frame outside the exe
(unwinding into a DLL while the thread is suspended could deadlock), so four
addresses came out as bare numbers. System DLLs load at the same base in
every process until reboot, so the profiling tool's own loaded modules
resolve them:

| address | is | share | caller (stack scan, below) |
|---|---|---:|---|
| ntdll+0x9C8F4 | `ZwWaitForMultipleObjects` | 6.1% | not found within 768 bytes |
| ntdll+0x9BE24 | `ZwWaitForSingleObject` | 4.3% | not found within 768 bytes |
| ntdll+0x9C424 | `ZwDelayExecution` | -- | the frame-pacing sleep |
| ucrtbase+0x482CE | the CRT's copy/fill (see note) | 4.7% | `render_engine_line` 46%, `nds_io_read` 14%, `lb_overlay_set_top_pixels` 13%, ... |

The nearest ucrtbase *export* to that address is `memcmp+0xEE`, and that is
what I first reported. It is wrong: the callers are the native scanline
renderer's line clears and copies, the top-screen blit, and other
copy/fill users, so the address is an unexported copy/fill routine placed
after `memcmp`. The dispatcher's byte validation, the first suspect for a
memcmp, compares 124 bytes on average (10,342 ranges, 514 at most) and only
on the ~83 slow lookups a frame (`cache_slow_lookup` 1.04M over the run
against 175.6M cache hits), so it is not a cost.

`lb_prof.cpp` now finds a library leaf's caller by scanning the suspended
thread's own stack for the first return address into the exe that follows a
CALL instruction, and resumes the proper unwind there. That is plain memory
reads, and no function table is consulted. The two kernel waits are called
through deeper library frames and are still unnamed. Section 3 names them
another way.

`tools/lb_prof.py --skip PREFIX` steps over frames when naming a caller.
Every lifted guest call goes through `runtime_dispatch` and then returns to
the dispatcher loop, so the host stack never shows a guest caller: the
direct caller of every hot `lb_arm9_*` function comes out as the dispatcher
or `scheduler_run_round`. Guest callers need a guest-level observer (an
override that logs LR).

## 2. It is not the 3D rasteriser

The software rasteriser runs on one thread at any width, and at 1536x642 it
has 2.5 times the pixels of the 842x474 surface where docs/347 measured the
emulation thread waiting 0.009 ms/frame on it. The main thread meets it at
exactly two places: `VCount144`, which waits for the frame begun at the
previous VCount 215, and `GetLine`, which waits for a row the compositor
asks for. `LB_3DWAIT=1` times both, and the render thread's own work,
every 300 frames:

    [lb_3dwait] 300 frames at 1536x642: render thread 1.93 ms per render
      (193 renders, max 4.14); emu thread waited at VCount144 0.000 ms/frame
      (0 frames >= 0.05 ms), in GetLine 0.486 ms/frame (77 waits)

Over the whole replay the render thread takes 1.6-2.3 ms per frame (max
~6), the guest submits about 150 frames in every 300 (its own 30 Hz), the
emulation thread NEVER waits at VCount144, and the compositor's GetLine waits
are 0 to 0.5 ms a frame. docs/347's conclusion holds at the widest surface.

## 3. It is the compositor's row bands

`LB_ROWTIME=1` times `LbRowPool` (the compositor's row workers, fixed bands
on hw-2 = 10 threads). Each worker records how late it started after the
wake and how long its band took; the caller records its own band and how
long it then waited:

    [lb_rowtime] 300 composites, 642 rows on 10 threads: caller's band
      3.64 ms, then waited 2.73 ms; worker band mean 3.69 ms; last worker
      done 6.38 ms after the wake (latest start 3.07 ms)

Across the replay: every 64-row band takes 3.5-3.8 ms on its thread, about
**36 ns a pixel** (three extended layers decoded and composited across 1536
columns, 99k pixels a band). And under that desktop load the last worker
**starts 0.8-3.2 ms after the wake**, so the caller, done with its own band,
then waits 1.0-2.8 ms. The rows cost 4.6-6.4 ms a composite, of which the
caller's wait is the waits in section 1.

That is oversubscription: the runner's threads (emulation, rasteriser, one
row worker per remaining logical CPU) fill the machine, with the desktop
taking several of the same CPUs. A late worker holds up the frame by its whole band.

## 4. The hottest guest code

`FUN_0207E9B4` (4.5% inclusive) with `FUN_0207EB78` (3.9%) is a tile-rect
entity gather. It walks x from rect+8 to rect+0x10 and y from rect+9 to
rect+0x11 (bytes). Per tile it looks up the entities on that tile through
three slots of a mask, de-duplicates them with a flag at entity+0x15A, and
appends them to a list. Then it clears the flags. Its cost is proportional
to the rect's area. Its callers include the render gather `020882BC`
(the projectile trace below). Which caller dominates at runtime is not
established: lifted calls tail-dispatch (section 1), so it needs an observer
on 0x0207E9B4 that logs LR and the rect. If it is the render gather, whose
rect the port widens to the surface (apply_cull_width), then this cost
grows with the window by design. Inclusive shares understate it, because its
callees run as separate host frames.

## 5. Decomp results from the same pass (read-only agent, verified)

- **Unit projectiles are drawn in the bands already.** Arrows, bolts, shots
  and cannonballs in flight are `Battles::Sim::Projectile` entities (vtable
  `0x021496F4`, constructor `0205D100`, handles in the vector at
  `0x02155048`). Render task 0x1D (`02039148`) gathers them through
  `020882BC`: the grid through `0207E9B4`, then that vector through
  `0205D3D4`, which keeps an entry only if its cell is inside the same
  rect. That is the gather rect `apply_cull_width` widens. Their draw
  `020EBF34` (a 16x16 quad through `0202DA80`) has no screen-bound compare.
  The effect helpers `02016070`/`02016110` that docs/350 widened serve
  Effects and particle systems, never unit projectiles, so the
  "effect/projectile" wording in the port's log line was wrong. It now says
  effect/particle.
- **docs/350 s6 corrected:** the field draw's "three lists" are one gathered
  list walked five times. `0203AB00` hands kind-1 entities to the projectile
  draw `020EBF34`, and `020EBB70` draws collectables.
- **One unwidened screen test, not measured:** `020A8DD0` tests a point
  against 0..256 x 0..192 (20.12 fixed point) when its fourth argument is 0.
  Its only such caller is `020DAB50` in `020DAAE4` (WorldViewScreen, called
  from mission-API code at `02048A78`). A failing point's indicator is
  clamped to [4,236] x [4,172]. In a widened campaign mission, a point of
  interest visible in a band would therefore probably be marked as
  off-screen at the native edge. That consequence is inferred, not seen.
  Nothing in the port patches it.
  **Read further (2026-09-24):** `020DAAE4` is a four-second mission marker
  (its expiry is `OS_GetMilliseconds() + 4000`). When the point is outside
  the native view it keeps the target's world position and clamps the marker
  to the native edges. It then snaps the position to 8 px and passes it on
  as 8-bit values (`& 0xFF`), so this marker cannot address the widened area
  at all: widening the `020A8DD0` test alone would make band coordinates
  wrap. A fix means placing the marker host-side, as `lb_hud` already places
  relocated HUD elements, and it needs a campaign test route to see it
  first. The reference replay is a skirmish and never calls this.
- **`tools/lb_find_screen_rects.py` could never report a `cmp`:** it masked
  with 0x0FE00000, which clears the S bit every `cmp` has. Fixed. It now
  finds docs/350's effect and particle compares, which it should always
  have found. Two further pairs it reports were read and are not culls:
  - `02024C58` is `FUN_02024BD8`'s loop bounds, filling OAM with a 4x3 grid
    of 64x64 sprites over the 256x192 screen.
  - `020AB78C`/`020AB7B8` is the drag-pan camera's map-edge clamp in
    `Input_DragPan_020AB5F0`: camx + 256 against the map width in pixels,
    camy + 192 against its height. The port meets map edges by sliding the
    native window within the surface.

## 6. The levers, ranked by size

1. **The compositor's per-pixel cost**, ~36 ns a pixel. The loop is fully
   general: per pixel it pushes up to three BG layers and the 3D layer
   through a priority sort, then `compose()`, brightness and `to_rgb32`, on
   top of a per-tile decode with an out-of-line call per 8-px tile. A path
   specialised to the field's configuration, or a decoded-tile-row cache,
   is the largest single win available. It is a change to the core
   compositor and needs its own pass, graded pixel-identical by layer dump
   and composite.
2. **Late workers under load.** The existing `LB_ROWCHUNK=K` hands rows out K
   at a time, so on-time threads absorb a latecomer's rows. docs/349 found
   no gain on a quieter machine. It is output-safe now: the one per-thread
   row state (docs/352) is reset on every call. A/B below.
3. **The native engine-A scanlines in widened frames.** The emulation
   thread still composes engine A's 256x192 picture every scanline
   (`render_engine_line` and its copies and fills, a few % of the thread),
   although the widened composite replaces it on screen. The readers of
   `nds_gpu2d_framebuffer(1)` in battle are:
   - the frame loop, where the adaptive surface replaces it;
   - `lb_menu_fill`/`lb_edge_fill`, only on scenes the widener refuses;
   - the debug server's non-adaptive `framebuffer` command, which the
     harness's screen shots use;
   - the battlefield panel (`lb_field_learn_sprites` and
     `lb_field_composite_guest`, lb_field.cpp);
   - display capture, which the guest can read back.

   So a skip would be conditional: a widened battle frame, capture off, the
   panel closed, no native shot pending.
4. **Emulation helpers.** For example, `lookup_static_cached` is 4.3% of the
   thread over ~16k dispatches a frame (178M over the run), about 50 ns a
   dispatch for what is usually a cache hit. Recompiler-side work.

**The LB_ROWCHUNK A/B** (3440x1440, alternating runs, the machine as it
was; "load" is the CPU load sampled just before each run):

| run | load | fps | adaptive ms | emu ms | frames > 16.7 ms | underruns |
|---|---:|---:|---:|---:|---:|---:|
| fixed 1 | 31% | 51.9 | 4.36 | 13.47 | 236 | 943 |
| chunk 1 | 9% | 56.1 | 3.77 | 12.74 | 215 | 512 |
| fixed 2 | 21% | 54.8 | 4.13 | 12.82 | 223 | 479 |
| chunk 2 | 9% | 48.9 | 4.56 | 14.29 | 242 | 559 |
| fixed 3 | 25% | 44.1 | 6.05 | 15.05 | 259 | 1929 |
| chunk 3 | 26% | 49.5 | 4.58 | 14.09 | 238 | 1100 |

Means: adaptive 4.85 ms fixed against 4.30 chunked, 50.3 fps against 51.5.
But one of the three pairs goes the other way. And emulation time, which
chunking cannot touch, moves 12.7 to 15.0 ms between runs, so the machine's
load swings the frame by several times the effect. **Inconclusive; the default
stays as it is.** Settling it needs a quieter machine or many more pairs.

**What this means for 60 fps.** Under the load this machine carried
tonight, 3440x1440 ran 44-56 fps (58.9 in docs/352's sweep, earlier in the
evening). The frame is emulation 12.7-15 ms plus presentation 5-6 ms. 60
fps at the widest window needs about 2-4 ms taken out, and levers 1 and 3
are the only ones measured here that are that large.
