# 350 — The Eagle base holds; effects were culled to the DS screen

Written for: whoever audits the widened field next.

Status: **built, measured, deployed** 2026-09-23 17:05 --
`build/LegoBattles.exe` sha256 `53c078c0...`, the player's settings.json kept
(and identical to the harness's pinned copy, so the deployed configuration is
the one measured). Section 3.4 has the particle A/B that cleared it.
Superseded at 19:17 by `7875b377...`, which carries the same fix plus
docs/351's steady pacing; the exactness gate is identical on it.

Two results from one audit pass over a recorded player battle
(`build/lb-session-1790122638.lbdemo`, an 842x474 surface):

1. **docs/324 item 7, "eagle command base sprites flicker/truncate seemingly
   random", does not reproduce on the current build.** The base in that
   battle is the recording player's own. 2,000 consecutive frames of it (three windows,
   covering selection, camera pans and the base crossing between the native
   columns and a band) show the building complete in the 3D layer and 100%
   of it reaching the presented frame whenever it is on the surface.
2. **A real adaptive-only defect, found in the decomp while checking item 7's
   last candidates: the cartridge culls effects and particle systems to the
   DS screen with its own test, outside the gather rect.** Every explosion,
   spell, construction effect, spell icon and particle system whose anchor
   lay in the left, right or bottom band was never drawn. Fixed by widening the
   cartridge's own compares (35 instructions in two functions).

## 1. docs/349 was wrong about the map

docs/349 section 9c closed its flicker check by saying the defect was on the
Eagle Command base (Earth), which this recording's map does not have. It does
have one: **it is the recording player's own base.** The game log says so --
`[lb_music] owner 0's builder is blueprint 82 -> side 4 (Earth)` in all five
recorded sessions -- and docs/12's faction table gives Earth's
base as `EagleCommandBase` (blueprint 90). The port's entity report puts it at
world (331,239), owner 0, category 7. Retracted in place in docs/349.

The 300 frames docs/349 measured (composite frames 2785-3084) had the base in
view the whole time, in the right band: the largest object in the 3D layer,
53 px wide at surface x 373..425, 1,957-1,991 opaque pixels, with the builder
standing in front of it. So that measurement was already a measurement of the
Eagle base; it was just not recognised as one.

## 2. Item 7 on the player's own base

### How the cartridge draws it (decomp)

`Building_Draw3D` (0203B3AC) draws every building as a textured MODEL under
its own orthographic projection, `MTX_OrthoW(0, 192, 0, 256, -512, 512)`:
screen pixels, y down, no camera matrix. The camera is applied in software
(`Building_WorldToScreen`, 0203B2CC: world minus `(camx - 4, camy)` plus half
the footprint), and the depth is `z = base + screen y` (+0xBF for one model
flag) for painter's order. Two details matter for a "flicker" report:

* **The team colour is a per-material palette offset**, set on every draw
  (`material+0x1C = palette + colour*2`, colour 5 mapped to 6). That is the
  palette entry docs/297 could not get from the static sheet (`#FF4D24`
  against the sheet's `#FB5100`): it is the owning player's colour, applied at
  run time. docs/297's open "NOT investigated" line is answered.
* **Selection is shown through the polygon ID**: 0x10 when the building is
  selected, 0x18 otherwise, 8 for one flag -- edge-marking groups, so a
  selected building gets an outline colour. Alpha is 31 unless a global flag
  at `*(*0x020092B4)+0x20` is set, when it is 0 (the DS draws alpha-0
  polygons as wireframe).

### The measurement

`basetrack2.py` (scratch) follows ONE object through a consecutive
LB_DUMP3D recording: seeded at the base's position, followed by nearest
centroid with an area-continuity test (a unit walking past cannot be taken
for it), and re-acquired by the base's own 53-px footprint after a camera
jump. Per frame it records the object's opaque area in the 3D layer, how
many of those pixels reach the presented frame unchanged, and its bounding
box, and flags a frame whose area or visible count drops below 85% of both
neighbours, or whose box shrinks, away from the surface edge.

| window (composite frames) | surface | what happens in it | frames | flagged | visible fraction |
|---|---|---|---:|---:|---|
| 2785-3084 (docs/349) | 842x474 | base in the right band, camera pans y 81 -> 200 | 300 | 0 | 1.000 every frame |
| 1300-2299 | 842x474 | base selected twice (frames ~1623, ~2283), camera (32,160) -> (-48,80), a construction beside it | 1,000 | 2 (see below) | 1.000 every frame |
| 100-799 | 842x474 | the opening pans: base native -> left band -> native, then panned off the top into shroud | 700 | 0 | 1.000 every frame on the surface |

The two flags in the middle window are the builder: with its yellow
selection outline it walks into the base's connected region and out again,
so the box widens and narrows while the base's own pixels do not change. A
strip of the eight frames around them shows the base complete in every one,
with its health bar above it. The 72 frames the tracker reports "lost" in the
last window are the player panning into the shroud: the base's box reaches
y 0 and shrinks into the top edge on both sides of each gap, which is the
base leaving the view, not failing to draw.

**Not reproduced.** The report predates docs/325-349. Two flicker mechanisms
were already closed before it (docs/267, docs/310), the one-frame 3D/BG
pairing lag was fixed after it (docs/348), and so was the camera cadence
(docs/349). Which of those the 09-19 report described cannot be recovered
from a report with no frame or capture; the current build does not do it on
the player's own base in their own battle.

## 3. Effects and particles were culled to the DS screen

### 3.1 What the cartridge does

The gather rect (docs/208/210, `apply_cull_width`) decides which ENTITIES are
drawn. Effects and particle systems do not pass through it. The render task list
draws them in tasks 0x1F and 0x20,
whose list walkers (020294F8, 02016910) have no cull of their own and call
each object's draw. The draws cull, through one of two helpers:

**`func_02016070`**, one anchor point, disassembled:

```
0201609C  ldrsh r2, [r3, #0x5E]      camera x
...                                  r5 = screen x, r0 = screen y
020160CC  cmp   r5, #0
020160D8  blt   reject               x < 0
020160DC  cmp   r5, #0x100
020160E0  ble   test_y               x <= 256
020160E4  ... mov r0, #0             reject
020160F0  cmp   r0, #0xC0
020160F4  movgt r0, #0               y > 192: reject   (no top bound)
```

**`func_02016110`** (named `Projectile_WorldToScreen` in the Ghidra corpus;
it is really a rect test): an effect's rect, given as a position and a size,
is accepted if any of its four corners is on the 256x192 screen or if one of
its edges spans it. 32 compares against `#0`, `#0x100` and `#0xC0`; corner x
lives in r1/r3/r4/r5 and corner y in r0/r2, each loaded by the `ldrsh` just
before the compare.

Which draws use which, from vtable slot 5 of every `Battles::*Effect` class
in `lb_fxvtables.h`, read from the ARM9 image:

| helper | callers (the draw returns before submitting anything when it fails) |
|---|---|
| point, `02016070` | ArrowHighLight, ArrowVolley, Artillery, ClusterBomb, CoconutStorm, CrabSwarm, DrawBox, EAttack, FireBall, ForrestSpawn, Freeze, GenericExplosion, LightningBolt, MonkeySwarm, Roar, SpaceLaser, ThunderHammer, Upgrade |
| rect, `02016110` | BuildConstruction (a 48x48 rect at effect+0x10), BridgeConstruction, HealthBoostTrailStar, HeroStartSpell, all ten SpellIcons classes -- and the PARTICLE SYSTEMS, twice (below) |
| neither | Flash, Jiggle, Teleport, Victory (no screen-bound compare at all); EarthQuake, HotWire, Tracking (the draw is a bare return) |

**The particle systems, which the audit had down as "projectile(?)".** Render
task 0x1F's walker `020294F8` and a second walker `020294A8` both iterate the
list at `*(0x02151D14)+0x54`, linked through +4, whose nodes the
ParticleSystem constructor `02029AE8` builds (it stores vtable `02144060` and
the +4/+8 links -- the chaining docs/237 saw). `020294F8` draws each node with
`0202A3EC`. `020294A8`, reached from `020836A0`, calls `0202A058` on each node
and, when that returns non-zero, `02029674` on it -- and **`0202A058` is the
system's update**: unless bit 3 of +0x40 is set, it rect-tests the system's
bounds (position at +0x18) and returns 0 before emitting anything when the
test fails; otherwise it allocates new particles from the pool at
`*(0x02151D84)+4` (skipping one when the pool is empty) and places them
relative to the camera. So a particle system in a band on the stock code
neither draws nor EMITS. That second half is not drawing, and it is the one
place the fix changes more than pixels -- section 3.4 measures it.

The accepted positions go to the geometry engine: the draws write MTX_MODE,
MTX_IDENTITY, PUSH/POP and MTX_TRANS (0x04000440..0x04000470), so these are
3D quads, never OAM sprites, and a band position renders exactly where units
already render.

### 3.2 Why docs/211 missed it

docs/211 closed "effects in the band" on *"There is no separate cull for
effects"*, from a scan for the camera-x load paired with a VIEW-WIDTH ADD --
the gather rect's shape. These helpers subtract the camera and compare the
screen position with literal 0, 256 and 192; there is no add. `0201609C`,
the camera load inside `func_02016070`, is one of that scan's own 30 sites.
docs/237 then established that effects are POSITIONED in the bands and that
the band columns are drawn, and said in so many words that it did not show an
effect sprite drawn there. Retracted in place in docs/211.

### 3.3 The fix

`apply_fx_cull()` in `port/runner/src/lb_patches.cpp`, run every vblank beside
`apply_cull_width()`. It makes the cartridge's own test the test for the
bigger screen: every lower bound becomes `CMN rN, #span` (flags of rN + span,
so the same lt/ge branch now means rN < -span) and every upper bound
`CMP rN, #256+span` or `#192+span`, where span is the whole overhang
(`lb_band_span`, `lb_band_span_y`) -- sized for the worst case on each side
for the reason the gather rect is: the band's split slides at map edges, and
guest code must not be rewritten as the camera moves. Immediates are rounded
UP, so the test stays a superset; an effect accepted just off the surface is
clipped by the 3D engine, as the cartridge relies on at its own edge. Every
compare of one kind gets the same value, which keeps the rect test's
spanning cases consistent with its corner cases.

It only ever replaces the stock word or one it wrote itself (same register,
either compare form), puts the stock words back when Enhanced is off,
`LB_WIDEMAP=0` or `LB_FXCULL=0` (the A/B switch), and warns once if the code
is neither. At the 842x474 surface:

```
[lb_fxcull] effect/projectile screen test widened: x -588..844, y -284..476
            (was 0..256, ..192; 35 of 35 compares written)
```

Patched words fail the recompiler's content validation for those two
functions only, which then run in Tier 3; every other function in their
4 KB page still validates and stays lifted -- the same arrangement the
gather rect has run under since docs/208.

### 3.4 Measured

**Where effects fall in the recorded battle.** `LB_FXLOG=1 LB_FXPERIOD=6` over
the replay found one class in a band: the player's own `BuildConstructionEffect`
(@0230DAD0) in the RIGHT band from composite frame ~1683 to 2103, beside their
base -- the same battle, the same base, whose band docs/349 and section 2 were
looking at. Its draw hands the rect test a 48x48 rect at effect+0x10, which is
where the sweep reads its position.

**A/B, one build, one switch.** The new build twice over the recorded replay at
842x474, 300 consecutive frames (1650-1949), `LB_FXCULL=0` against the default:

| | frames with a 3D difference | pixels added | pixels removed | pixels recoloured | where |
|---|---:|---:|---:|---:|---|
| fix on vs fix off | 181 of 300 | 25,162 | 0 | 4,888 | all inside x 403..418, y 256..279 |
| fix off vs the previous build | 0 of 300 | 0 | 0 | 0 | -- |

Every difference is one 16x24 sprite beside the Eagle base: the golden LEGO
brick of the construction effect, bobbing through its animation, drawn by the
fixed build and absent from the stock one (`fxcull_ab.png` in scratch: base
selected, brick at its lower right, top row stock). "Recoloured" is where the
brick now covers the base's own shadow. The presented frame differs by exactly
the same pixels, so it reaches the screen. The position is the cartridge's own
arithmetic in both arms -- the patch changes only accept/reject -- and it is
drawn by the same widened 3D path that already places units in the band.

**The simulation does not move.** Every state line both runs log up to the
replay's end -- per-second entity positions, both bases, builders, heroes,
the selection set, the camera -- 2,056 lines, identical. The draws write
only geometry-engine registers (ArrowVolley's final store, `*0x0200F4DC = 1`,
is MTX_POP at 0x04000448) and the per-sprite UV rect they compute from the
animation frame, and this is the measurement that says so for a whole battle.
(Lines AFTER the replay ends diverge between any two runs: that is the
harness's own camera probe on wall-clock time -- see the exactness gate's
known trap.)

**Particles: no random numbers, a pool of their own.** The one behaviour the
fix changes beyond pixels is that a particle system in a band now EMITS. If
emission drew from the game's random-number stream, a wider window would
change combat outcomes -- and two lockstep players at different window sizes
would desync. It does not: `0202A058`, the update, contains no multiply at all
(no inlined generator) and calls only the particle pool's allocator
(`020F16C4`, 0x48 bytes, no calls; pool at `*(0x02151D84)+4`), the vector
constructor, the world-to-screen helper `02015FBC`, the bounds getter
`0202A8E0`, a lookup by id (`02029448`), a list insert (`020F167C`, whose two
callees are 0x28-byte leaves) and a one-word copy (`02004344`) -- every one
of them a leaf with no multiply. The draw `0202A3EC` adds fixed-point
conversions (`0204F398`/`0204F3D0`), a fill (`0211069C`) and geometry
commands. None of them reaches a generator. The
replay above had no particle system in a band (all 3,306 samples off the
surface), so it was checked where there is one.

**Particles, measured: an input-free linger into the AI's attack.** The
recorded replay again, then 1,100 s with no input (`--demo-linger`); the AI
builds and attacks, and band particle systems appear beside the player's base
from composite frame ~23,700. Fix on against `LB_FXCULL=0`, recorded 23,650-
23,949: with the fix a dust cloud is drawn around the enemy fighting at the
top right of the explored ground, in the right band; without it only that
unit's health bar is there (`linger2_on.png` / `linger2_off.png` in scratch).

**Does the simulation move?** The state lines part after the replay: 5 isolated
samples in 7,700-10,400, then every sample from 22,623 -- moving units 1-3 px
apart, later 5-20; at 34,803 the same 45 entities with the same category
counts (the player's base fallen in both), three AI builders standing at the
same places under DIFFERENT heap addresses. Two readings: the fix changes what
the simulation does (heap or particle state feeding it), or only when its
steps land. Counting the cartridge's own iterations decides it (LB_LIVE3D_PROBE
swap-exec, one per pass of the task list, which runs one simulation step --
task 0x0A, docs/351 section 1):

| composite frames | iterations, fix on | fix off |
|---|---:|---:|
| 60-6,300 (the replay) | 3,817 | 3,817 |
| 6,300-15,000 | 4,396 | 4,396 |
| 15,000-22,000 | 3,479 | 3,479 |
| 22,000-26,000 (band particles) | 1,967 | 1,974 |

and at every one of 269 post-replay samples where both runs had completed the
SAME number of iterations, the states (positions, pointers ignored) are
identical in 263; the other six are samples taken mid-iteration, one step
apart (1-3 px on a moving unit). **The simulation does the same steps; the
extra band-particle work only schedules them later** -- 7 iterations of
1,974, 0.36%, in the heaviest stretch -- which is what a DS does when its
player looks at a busy fight. Lockstep turns fix the step sequence, so this
cannot desync a match; and the heap addresses the steps allocate at are not
read by the simulation (identical positions under different pointers), as
the stock game's own camera-dependent particle lifetimes already require of a
two-player DS match.

**Cost.** The two patched functions fail content validation and run in Tier
3, so their cost shows up there. LB_SAMPLE over the whole recorded replay at
842x474: the "tier3 interpreter" group is 0.26% of main-thread samples with
the fix and 0.16% without -- 0.34% against 0.25% of the time the thread was
not waiting, i.e. under 0.1% of a frame, about 0.015 ms. Tier 3 went from
975 to 1,079 blocks. Nothing a player can feel; the wall-clock fps samples
from the same afternoon (13.4 vs 15.3 ms of emulation in two single post-demo
windows) moved by twenty times that with the machine at 57% load from other
programs, which is the reason not to grade a cost from them.

### 3.5 Gate

The exactness gate of docs/349 section 7 -- the recorded replay at 1473x830
under the pinned harness settings, `LB_COUNTS_AT=1500,3000,4500,6000,7500` --
on the new build:

| frame | reference insn9 / insn7 | `LB_FXCULL=0` | fix on (x -480..736, y -222..416) |
|---:|---|---|---|
| 1500 | 128,639,175 / 23,269,124 | identical | identical |
| 3000 | 467,336,666 / 40,632,466 | identical | identical |
| 4500 | 884,105,614 / 62,577,298 | identical | identical |
| 6000 | 1,322,302,763 / 83,387,395 | identical | identical |
| 7500 | 1,769,454,450 / 104,969,363 | identical | identical |

With the switch off the build is the previous one to the instruction. With
the fix on the counts are also unchanged at this surface, and that is not a
contradiction: the replay's taps land differently at 736x415, the battle is not
the 842x474 one, and no effect fell into a band before frame 7500 there -- the
Tier-3 interpreter counts instructions too (`nds_note_insn_retired` in
`io.cpp`), so a newly accepted effect, and the draw it releases, would have
shown. What the gate proves is that the
patch disturbs nothing when no effect is in a band: the widened compares make
exactly the decisions the stock ones did. The 842x474 A/B above is the case
where one is.

## 4. Found on the way, and fixed: the old fog finder's drop loop

Left unattended after the recorded replay (the harness missed the replay's
end, section 5), the AI played on and attacked the player's base -- siege
units on the field, the player's hero dead, a FlashEffect and particle
systems beside the base; from composite frame 30,963 the port logged, three
lines a frame for the rest of the run (20,872 drops):

```
[lb_findfog] 02255BA8 disagrees 126/132 (126 revealed over shrouded, ...) -- OPENS HIDDEN GROUND, dropping now
[lb_findfog] DROPPED 02255BA8: 126/132 disagree with the guest's own fog, 30 frames running
[lb_findfog] LOCATED on frame 0 (no wait): 64x64 bits, rows at 02269E44, stride 8
```

**It is a false positive.** The grader (`lb_widemap.cpp`, the `g_vis_bad_streak`
block) compares the record's EXPLORED bit (`vis_revealed`: holder+0x10) with
`g_seen`, whose "shrouded" is `BG2 tile != 0x064` -- that is, NOT CURRENTLY
VISIBLE, which includes the fog-of-war set (0xF5 and edges) that docs/348
showed the cartridge draws over explored-but-unseen ground. With no unit of
the player's in the native window, every explored cell there is fog-of-war,
the two disagree on 126 of 132 samples in the "revealed over shrouded"
direction, and the record is dropped as "cheating". The drop re-arms the
search, `locate_fog_record` walks the pointer chain -- which never consults
`g_vis_bad` -- and adopts the same record the next frame.

**What it costs, measured from the code rather than assumed:** not the
picture. The band's fog comes from docs/348's exact rule, `fill_fog_for`'s
first branch, which reads both grids through its own chain
(`fx_read_record`) and returns before any `vis_revealed` use; those sit in the
fallback taken only when `LB_FOGEXACT=0` or the record is unreadable. What
remains is a relocate and a grade each frame and about 180 log lines a
second into the player's session log whenever their native view holds no
unit of theirs -- a base area the enemy has cleared, or explored ground
with nothing on it.
Six player session logs in `build/logs` hold two drops between them
(one session), and the 42 harness logs beside them 2-5 each; the steady loop
needs the view to stay unoccupied.

**Fixed: explored against explored.** The police now hold the explored bit
against the cartridge's own tile FAMILIES, derived from Fog_CodeToTiles' two
tables (the ones docs/348's exact rule already reads): a tile only the sets
for an UNEXPLORED centre use is cloud, a tile only the sets for an explored or
visible centre use is clear or fog-of-war. The ROM's tables split cleanly --

```
[lb_fogkind] 46 cloud tiles, 22 clear/fog-of-war tiles, 0 used by both
```

-- so each displayed cell's family is stamped beside `g_seen`
(`g_seen_kind`), and a record is "opening hidden ground" only where the
screen shows CLOUD. Codes with a field of 3 are skipped (Fog_NeighbourCode
never forms them); without the tables the old comparison stands.

Re-run of the case that looped -- the recorded replay, then 600 s with no
input at the cartridge's cadence (`LB_CADENCE=20`, as the original run had):
the game reaches the same moment, the player's base falling between composite
frames 30,963 and 31,023 (bases 2 -> 1, 44 entities, the player's hero already
dead), and the fixed build logs **0 drops and 1 location** -- the first
adoption -- through frame 38,043 and the probes after it, against 20,872
drops before. Nothing about the picture changes: the band fog is docs/348's
exact rule either way. Deployed 19:52 in `ee0ac8e8...` (settings kept): the
exactness gate identical at all five frames, and Vanilla pixel-identical to
this morning's deployed build over 60 consecutive frames of both screens
(240 of 240 images; neither patch writes anything in Vanilla).

## 5. The harness missed the replay's end

`lb_exp.py` watched for `[lb_demo] finished` in the last 20 KB of the log,
polled once a second. `LB_FXLOG=1 LB_FXPERIOD=6` writes faster than that, so
the line scrolled out between two polls and the fix-off run waited out its
900 s ceiling ("NEVER FINISHED") -- which is how the unattended state of
section 4 was reached. It now reads everything written since the previous
poll. `--demo-linger N` makes the same thing deliberate: N seconds with no
input after the replay, which an input-free game reproduces frame for frame.

## 6. Not covered here

* **Projectiles proper.** The helper the Ghidra corpus calls
  `Projectile_WorldToScreen` turned out to serve particle systems and rect
  effects. The field draw (task 0x1D, 02039148) walks three lists -- units
  through 0203ABBC -> 0203ABC8, and two more through 020EBF34 and 020EBB70
  -- and none of those draws has a screen-bound compare (the one `cmp r5,
  #0x100` in the unit draw, 0203AE3C, is a case of a switch on a mode value
  0..8/0x10/0x40/0x80/0x100). Whatever they draw therefore rides the widened
  gather rect. Which list arrows and cannonballs are on was not named.
  **Answered in docs/353 s5, and corrected there:** the "three lists" are one
  gathered list walked five times. Unit projectiles (`Battles::Sim::Projectile`,
  entity kind 1) are gathered through `020882BC` into it and drawn by
  `020EBF34`, which `0203AB00` calls, and 020EBB70 draws collectables. No
  screen-bound compare is on that path, so projectiles in the bands are drawn.
  The effect helpers widened here never see them, so this doc's
  "effect/projectile" wording in the log line was wrong. It now reads
  effect/particle.
* **docs/297's wireframe flag**: which global `*(*0x020092B4)+0x20` is was not
  identified. Nothing measured here drew a building as wireframe.
