# 349 — A quarter of every frame was a linear search

Written for: whoever picks up the frame budget next.

Status: **built, measured and deployed twice** -- `build/LegoBattles.exe`,
2026-09-23 09:31 (sha256 `11c74995...`: the lookup fix, sections 1-7) and
again at 12:45 (sha256 `e1440f5b...`: plus the steady present cadence of
section 9 and the MPU memo of 9b), the player's settings.json kept both
times.

The last build, checked before it shipped: the exactness gate (section 7)
identical at all five frames; the same 16 frames of the reference replay
pixel-identical to both earlier deployed builds; the reference replay at 59.8
fps with 0 audio underruns and a present-interval sd of 0.42 ms; the pan at
9.11 / 9.05 ms of emulation (9.11 / 9.17 before); vanilla at 59.8 fps.

The defect (docs/347 item 1): the game stuttered and ran below speed.

## The one-paragraph version

docs/262 and docs/282 put the frame's cost in "the emulator core" from 50
gdb samples and from NDS_PROFILE_SCHED's ARM9 / ARM7 / devices split. Neither
could say *which function* inside those shares was spending the time, so this
adds an instrument that can: an in-process sampling profiler. Its first
profile of a battle put **26% of the main thread in one port function**,
`lb_dispatch_original`, which the port's own overrides use to call the
function they replaced. It found that function by walking every row of every
dispatch bank -- 409,316 rows in the ARM9 title bank alone, one per guest
instruction, about 10 MB -- **on every call**, and three overrides that run
many times a frame (a VRAM-transfer observer, the heal tick and set-HP) called
it every time. Memoising the answer, which can only change when a bank is
registered, cut the emulation from 15.3 to 10-11.7 ms a frame at a
1473x830 window, brought every measured scenario to the 60 fps cap, and cut
audio underruns over the reference replay from 729 to 7.

## 1. The instrument: LB_SAMPLE

`port/runner/src/lb_prof.cpp`, off unless `LB_SAMPLE=1`. A second thread wakes
every 500 us (`LB_SAMPLE_US`), suspends the main thread -- which runs the
frontend loop, so all of emulation and the compositor -- reads its context,
walks up to 12 frames with the x64 unwind tables, and resumes it. Nothing is
allocated while the target is suspended, and the walk stops at the first frame
outside the image, so it never needs a lock another thread could hold.
Records are appended to `LB_SAMPLE_OUT` once a second, because the harness
kills the process instead of letting it exit.

`tools/lb_prof.py FILE --last 8` maps the samples through `nm` and prints:

* **GROUPS** -- each leaf folded into what it is: generated guest code, the
  runtime helpers it calls per instruction, the native 2D scanline renderer,
  the compositor, port code, and "outside the image" (driver and OS -- on a run
  that reaches the 60 fps cap this is mostly the audio-pacing idle wait);
* **SELF** and **INCLUSIVE** by function;
* `--callers F`: who calls F.

`--last 8` keeps the final 8 s, which covers the harness's 6 s fps window.
Addresses are rebased with the exe's on-disk ImageBase: the in-memory header
the runner can read has already been rewritten by the loader to the ASLR base
(the first attempt to rebase in the runner therefore did nothing).

`tools/lb_exp.py` also learned `LB_EXE=<path>` (forward slashes -- the
PowerShell tool's python is MSYS), to run another binary through the same
harness for an A/B.

## 2. What the first profile said

Harness pan, 1473x830 window, the build deployed on 2026-09-23 plus the
profiler (55.8 fps, emulation 15.3 ms/frame), 12,392 samples:

| leaf | share |
|---|---:|
| `lb_dispatch_original` | **26.1%** |
| `runtime_code_cycles` | 6.3% |
| `decode_text_line` (native BG scanlines) | 5.2% |
| the compositor's per-pixel lambda | 4.5% |
| `lookup_static_cached` | 3.3% |
| `runtime_tick` | 3.3% |

and by caller, `lb_dispatch_original` was reached from

| override (lb_patches.cpp) | share of its samples |
|---|---:|
| `gfx_transfer_observer` (func_0204E0E0, the generic VRAM transfer) | 53.6% |
| `heal_tick_override` | 22.6% |
| `set_hp_override` | 20.4% |
| the touch/selection/click/owner overrides | 3.4% |

## 3. The defect

apply_hooks 5b, verbatim before this change:

```cpp
extern "C" void (*lb_dispatch_original(int cpu, uint32_t addr))(void) {
    const CpuCtx& c = g_ctx[cpu & 1];
    void (*found)(void) = nullptr;
    for (const DispatchBank& bank : c.banks)
        for (unsigned i = 0; i < bank.len; ++i)
            if (bank.table[i].addr == addr && bank.table[i].fn)
                found = bank.table[i].fn;
    return found;
}
```

It is correct -- the original of an overridden address is the last bank that
carries it, because the title banks register after every override -- and it
was written when the banks were small. The lift now emits one dispatch row per
guest *instruction* (so that a function can be resumed at any label, docs/302),
which makes the ARM9 title table 409,316 rows. Every override tail-calls its
original through this function, and a transfer observer on the game's generic
VRAM copy runs many times a frame.

## 4. The fix

The answer depends only on which banks are registered: the tables are `const`
arrays, and the only writers of the bank list are `nds_register_dispatch`
(append) and `runtime_init` (clear). So the result is kept per (cpu, addr) in a
64-entry memo, keyed on a generation counter that both of those writers now
bump (`g_lb_dispatch_gen`). A lookup after any registration rescans; nothing
else can change the answer. The scan itself is unchanged.

## 5. Results

All runs: no dumps, `LB_DUMP_EVERY=100000`, the machine otherwise running
ordinary desktop applications plus another project's two emulator instances
(~13-18% total CPU).

| run | build | fps | emulation / frame | idle / frame | audio underruns |
|---|---|---:|---:|---:|---|
| harness pan, 1473x830 | before | 55.8 | 15.32 ms | 0.00 ms | 49 in the window |
| harness pan, 1473x830 | after | **59.7** | **9.95 ms** | 4.08 ms | **0** |
| reference replay, 1473x830 | before (deployed e7a9316e) | 56.4 | 15.28 ms | 0.06 ms | 729 over the run |
| reference replay, 1473x830 | after | **59.8** | **11.71 ms** | 2.48 ms | **7** over the run |

"Idle" is the frontend's audio-pacing wait (`drain_ticks`): time the loop had
to spare. 60 fps is the cap, so the saving shows as idle time, not as fps
above 60.

The profile of the replay after the fix no longer contains
`lb_dispatch_original` at all; its largest leaf is that idle wait.

### Stability, not just the mean

The live stats now count frames whose WORK (emulation + presentation, not the
pacing wait) ran past 16.7 / 20 / 25 / 33 / 50 ms; `fps.txt` prints the window's
counts. This build, the harness's own settings (pixel scale 1.75, so a larger
surface than the reference settings' 2.0):

| run | fps | emulation | compositor | idle | work > 16.7 / 20 / 25 / 33 ms | underruns |
|---|---:|---:|---:|---:|---|---:|
| reference replay, 1473x830 (842x474 surface) | 59.8 | 10.5 ms | 1.4 ms | 3.9 ms | 23 / 1 / 0 / 0 of 360 | 0 over the run |
| harness pan, 1473x830 | 59.7 | 9.1 ms | 1.7 ms | 5.1 ms | 2 / 1 / 0 / 0 of 360 | 0 |
| harness pan, 1920x1080 | 59.6 | 9.7 ms | 3.1 ms | 2.9 ms | 45 / 6 / 2 / 1 of 359 | 2 |
| harness pan, 2560x1440 | 59.9 | 10.3 ms | 4.5 ms | 0.6 ms | 215 / 16 / 1 / 0 of 361 | 0 |

Every configuration holds the rate. The margin shrinks with the surface: the
compositor scales with its pixel count, and at 2560x1440 with pixel scale 1.75
the average frame is within 0.6 ms of the period, so most frames run a little
long and the audio queue absorbs them. WHICH frames run long at the reference
size is not measured. One candidate is the game's own update rhythm --
NitroMain waits a second VBlank after an update that took under 20 ms
(docs/348 section 1), so the guest's work need not be spread evenly over
frames -- but nothing here shows that; a per-frame work trace would.

### The picture did not change -- checked, not argued

The reference replay, run on the deployed build and on this one, dumping
the same four presented frames (2001, 3501, 5001, 6501; `LB_DUMP_START=2000
LB_DUMP_EVERY=1500 LB_DUMP_SEQ=4`): the widened 842x474 composite, both native
screens and the top screen, **16 of 16 images pixel-identical**. Frame 6501 is
a battle -- terrain, units, a base, the cloud shroud -- so the whole enhanced
pipeline is covered, not a menu.

### Vanilla (adaptive off)

1920x1080, `--adaptive-widescreen none`, the harness route: 59.7 fps,
emulation **9.1 ms/frame** against 17.3 ms for the same run on the previous
build (docs/348's `exp-vanilla2-1920`), 7 ms of idle, no underruns. The frames
show the cartridge's own 256x192 screens -- HUD, minimap, shroud -- as before.
(A pixel comparison of THIS pair is not meaningful: the harness's camera
probe moves the view at a wall-clock moment, so the two runs' last frames
show different parts of the map. The replay comparison above is the exact
one.)

## 6. Where the rest of the frame goes now

The reference replay, after the fix, final 8 s:

| group | share of samples |
|---|---:|
| runtime helpers (timing model, dispatch) | 29.7% |
| generated guest code | 20.7% |
| outside the image (mostly the idle wait) | 18.8% |
| native 2D scanlines | 10.5% |
| other runner | 10.7% |
| compositor | 5.5% |
| port | 3.1% |

The largest single helper is `runtime_code_cycles` (8.5%), the per-instruction
code-fetch cost, followed by `lookup_static_cached` (4.1%), `runtime_tick`
(4.0%), `runtime_mem_cycles` (3.5%) and `cp15_data_cacheable` (1.5%). Those are
the next levers; any change to them must leave the emulation bit-identical,
which is what the gate below is for.

## 7. An exactness gate: LB_COUNTS_AT

`LB_COUNTS_AT=1500,3000,...` prints, at those presented frames, the guest's
retired ARM9 / ARM7 instruction counts and cycle totals. The frontend runs
exactly one emulated frame per present and a demo replays its input by frame
number, so two builds whose guest execution is identical print identical
lines. The instruction counts are the sensitive part: the cycle totals are
pinned to the frame boundary by the scheduler and move only by one
instruction's overshoot. (`event_counts.json` cannot do this: the harness
reads it at a wall-clock moment, a different frame every run.)

Checked before relying on it -- the same build, the reference replay run
twice:

| frame | insn9 (both runs) | insn7 (both runs) |
|---:|---:|---:|
| 1500 | 128,639,175 | 23,269,124 |
| 3000 | 464,454,752 | 40,617,236 |
| 4500 | 887,040,404 | 62,709,947 |
| 6000 | 1,336,655,868 | 82,860,006 |
| 7500 | 1,784,054,571 | 101,451,476 |
| 9000 | 2,226,912,705 / 2,226,276,481 | 119,211,912 / 119,211,514 |

Identical to the instruction through frame 7500. The two runs part at frame
9000 because the demo's last event is at frame 8188 and after it the harness
probes the camera -- it holds Right on the D-pad for 0.8 s to confirm it has
found the view object -- at a wall-clock moment. **Only frames inside the
demo are a valid gate.**

### The gate also needs the same settings.json (2026-09-23, afternoon)

The instruction counts depend on the port's settings as much as on the build:
the surface size feeds guest memory every vblank (apply_view_width/height,
apply_cull_width, apply_camera_view_size), and the gameplay options are
simulation-affecting. Mid-afternoon the harness's own
`port/runner/build/settings.json` was rewritten during a run -- the overlay
autosaves whenever any setting changes -- to pixel scale 1.0 (a 1472x768
surface), zeroed unit caps and two gameplay fixes switched on. The next gate
"failed" from frame 3000 on. It looked first like host load, then like a
pacing change; the old deployed build under the same settings gave the same
numbers, which is what exposed it. The writer was not identified (the save is
silent).

`lb_exp.py` now copies `tools/lb_harness_settings.json` -- the reference
deployed settings, pixel scale 2.0 -- over the harness copy before every run,
prints a WARNING naming any setting the port changed during the run, and
refuses to touch any settings.json outside the harness build folder. Under
those pinned settings, two runs of the current build and one of the deployed
build agree to the instruction:

| frame | insn9 | insn7 |
|---:|---:|---:|
| 1500 | 128,639,175 | 23,269,124 |
| 3000 | 467,336,666 | 40,632,466 |
| 4500 | 884,105,614 | 62,577,298 |
| 6000 | 1,322,302,763 | 83,387,395 |
| 7500 | 1,769,454,450 | 104,969,363 |

This supersedes the table above as the reference (that one was taken at the
old harness pixel scale, 1.75).

## 8. Item 3, tested per unit at last

docs/347 item 3: a drag started anywhere, but what it selected did not match
the box drawn. docs/348 fixed the two decomp causes -- the current
row still read from a byte the drag-crash clamp pins to 0..191 (020AF1BC,
020AF310 and four siblings), and the drag edge-scroll firing anywhere in the
widened area -- and deployed them untested per unit: no recording had a unit
standing in the widened ROWS, and docs/313's 14-of-14 proof was horizontal
only, on an older build.

`tools/lb_exp.py --vsel` asks it directly. It steers the camera with the
D-pad until the player's hero stands in a widened band, then drags boxes
around it and reads the game's own selected set (`[lb_sel]`, the handle list
at 0x02158028 -> the entity pool, docs/313) after each release. A drag is
judged only when the selection before it lets the outcome be attributed to
that box: a box that should select the hero must find it unselected first;
anything else is reported as inconclusive, not as a pass.

### The first result was the harness's, not the game's

The first run scored the crossing box as WRONG -- the hero, inside it, not
selected. The pen-edge log showed why: that drag never produced a pen-down at
all. The port drops touch while `ImGui::GetIO().WantCaptureMouse`
(`lb_overlay_wants_mouse`, the first line of `set_touch_from_mouse`), and
ImGui recomputes that once a frame from where it last saw the mouse. The
previous drag had been released at window (530, 507) -- inside the Top Screen
overlay panel -- and the synthetic press elsewhere arrived in the same frame,
so ImGui still thought the mouse was over its panel and took the drag. Other
drags lost only their first sample the same way (the log's DOWN / up / DOWN),
which moved the press corner 1/12 of the way along the drag and, with a unit
selected, turned the stray tap into a move order. A real mouse travels over
several frames and cannot do this; the probe now moves the cursor to the press
point with a button-up and waits 0.12 s before pressing. Every drag probe in
this harness is exposed to it.

### Results, 1473x830 window (842x474 surface), the deployed build

| run | hero parked at native | boxes judged | as expected |
|---|---|---:|---:|
| above the field, cross first | (147, -97) | 4 | 4 |
| above the field, reversed cross first | (147, -97) | 5 | 5 |
| below the field, cross first | (147, 239) | 3 | 3 |
| below the field, reversed cross first | (147, 239) | 3 | 3 |
| left widened columns (`--vsel-axis x`), reversed cross first | (-61, 95) | 3 | 3 |
| right widened columns, hero walked 110 px east first (`--vsel-walk 110`) | (326, 79) | 3 | 3 |
| above the field again, with the game's own rectangle logged | (147, -81) | 3 | 3 |

Judged boxes: a box entirely in the widened band around the hero; a box from
the band across into the native field, and the same dragged the other way;
and two that miss it (beside it in the band, and one confined to the native
field on its line). **24 of 24 as expected.** The game selects what the box on
screen contains, in all four widened bands, in both drag directions.

The right band needed one more step: the player's base sits near the map's
west edge, so the camera runs out of map before the hero passes native x 286.
`--vsel-walk 110` selects the hero, orders it 110 px east with a tap, and
clears the selection with the game's own deselect icon (anchored to the
window's bottom-left corner) before parking. In that run the box confined to
the native columns picked up the player's BASE standing inside it and not the
hero -- right on both counts.

**The game's own rectangle, logged.** `LB_SELRECTLOG=1` prints the inputs of
the rectangle builder (FUN_020AA8E4: press x/y in r1/r2, current x in r3,
current y a signed halfword at [sp]) whenever they change. For the crossing
box drawn at native (123,-109)-(171,60) the game built its selection
rectangle from **press (122,-110) to current (170,59)** -- rows well above
native row 0, which is exactly what docs/348's 16-bit row reads exist to
allow, and the drawn box to within a pixel of rounding in the harness's
window mapping.

### And the vertical band draws the geometry out there

docs/348 moved the vertical band into clip space so that units above and
below the native rows are not culled, and could not show it: no recording
put world geometry in those rows. These runs do. `screenshots/exp-vgeomA` and
`exp-vgeomB` (the same probe with the dump ring on; the kept frames are from
after the harness's camera probe, which pans right 256 px) show the hero and
builder -- selected, outlined, health bars drawn -- standing entirely in the
widened rows ABOVE native row 0, and the hero at surface row ~240, BELOW
native row 191 on a surface whose band is pinned to its top by the map's north
edge. Nothing is cut at the old 0..191 boundary; the base is cut only by the
surface's own top edge, where its position puts it.

Two things seen on the way, neither judged. A drag over empty ground
sometimes deselects and sometimes acts as a move order for the selected unit
(both above and below the field); which, is the game's gesture rule and was
not traced. And the builder, standing 4 px outside one box edge, was picked up
by one drag direction and not the other -- a sprite-extent tie at the box's
rounding edge, not a band defect (the hero, well inside, was selected every
time).

## 9. Sixty presents a second, delivered in pairs (item 4)

docs/347 item 4: the camera felt unsteady, particularly in motion, as if it
were not moving or were being pulled back while it moved. docs/348 fixed the layer pairing (78.6% of moving frames at exactly 0 px
drift). What it could not see is WHEN each frame reaches the screen.

The frame loop emulates a frame, presents it, then `drain_audio` sleeps until
the audio queue falls to its floor (2048 frames). The queue only falls when
the SDL audio callback takes a block, and the device is opened with 1024-frame
blocks at 32,728 Hz -- one every **31.3 ms**, while a DS frame is 16.7 ms and
makes ~547 frames of audio. So the loop is released once per callback and runs
two frames back to back.

Measured without a new build, from the LB_SAMPLE profiles already taken, by
timing the ends of the idle periods (runs of samples in the Sleep inside the
limiter). The largest group of intervals is 28-32 ms, one callback period:
111 of 254 on the harness pan and 50 of 139 on the reference replay. Only 7 and
1 fall at 16-18 ms, one DS frame. The rest are either short (79 on the pan
below 20 ms, brief idle blips) or longer (clusters at 38-42, 58-62 and
88-92 ms; not interpreted here). Two frames per idle period means presents in
pairs
about one frame's work apart (~10 ms), then a ~21 ms gap. On a 60 Hz display
the second of a pair can land in the same refresh as the first: a dropped
frame the frame counter never sees.

### The change: LB_PACE=timer

The loop now waits for a steady deadline at the DS's own rate (33,513,982 /
560,190 = 59.8261 Hz) BEFORE the composite, so each present follows a fixed
period plus the (nearly constant) composite time. The audio queue still sets
the long-run rate: the period is lengthened or shortened by up to 2% in
proportion to how far the queue sits from its target, one callback block
(1024 frames) above the floor (dynamic rate control), and drain_audio's own
wait stays only as a 125 ms safety bound. When the queue is below its floor
the loop presents at once and lets the emulator run ahead, as the old loop did
after a slow stretch; otherwise a frame that runs late is presented at once
and the deadline restarts from it. Turbo and the boot prebuffer are
untouched, and `LB_PACE=audio` restores the old loop exactly.

A refinement was tried and withdrawn: keeping the schedule through overruns
shorter than a period, so the light frame after a heavy one lands back on
time. Exact by the gate, but no clear gain -- present sd 1.94 vs 2.46 ms on a
post-unpause scene, 3.42 / 0.90 vs 0.66 on the reference replay, all with
another project's load on the machine -- and under sustained overload it
catches up in bursts of back-to-back presents. The deployed rule (a late
frame restarts the schedule from now) stays.

### Measured: the reference replay, same binary, both loops

| loop | present interval sd | intervals > 20 ms | > 25 ms | underruns, whole run |
|---|---:|---:|---:|---:|
| `LB_PACE=audio` (the loop until now) | **6.29 ms** | 84 of 360 | 34 | 2 |
| `LB_PACE=timer`, first cut | 0.39 ms | 0 | 0 | **334** |
| `LB_PACE=timer`, target half a block above the floor, catch-up below floor - 1024 | 0.79 ms | 2 | 2 | 59 |
| **`LB_PACE=timer`, as shipped** (target one block above, catch-up below the floor) | **0.69 ms** | 3 | 0 | **0** |

The first row is the judder, measured directly: a sixth of all intervals over
20 ms at an average of 16.7. The first cut of the fix removed it and broke the
audio -- waiting even when the queue was short, it refilled only through the
0.5% rate control -- which is why the underrun column is in this table and the
fix is graded on it. As shipped: 9x steadier presents and no gaps in the
sound at all over the four-minute reference replay. The exactness gate
(section 7) passes on this binary -- the loop changes WHEN frames are shown,
never what is emulated.

And the layers still line up: docs/348's pixel gate (tools/lb_layershift.py,
a 512x384 pan recorded frame by frame) on the deployed build gives exactly 0 px
of sprite-vs-ground drift on 78.6% of moving pairs, 92.9% within 1 px, max
2 px -- the same figures docs/348 measured before the pacing change.

The live stats now also carry the present-to-present intervals (count, sum,
sum of squares, gaps over 20/25 ms), and `fps.txt` prints the window's mean
and standard deviation, so this is graded by the spread of the intervals and
not by the frame count.

## 9b. Two cheaper-frame attempts that did not pay (recorded, not shipped)

**Inlining the per-instruction code-fetch cost** (apply_hooks 0z2,
withdrawn). `runtime_code_cycles` was the largest runtime helper in the late
battle (8.5%), and at every call site the guest pc is a compile-time
constant, so an inline form over two C mirrors of the latched code timing --
with `always_inline`, because GCC otherwise stops inlining into the huge
generated functions, as the profile showed for 0z's own `lb_should_yield` --
looked like a clean win. It passed the exactness gate (identical instruction
counts at all five frames). It did not pay: the binary grew from 154.8 MB to
256.4 MB, a full build took ~110 minutes instead of ~40 with cc1 at 2-3 GB per
shard, and emulation on the pan measured 9.107 / 9.165 ms per frame on the
previous build against 11.130 / 9.312 ms with it. The same verdict docs/204
recorded for runtime_tick and the trace guard: at every guest instruction,
the instruction-cache cost of the expanded code outweighs the calls it
removes. The record sits in apply_hooks 0z2 and in runtime_arm.h beside the
earlier two.

**Handing the composite's rows out in chunks** (`LB_ROWCHUNK`, off by
default). The compositor splits its rows into one fixed band per thread, and
a frame waits for the most expensive band. Chunks of 16 rows from a shared
counter measured 4.474 ms against 4.416 ms for the fixed bands at 2560x1440:
band imbalance is not what bounds it. Left in as a switch.

**Kept: the MPU cacheability memo** (apply_hooks 0z3). `cp15_data_cacheable`
and `cp15_code_cacheable` scanned all eight protection regions on every call;
they are now memoised per 4 KiB page, keyed on the CP15 generation every
relevant write bumps, and a page is cached only when no region boundary falls
inside it. Runner-side, no code growth, and exact by the same gate.

## 9c. The brick HUD hides while the camera moves -- the cartridge's own rule

A 300-frame recording of the reference replay's battle (composite frames 2785-3084,
1473x830, every frame with its 3D layer) found the in-field HUD -- the brick
buttons and the deselect icon -- in only 23 frames, although the game's own
selection list (`[lb_sel]`) held a unit continuously from frame 2858 to 2953.
The port's HUD scan explained where it went: in those frames the 3D list held
no HUD quads at all (one scan: 7 polygons, all at world depth). The game had
not submitted them.

Lined up against the camera (`[lb_pairpick]`, per frame), the rule is exact:
the HUD is absent whenever the camera is stepping, appears ~3 frames after it
settles, and needs ~5 frames to appear after a selection. The oracle agrees.
`lb_exp.py --hudpan` selects the hero, holds still, pans with the D-pad and
stops, sampling the native field screen and the camera; with
`--adaptive-widescreen none`:

    still        cam (136,144)   HUD icons 178 px   (2.5 s, every sample)
    D-pad pan    cam 137 -> 583  HUD 0 from the second moving sample
    settled      cam (632,144)   HUD 0, then 178 px from ~3 frames later

(a few 3-67 px readings during the pan are terrain passing through the
sampled corner). **Not a port defect:** the cartridge hides its in-field HUD
while scrolling, and the port reproduces it frame for frame.

Probably, but not demonstrated, this is the intermittent brick HUD of
docs/324 item 15: before docs/348 a drag out in the widened area fired the
game's edge-scroll anywhere in the band, so the camera kept moving under the
pen and the HUD stayed hidden -- missing on one selection, present on the
next.

The same recording answers the other half of docs/324 item 7 as far as one
battle can: 300 consecutive frames, the 3D layer's coverage between 2,653 and
4,182 px per frame, and **no frame below 70% of both its neighbours** -- no
building flicker or truncation in the 3D layer there. ~~The report was on the
Eagle Command base (Earth), which this recording's map does not have, so that
report stays open.~~ **RETRACTED 2026-09-23 (docs/350):** the map does have
one -- it is the player's OWN base. The player's builder is blueprint 82 (Earth), the
Earth faction's base is `E_EagleCommandBase`, and the largest object in those
300 frames, 53 px wide at surface x 373..425, is that base standing in the
right band. docs/350 follows it through 1,700 more frames at the reference
842x474 surface.

## 9d. Unpausing, measured at last

docs/324 item 12, lag after unpausing: docs/311 measured the fog's recovery after
an unpause and closed it, which answered a different question. `--pause-cycle`
now samples the live stats across the unpause; `--pause-nocap` skips its
capture burst, which otherwise stalls the window inside the measured span
(with it: 198 frames over 16.7 ms and 26 presents over 20 ms -- the harness's
own). Without it, on the deployed build, the 3.4 s after unpausing: 59.7 fps,
emulation 8.8 ms/frame, 0 underruns, 2 frames over 20 ms of work, 3 present
gaps over 20 ms and none over 25. No lag. The report predates the
lookup fix of section 4, which is the most likely cause of the lag seen.

Wide frames and crystal fields: at 2560x1080 the battle
surface (1536x648, clamped to the map's width) has no black rows or columns
at any edge; crystal fields in the widened columns of the reference map
render whole.

## 10. Where the 2026-09-22 list stands

| # | defect | state | evidence |
|---|---|---|---|
| 1 | game stutters, runs below speed | **fixed, deployed 09:31 and 12:45** | 26% of the main thread was a port lookup (sections 2-5): reference replay 56.4 -> 59.8 fps, audio underruns 729 -> 7 |
| 2 | minimap box shows while the minimap is hidden | fixed, deployed 2026-09-22 18:06 (`04d81975`) | docs/347/348 |
| 3 | drag selection does not match the box drawn | **verified** on the deployed build | decomp causes fixed in docs/348; 24 of 24 judged drags per unit in all four widened bands, and the game's own rectangle logged (section 8) |
| 4 | camera unsteady, particularly in motion | layer pairing fixed (docs/348); **presents were delivered in pairs** -- fixed, deployed 12:45 | present-interval sd 6.29 -> 0.69 ms on the reference replay, 0 underruns (section 9) |
| 5 | command sprites under the fog | fixed, deployed 08:25 (docs/348) | UI punches from the cartridge's own cell grid |
