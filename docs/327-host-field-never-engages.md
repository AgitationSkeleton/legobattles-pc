# 327 — The host field never engaged on the map actually being played

Status: **root cause found, not yet fixed.** This supersedes docs/325's
"STATUS: phases complete, deployed" — that status was wrong, and the way it was
reached is the first thing worth recording.

## What was claimed, and why it was wrong

docs/325 reported phases 1-6 complete and the build "ready to play". Every piece
of evidence behind that came from `tools/lb_exp.py --build-flow` runs, which
seed one save and play one level. On that level `lb_field_locate()` succeeds, so
the host field is live and everything built on it — phase 2's terrain, phase 3's
fog — was verified working.

**On the level of the reported play run, `lb_field_locate()` never succeeds.**
That run's log contains no `[lb_hostterrain]` line at all, across 4,840
frames. No `[lb_shroud]` line either. The entire host-field path was inert, and
the run was using the old band-synthesis renderer — the one with the patchy
shroud. A subsystem that silently does nothing is indistinguishable from one
that is switched off, and that is how it was read.

The lesson is the project's own, applied to a case it had not been applied to:
one route on one level is not coverage. The harness never plays that level.

## Reproducing from the recording

The play run left `build/lb-session-1789866133.lbdemo`. `LB_DEMO_PLAY=<path>`
replays it — the demo system exists precisely because scripted input could not
reach the game's own UI, and it is the right tool here too. Launch with the full
`play.bat` argument list; anything less exits with `unknown arg`.

    LB_DEMO_PLAY=...\lb-session-1789866133.lbdemo LB_DEMO_NOREC=1 \
      nds_runner.exe dummy 4000000000 --rom "..." --config game.toml \
      --boot direct --save-path "..." --interactive --generated-firmware \
      --network off --freebios

That reproduces the failure exactly, including the camera object address.

## The diagnosis, in four measurements

Both failure exits of `lb_field_locate` were **silent**. Instrumented:

1. `FAILED at find_primary (map_base 06000000 char_base 06000000 DISPCNT
   00010F08 BG1CNT 0001)` — a menu frame, BG1 legitimately unset. Expected.
2. In battle: `map_base 06000800 char_base 06004000 DISPCNT 00012F08
   BG1CNT 4187`. **BG1 is properly configured** and it still fails.
3. `4 candidate table(s), best score 0 -- NOTHING ON SCREEN MATCHES.
   window holds 119 distinct tiles`. So the view is rich terrain, not fog —
   killing the first hypothesis, that a shrouded view has nothing to match.
4. The decisive one:

        window tiles 0005..01F2
        cand 021505A6 extent 16 tiles 0000..005E
        cand 022E7A6E extent  3 tiles 0000..0011
        cand 022F6F98 extent  3 tiles 0000..0011
        cand 02301806 extent 49 tiles 0000..0090

**Every candidate has extent < 64**, which `find_primary` requires, so all four
are skipped and `best` never leaves -1. And even without that gate, none of
their tile ranges reaches the tiles actually on screen (`0x1F2`).

## What that means

`find_primary` locates the metatile table by scanning main RAM for **twelve
consecutive u16 equal to 0,1,2,…,11** — an identity run assumed to be the first
metatile. On this level, the four arrays that match that signature are false
positives: small tables belonging to something else.

The signature was derived from one level and does not generalise. It is a
correlation, not a fact out of the decomp — exactly the class of shortcut
docs/247 had to undo for the fog record, where four correlated candidates were
adopted and dropped before the real chain was read out of the code.

A palette-bit theory was also checked and is **dead**: the table entries carry
no palette or flip bits (`0047 0049 004B 004D 004F 0051`), so masking is not the
difference. The numbers themselves are different.

## FIXED — the chain, out of func_020A3478

`FUN_020A3478` is 106 bytes and is the metatile expansion. The whole of it:

```c
uVar2 = *param_4;                                   // the grid cell
if (uVar2 < *(u16 *)(DAT_020a34e4 + 2))             // the SPLIT
    puVar1 = *(int *)(param_1 + 0x338) + 2 + uVar2 * 0xc;
else
    puVar1 = *(int *)(param_1 + 0x33c) + 2 + (uVar2 - split) * 0xc;
// six u16 copied out: 3 across, 2 down
```

`param_1` is the **map object**: its caller `FUN_020A3234` uses
`*(u8 *)(param_1 + 0x14)` as the grid row stride and column bound, which is
`map+0x14` — the width this port already reads. So:

    map    = *(u32*)0x02156CB0
    w / h  = *(u8*)(map + 0x14) / *(u8*)(map + 0x15)
    row    = *(u32*)(map + 0x34 + y*4)      func_020A322C
    mt     = *(u16*)(row + x*2)
    split  = *(u16*)(0x0214B9D0 + 2)
    base   = mt < split ? *(u32*)(map+0x338) : *(u32*)(map+0x33C)
    src    = base + 2 + (mt - (mt < split ? 0 : split)) * 12

lb_widemap.cpp had this recipe written down and verified since docs/196-200.
**lb_field.cpp did not use it** — it searched instead, and departed from the
format in three ways that only one level forgave:

| lb_field did | the cartridge does |
|---|---|
| grid as flat `base + (y*w + x)*2` | a **row table** at `map+0x34` |
| entry at `base + idx*12` | `base + **2** + idx*12` |
| table picked by "is a tile blank in VRAM" | by the **split**, nothing else |

`lb_field_locate` now follows the chain and validates it (dimensions in range,
pointers in main RAM, split non-zero), falling back to the old search only if
that fails. Live on the replayed level:

    [lb_locate] chain: map 022BD750 64x64 rows 022BD784
                tables 022BDAE8/0228374C split 440

`[lb_hostterrain]` went from **0 occurrences to 24** on the same demo. The
harness level still works (`map 022BD974 ... split 440`), so this is not a
trade of one level for another.

## What it must not be

Find the table the way docs/247 found the fog record: **out of the decomp**.
The code that builds the BG1 tilemap reads this table through a pointer; that
pointer is the answer, and it will be correct on every level rather than on the
one the harness happens to play. Widening the signature, lowering the extent
threshold, or scoring candidates against the screen would all be more
correlation, and this file exists because correlation already failed once here.

## Grading per frame instead of per screenshot

Stills cannot show jitter: it is a property of the DIFFERENCE between
consecutive frames. `LB_DUMP_SEQ=N` now turns the 4-slot dump ring into a
recording of N consecutive frames, and `LB_DUMP_START=M` skips M presents
first — without that a sequence captures frames 0..N-1, which is boot and
menus. The first recording taken here did exactly that: 96 of 178 pairs were
static menu frames and the rest were screen transitions whose apparent shift
saturated any search window, which reads as violent jitter and is nothing of
the kind. The block's own comment had already warned that a one-shot prefix
only captures boot.

**The measure that works** is the seam, not a shift estimate. A cross-correlated
shift of the native strip against a band is unreliable because the two show
different content — measured, the best native shift scored 0.250 against the
best band shift at 0.253, which is noise, and reading those as a real
disagreement would have invented a bug. The seam is direct: compare the column
pair straddling the band boundary against typical adjacent-column differences
elsewhere in the same frame. 1.0 means the boundary is indistinguishable from
any other column.

Measured over 200 consecutive gameplay frames at a 1280x720 window:

| | mean | median | max | frames > 3x |
|---|---|---|---|---|
| left seam | 1.04 | 1.06 | 2.33 | **0** |
| right seam | 0.97 | 0.81 | 5.97 | **8** |

The left boundary is seamless. The right spikes in frames 111-117 — seven
consecutive — plus 144, so it is an event rather than noise, and that burst is
the next thing to chase.

## FIXED: the host field now tracks the guest camera exactly

`lb_field_camera_rect()` positions the host field. It read **lb_widemap's
cached tile camera**, which is both quantised to 8 px and only refreshed when
`lb_widemap_fill` runs to completion. It now reads the view object directly,
every frame:

    view = *(u32*)0x02152034        func_02039A5C stores it
    camx = *(s16*)(view + 0x5E)
    camy = *(s16*)(view + 0x60)

Traced per frame, guest camera against host rect:

| | before | after |
|---|---|---|
| moving frames whose STEPS disagree | 877 / 927 (94.6%) | **0 / 683 (0.0%)** |
| host − guest dx | 48, 64, 32, 52, 56 … (spread) | **48 on all 1385 frames** |
| host − guest dy | 0, 16, −16, 2 … | **0 on all 1385 frames** |
| left seam mean / frames > 3x | 1.04 / 0 | **1.03 / 0** |

Both properties hold at once: the position tracks the guest exactly, and the
alignment the seam measures is preserved.

### Two wrong turns on the way, both caught by measurement

**"g_ht_cx is the guest's camera by definition."** The row accessor maps surface
x to `g_ht_cx + (x - extra)`, so this looked airtight. Positioning from
`lb_field_camera()` made tracking perfect and made the picture WORSE — left seam
mean 1.04 → 1.42, max 2.42 → 7.32, 0 → 5 frames above 3x. That function
*scans* for a camera object and finds a different one: dy agrees exactly while
dx is a constant 48 out, which is the signature of another object, not a lag.
The note already in this function — "the picture and the box described
different coordinate systems" — was describing exactly that.

**Pixel deltas cannot measure this.** Terrain and fog are both 8 px tiled
patterns, so a shift by a whole tile is BYTE-IDENTICAL. The band region held 6
to 48 distinct colours with the top four covering 81-90% of it. A delta of 0.0
there means "moved by a tile" exactly as much as "did not move", and it was
read as the latter. Two fixes were built against that reading and produced
byte-identical recordings before the cause was checked.

`LB_CAMTRACE=1` prints the guest camera beside the host rect once per frame.
That is the instrument for this class of bug: it has no dependence on what the
pixels look like.

## Superseded: the antiphase reading

With the field engaging, per-frame deltas over the panning stretch of a
200-frame recording, split by region:

    native  16.4   4.1  11.3   2.7  17.9   4.7  16.4   5.1
    band     0.0  20.6   2.0  14.3   1.5  22.6   0.0  23.3

Three facts follow, and only the third is a guess:

1. **Both regions alternate**, native at ratio 1.76, band at 2.54. The native
   strip is the guest's own rendering, so the game itself is moving the camera
   on every other frame — consistent with a 30 Hz camera, which is ordinary for
   a DS title and is NOT a defect.
2. **They alternate in ANTIPHASE.** A band delta of exactly `0.0` means those
   pixels were byte-identical while native changed by 16.4. Half the picture is
   always one frame out of step with the other half. That is the reported shake.
3. The likely cause is *when* the port samples the camera: the host field is
   positioned at composite time, and if the guest has by then written the camera
   for the NEXT frame, the bands lead the native columns by one. Not yet
   established.

**Two fixes were attempted against the wrong path and produced byte-identical
recordings** — the DISPCNT bail (`lb_widemap_fill` bail 2) is not firing here;
`[lb_wide] ext_valid=1` says the fill completes every frame. Identical output
after a real code change is a question, not a null result, and the second time
it happened was one time too many before checking which path actually runs.

The next step is to position the host field from the BG scroll registers in
effect for the frame being composited — the values `func_020396E0` wrote — so
the bands are drawn from the same camera the native columns are, by
construction, rather than from a value sampled at a different moment.

## RETRACTED IN FULL: "lb_widemap_fill never completes" and the 4 px shake

Both claims below are **wrong**, and the way they were reached matters more
than the claims.

`fill ok=0 bail2=1711` was read three times as "the fill never completes". The
counters were only printed **from band_compute's fallback**, so the probe went
silent at exactly the moment the battle began, and frozen boot-phase numbers
were read as if they described gameplay. The `[lb_locate] chain` line proving
the map was found sat **nine lines below** the last counter line in the same
log.

Printed unconditionally, through the battle:

    entries=4201 ok=2478 bail1=0 bail2=1720 bail3=0 bail4=0 bail5=2
    geom=1 map_w=64

The fill completes 2478 times. Every one of the 1720 bails is before the map is
located, where bailing is correct. **Nothing was broken here.**

The 4 px band alternation goes with it. `band=188` on 14 samples and `band=192`
on 6 is not the `span/2` fallback: **zero** fallback firings occur after the
locate. Both values are the clamp's real output, which shifts legitimately as
the camera nears the world edge.

Three code changes were made against the wrong reading and all three produced
**byte-identical counters** — which was the signal, and it took three
repetitions to act on it. The last of them was also a latent regression: it
gated the fill on a cached map chain, and a cached chain persists after a
battle ends, so menus would have passed the gate and drawn a stale battle's
bands over them. Reverted; the original test is restored and verified
(`ok=2283 geom=1 map_w=64`).

### The rule this earns

A probe must not be gated on the condition it is investigating. This one only
spoke while the fallback fired, so it could report a problem and never report
its absence.

## SUPERSEDED (kept for the record): lb_widemap_fill never completes

Measured on the replayed level, over ~1800 frames:

    fill ok=0 bail1=0 bail2=1711 bail3=0 bail4=0 bail5=0

Bail 2 is the DISPCNT gate, and it fires on **every** frame, so
`g_geom_valid` and `g_map_w` are never set. Consequences:

* `band_compute()` always takes its `span/2` fallback — 192 on a 640 surface
  against a clamped 188. Measured with `LB_HUDLOG`: 15 frames reported
  `band=188` and 6 reported `band=192`. **That 4 px alternation is a real
  residual shake**, separate from the camera tracking fixed above.
* The earlier "hold the camera across a short absence" fix is **dead on
  arrival**: it restores `g_cam_px_was_valid`, which can never have been true
  if the full path has never run once. A bootstrap that cannot bootstrap.

Relaxing the gate from the BG-enable bits to a map-chain test did **not** help,
and the reason is a trap worth recording:

    gate saw dispcnt=00010D0B map=022BD750 w=0 h=0 t338=00000000 row0=31111111

`022BD750` is the SAME map object `lb_field`'s chain reads correctly as 64x64
with tables at `022BDAE8`. Here `map+0x14` reads 0 and `map+0x34` reads
`0x31111111`. Same address, same frame, different values.

### RETRACTED: "this is a threading problem"

The obvious reading was that `band_compute()` runs in the fanned-out row body
while `lb_hostterrain_prepare` runs on the main thread. **Measured, both print
the same thread id (2850328247).** It is not threading.

Instrumenting the gate itself, and the same addresses from
`lb_hostterrain_prepare`, at the same cadence in the same run:

    [lb_gate]      tid=2850328247 dispcnt=00010D0B mode=1 map=00000000 w=-1
    [lb_gate]      tid=2850328247 dispcnt=00010D0B mode=1 map=022BD750 w=0
                   t338=00000000 row0=31111111
    [lb_gate_main] tid=2850328247 map=022BD750 w=64 h=64 t338=022BDAE8
                   row0=0227F72C

Same thread, same run, same addresses, different values. So it is **timing**:
`lb_widemap_fill` is called at a point in the frame where the map state is not
valid — the pointer at `0x02156CB0` reads 0 outright on most of its samples —
while `lb_hostterrain_prepare` is called at a point where it reads correctly.

`mode=1` passes, so the display-mode half of the gate was never the problem,
and relaxing the BG-enable bits could not have helped: the map read beside it
fails too.

**The fix direction** is therefore to stop `lb_widemap_fill` reading guest
memory at its current call site — either move the call to where the state is
valid, or capture the map chain at `lb_hostterrain_prepare` time and hand the
values to it. Which of those is right depends on what else in `fill` depends on
being called where it is, and that has not been established yet.

## State after the camera fix, re-verified post-revert

    of 894 MOVING pairs, 0 disagree (0.0%)
    host-guest dx: 48 on all 1696 rows
    [lb_phase] agreement by cam_tx%3: 0: 100.0% of 489324
                                      1: 100.0% of 5016
                                      2:  99.3% of 36300

Camera tracking is exact and the fog agrees with the guest's own at ~100% on
all three metatile phases. Deployed.

## The drag crash: not reproduced

Two purpose-built routes, `--drag-native` and `--drag-boundary`, both ran to
completion without crashing. So either the crash needs a state these routes do
not reach, or it needs the reported window size (the routes report every
requested size collapsing to a 512x192 guest — the `--window-size` resize does
not take, see above; only `LB_WINDOW` works). **Cause found 2026-09-24:** the
resize goes through lb_capture.ps1 restricted to the runner's pid, and under
MSYS python lb_exp.py passed the MSYS pid, which no Windows window carries, so
every resize (and every window capture) silently failed. Fixed in `9d7f447e`:
the end-of-run sweep now reports 640x480 -> 306x216, 1280x720 -> 626x336 and
so on.

`--drag-boundary` printed "NO frontend offset selects the hero in band", which
looks like a selection bug in the widened columns and would explain "the HUD
does not draw when a unit is selected". **It is not usable evidence**: the
CONTROL case, a drag entirely inside the native region, also selected nothing
(`hud 1 -> 1  nothing`). An instrument whose control fails cannot grade the
test case. The known cause is recorded already — every `moveto` is a pen-down
and a raw touch is sticky, so only press-move-release is a real drag, and these
probes do not do that.

**To make progress on either, real input is needed**: a `.lbdemo`
recorded on a run where the drag crashes, or where selecting a unit fails
to draw the HUD. The replay harness reproduces those exactly; the scripted
probes do not.

## THE HARNESS NOW HONOURS --window-size, and it immediately found a bug

`tools/lb_exp.py --window-size` now sets `LB_WINDOW` as well as attempting the
PowerShell resize, so the size is applied at window creation where nothing can
silently fail:

    window-size: 1855x988 confirmed by [lb_geom]
    [lb_window] initial window 1855x988
    [lb_geom] window=1855x988 guest=928x494 scale=2.000

Every frame-rate and geometry number this harness produced before today was
measured at **1024x384 regardless of what was asked for**.

### RESOLVED: the 18.5% was the METRIC, not the renderer

The comparison ignored `g_obs`, the companion "has this cell been displayed
yet" mask. `g_seen` starts at 1 (shrouded) everywhere and is written only where
the guest's own columns have been scanned, so every cell the camera had never
toured counted as "the grid says revealed, the screen says shrouded" — a
disagreement manufactured by the metric.

It bites hardest at LARGE surfaces because a wider view clamps the camera
further from the map edges, leaving more of the map never displayed. Adding the
guard:

| cam_tx%3 | 0 | 1 | 2 |
|---|---|---|---|
| before | 97.4% | 99.2% | **18.5%** of 242,880 |
| after | 95.6% | 100.0% | **98.8%** of 512,424 |

**There is no fog rendering fault at the 1855x988 window size.** The shroud
agrees with the guest's own at 95.6-100%. A rendering fault would not care how
much of the map the camera had toured, which is the tell that separates the two
and is worth applying before believing any agreement number.

### Superseded: "fog agreement collapses on one phase"

At 1024x384 (what every previous run actually measured):

    [lb_phase] agreement by cam_tx%3: 0: 100.0%  1: 100.0%  2: 99.3%

At 1855x988, the reported size:

    [lb_phase] agreement by cam_tx%3: 0: 97.4% of 142032
                                      1: 99.2% of 185328
                                      2: 18.5% of 242880

**18.5% on one residue, over 242,880 samples.** That is "the shroud is patchy",
reproduced and quantified, and it was invisible while every run silently ran at
the small size.

The file's own rubric says what this shape means: *"A misalignment shows up as
one bucket near 100% and the others far below; a wrong record shows up as all
three alike."* Two buckets near 100% and one collapsed is a misalignment that
bites at one camera residue.

Where to look: the comparison samples `tx` in steps of 3 and indexes
`mx = floor_div(cam_tx + tx, 3)`, so the sub-column it samples inside each
metatile is **fixed by `floor_mod(cam_tx, 3)`**. A one-column origin error
therefore shows on exactly one residue and hides on the other two. Whether the
error is in the sampling or in `vis_revealed`'s own indexing is not yet
established, and the two need separating before anything is changed — the fog
record itself grades 100% at the small window, so it is not simply the wrong
record.

fps at that size: 46.49 with `cache_absent_itcm = 0`.

## BASELINE at a 1855x988 window, 200 consecutive frames

`LB_WINDOW=1855x988` (guest 928x494), replaying the recorded demo:

| measure | result |
|---|---|
| camera tracking | **0 of 1237 moving pairs disagree (0.0%)** |
| host - guest dx | 48 on all 1907 rows (constant offset, not a lag) |
| left seam | mean 0.96, max 1.99, **0** frames above 3x |
| right seam | mean 1.08, max 2.01, **0** frames above 3x |
| fog agreement by cam_tx%3 | 95.6% / 100.0% / 98.8% |
| frame rate | 49.86 fps with `cache_absent_itcm = 0` |

### RETRACTED: "the right seam spikes in 20 of 200 frames"

That was the instrument. `seam.py` divided each boundary difference by
reference columns sampled at `(extra//2, extra+60, extra+180, far right)` --
two of which sit deep in the BANDS, where the content is low-detail fog. A
boundary difference divided by a fog-region baseline reads high regardless of
whether there is a seam.

Scanning columns 584..601 directly settles it: at the true boundary pair
(x=591/592 on a 928 px surface, native spanning 336..591) the difference is
**0.69 of its own neighbourhood** -- lower than the columns either side of it.
There is no discontinuity there at all.

`seam.py` now takes its reference from six columns immediately beside each
seam. Compare like with like, or the ratio means nothing — the same error in
different clothes as the `g_obs` one above, and the third measurement artifact
in this file to be mistaken for a rendering fault.

## Window adaptivity, swept

| window | guest surface | scale | view vs world (1536x1024) |
|---|---|---|---|
| 1024x600 | 512x300 | 2.000 | inside |
| 1855x988 | 928x494 | 2.000 | inside |
| 2560x1440 | 1280x720 | 2.000 | inside |
| 3440x1440, scale 1 asked | **1536x642** | **2.243** | **capped at the world** |

The last row is the case the zoom floor exists for: a scale of 1 on a 3440 px
window would put a 3440 px view against a 1536 px world, which has no valid
camera range and draws black either side. The floor raises the scale until the
surface is at most the world, so the whole map is in shot and no further —
which is what an RTS zoom-out limit is.

Every size reports `window-size: WxH confirmed by [lb_geom]`, which is only
possible now that `--window-size` actually applies.

Minor, not chased: at the capped size the field rect reads `field=-2,0
3445x1440` against a 3440 px window — 1536 x 2.243 = 3445, so the surface
overshoots by 5 px and is centred at x=-2, cropping 2-3 px each side.

## The HUD classifier is correct; two readings of it were not

**"The z threshold is stale: HUD z is ~6.6M against a 4.3M threshold."** Wrong,
and wrong the same way as the seam denominator. `rejected: z=5 (lowest
6659584, threshold 4300000)` reports the lowest z among **REJECTED** polygons —
the nearest piece of world geometry. The documented 4259328..4267520 range is
the **ACCEPTED** HUD population. Comparing one against the other says nothing;
6.6M being comfortably above 4.3M is the threshold working, with margin.

**"The offscreen guard wrongly rejects band-region HUD."** Also wrong. Setting
`LB_HUDZ=7500000` let more polygons past the depth test and they were then
rejected as offscreen — which looked like a second bug, and is the guard doing
its job on the world geometry the raised threshold had just admitted. The guard
is deliberate and measured:

> Measured with a unit selected, the list contains quads at surface x 0..10 --
> native -96..-86, already out in the widened column. A screen-locked HUD
> element cannot be there; those are world geometry.

Health bars are **world-anchored** — they sit above whichever unit they belong
to — so a quad out in a band is correctly not treated as screen-locked HUD.

What is real, and still unexplained: hudquads tops out at **6** on a 928x494
surface against **13** at 512x192. That is a genuine difference between the two
sizes, but the two runs are different moments of different routes, so it is not
yet evidence of a fault. Comparing it needs the same scene at both sizes.

## The 26.9 fps: it is the SOFTWARE 3D renderer, and the GPU path is a known dead end

A play run measured 26.9 fps at 1855x988, which is borderline unplayable.
The visible-rect fix above saves ~1.0 ms of a ~37 ms frame — under 3%, and not
the cause.

The cause is in that run's log:

    [gpu3d] compute renderer init failed
    [gpu3d] OpenGL auto-selection failed; using threaded software

`nds_gpu3d_use_compute_renderer` has two gates: `render_width > 448` and
`render_width % 64 != 0`. A 928-wide guest surface fails **both**
(928 mod 64 = 32). So a capable GPU sits idle while the DS's 3D hardware is
emulated in software over ~4.7x the pixels the DS ever drew. Wider window,
more software rasterisation.

**I attempted to raise the cap and was wrong to.** apply_hooks already records
the experiment, and the answer:

> Raising it to 1024 does start the renderer -- 33 compute shaders compile and
> the log reads "[gpu3d] renderer: OpenGL 4.3 compute" instead of "using
> threaded software" -- and then the process SEGFAULTS a few lines later,
> **reproducibly**, both under the harness and run directly. ... Shipping it
> would hand the player a crash in place of a low frame rate, which is a worse
> bug.

My hook collided with that existing one on the same anchor, which is the only
reason the build failed rather than producing a crashing binary. Reverted.

The recorded next step is unchanged and is the right one: **bisect by raising
the ceiling to 512 alone** — the smallest width that reproduces — with the
readback mapping and the `SetRenderWidth` path under a debugger. There are more
448- and 192-derived assumptions inside the compute bridge than the two buffers
that were already fixed.

Until that is done, the honest position is: **wide windows cost frame rate by
construction on this port**, and `LB_HOSTTERRAIN=0` plus a narrower window are
the only levers the player has.

## Still unexplained

Also reported: the native strip jittering, camera and sprites shaking
while moving, no HUD when a unit is selected, and a crash on drag. The drag
crash does not appear in any log: the recorded run closed cleanly after 4,840
frames (`[sdl] closed`), and replaying the demo does not crash. None of these
are explained yet, and none should be assumed to share a cause with the above.
