# 325 — Making it a PC game: the native renderer, planned

> ## SUPERSEDED, 2026-09-20 — this renderer is OFF by default
>
> The status table below says "phases complete, deployed" and "the rendering
> half is verified". **Both are withdrawn.** On 2026-09-20 a screenshot of the
> deployed build showed it badly broken, and `LB_HOSTTERRAIN` now defaults to
> **off**. What the table got wrong, and
> why, in the order it matters:
>
> * **"Phase 1 — host terrain is cheap. done, measured."** Measured at
>   512x192, alone, without the shroud pass or the fog reads layered on it. On
>   a 2304x1536 map at a large window it is **2.587 ms/frame** whole-map
>   against 0.430 ms clipped (docs/330).
> * **"Phase 2 — bands drawn from the level grid. done."** Measured
>   contributing **one band pixel in 672** on the middle output row: 86 samples
>   at 0, 17 at 1, 5 at 4. `lb_widemap`'s band synthesis overdraws it. The port
>   was paying for two terrain renderers and showing one (docs/334).
> * **Every phase was verified on a level where `lb_field_locate` SUCCEEDED.**
>   On the level actually being played it failed until `5263b07b`, so none of
>   this had ever run there. "Verified" meant "verified on one route on one level",
>   which docs/327 already recorded as the lesson and this header did not.
>
> The plan's *analysis* is still worth reading — the frustum retraction, the
> confounds, the architecture. Its *status claims* are not. See docs/334 for
> the two-renderer situation, docs/336 for why the widened view fills with
> shroud, and docs/335 for the jitter.

## STATUS (as written 2026-09-19, superseded above): phases complete, deployed

| phase | state |
|---|---|
| 1 host terrain is cheap | done, measured |
| 2 bands drawn from the level grid | done |
| 3 fog from the bitmask | done — and the fix that made it correct was letting the GUEST draw its own fog over the widened columns |
| 4 retire the band synthesis | **void** — its subject turned out to be load-bearing; recommendation recorded, not acted on |
| 5 zoom | done — wheel bound, zoom-out bounded by the world |
| 6 HUD and camera clamp | done — clamp capped to the world; the HUD half was already working |

Deployed to `build/` and smoke-tested **in the deployed layout with no
environment variables**: boots, reaches the menu, no errors, 4/4 runtime DLLs.
Default window measured **55.91 fps with `cache_absent_itcm = 0`**.

**Needs a play session, and cannot be settled without one:** that a unit at the
edge of the view is still *clickable* at every zoom step. The rendering half is
verified; the input half is not.

Still open, unrelated to these phases: the ITCM two-byte corruption (docs/326 —
costs measurement, not play), a small black strip at the top edge of a very wide
frame, and three isolated revealed patches inside the native strip.

The goal: a native PC port of LEGO Battles that uses the components of
ndsrecomp without being strictly ndsrecomp on its own, and feels like a proper
PC RTS (C&C and the like): the window size seamlessly and glitchlessly shows
the game world at a distance that follows the window size.

This is not a new goal — the project already aims at scalable windows showing
**more of the game field** than the DS did, with **mousewheel zoom**. What is
new is the verdict on the *implementation*: the current one widens the DS
picture at the sides, and it shows.

## The finding that reframes the whole problem

**Nothing in this game is a hardware sprite.** docs/162 measured it:

```
engine A DISPCNT = 00012F08   ->  OBJ enabled = 0
engine A OAM     = every entry attr0=00C0, y=192   (parked off-screen)
engine B OAM     = the same
```

Units, buildings, effects and the in-field HUD are all **3D geometry** (docs/156
by layer-toggling, docs/184 by reading `nds_gpu3d_line`, docs/194 for the HUD).

So the battlefield is exactly three things:

| what | how it is drawn | DS limit? |
|---|---|---|
| terrain | BG1 tilemap | **yes** — 32 tiles, hardware wrap |
| fog / shroud | BG2 tilemap | **yes** — same |
| units, buildings, effects, in-field HUD | 3D geometry | **no** — frustum and viewport only |

The "32 columns and it wraps" constraint that shaped `lb_widemap` applies to
**two layers**, not to the game. And both of those layers have a **map-space
source already located**:

* terrain — the level grid, `u16` per cell, 64 wide, plus a metatile table
  (docs/168, docs/172). `lb_field.cpp` already renders the entire 1536x1008 map
  from it and was verified pixel-wise against an independent Python
  implementation: 94.30% identical, with 97.8% of the difference inside animated
  water.
* fog — the map-space visibility bitmask, `row[y][x>>3] & (1<<(x&7))`, reached
  from `0x021569E4` +0x10 -> row table at +0x14 (docs/247). **100.0%
  agreement, on by default.**

**Everything needed to draw the world natively is already located and proven.**
It is simply not wired to the display path.

## Where the current design spends its effort

`lb_widemap.cpp` is **6,930 lines**. Its job is to synthesise BG tile entries
for the columns outside the guest's 32, so the emulated 2D compositor can draw
them. Every visual bug reported on 2026-09-19 lives in that mechanism
or in its interaction with the 3D layer:

* shroud re-appearing in explored ground while panning — band fog reconstruction
* terrain flicker — the band fill's camera model and its `choose_camera` vote
* Eagle Command sprites truncating — see the next section; this one is not
  `lb_widemap` at all

`lb_field.cpp` is **1,581 lines** and draws the whole map correctly. Today it
runs only on `LB_MAPDUMP` frames — a diagnostic, not the display path.

## RETRACTED: the horizontal frustum is already widened

This page first claimed that the port widens the vertical 3D frustum but not
the horizontal one, that horizontal coverage relies on a finite ~114 px
over-wide reserve, and that units therefore clip at wide windows. **All three
are wrong**, and the correction matters because a fix was about to be written
against them.

`GPU3D::SubmitVertex()` already scales X in clip space:

```c
if (RenderWidth > 256 && Viewport[0] == 0 && Viewport[4] == 256)
    vertextrans->Position[0] = (Position[0] * 256) / RenderWidth;
```

That is exactly `k = 256/RenderWidth` -- the same "same pixels-per-world, more
of it" trade the Y axis gets from the clip matrix, applied per vertex instead.
`SubmitVertex` runs before `SubmitPolygon` and therefore before
`ClipPolygon`, so the clipper sees the scaled coordinates and the frustum is
genuinely wider. Both axes are handled.

Two further candidates for the "Eagle Command sprites flicker/truncate" report
were also ruled out by measurement rather than argument:

* **the DS geometry limit** -- `[lb_polypeak] peak 74/2048 polygons,
  296/6144 vertices`. Nowhere near it.
* **the game's own view rectangle** -- `lb_setting_view_width()` reads
  `LB_VIEW_WIDTH` and defaults to 0, so `apply_view_width` never runs in a
  normal session and the game's view stays 32x24 tiles. `[lb_fov]` appears in
  13 runs on disk, every one of them named `exp-vw*` -- explicit experiments.
  This also means docs/251's "sprite width invariant across surfaces" was
  measured WITH the view patched and does not describe the shipped config.

**So the truncation cause is still unknown.** What is now excluded: the
horizontal frustum, the polygon/vertex caps, and the guest view rectangle.

One thing worth carrying forward from the same logs, unexplained: the HUD
classifier reports `rejected: z=9 ... of 10 polys` with `cumulative z=61583`.
Nine polygons in ten rejected on depth is a large number to have gone unnoticed
and may or may not be related.

**A note on how this went wrong.** The plan was written from the docs, and
docs/251 is accurate about an experiment whose configuration is not the default
one. Reading the code that actually ships would have caught it in one step. The
lesson is the project's own: the measurement, not the write-up of the
measurement.

## A latent bug found on the way

`apply_view_width()` and `apply_view_height()` can each patch **once**:

```c
if (cur == want) return;                 // already patched
if (cur != kViewDimStock) return;        // not loaded, or not what we think
```

After the first patch `cur` is neither `want` nor stock, so a second call with a
different width returns without writing. The sibling `apply_cull_width()` gets
this right -- it accepts stock **or** a previously-patched encoding
(`(cur & 0xFFFFF000u) == 0xE2822000u`). Since these are env-gated and off by
default, nobody has hit it; anything that later drives view width from the
window size will.

## The target architecture

Draw the field from the game's own data, at the host's resolution, and stop
asking the DS 2D compositor for it:

```
  terrain   lb_field_render     from the level grid + metatile table + VRAM tiles
  fog       (new) from the map-space visibility bitmask, docs/247
  world     nds_gpu3d wide render, clip matrix scaled on BOTH axes
  HUD       existing overlay / lb_hud
```

The guest keeps running its own simulation, its own fog updates, its own
culling, its own 3D submission. Nothing about the game logic changes. What
changes is that the *picture* of the battlefield is composed from map-space data
at the host's chosen scale, instead of from a 256-px-wide hardware layer plus
synthesised edges.

**Zoom falls out of this, rather than being added to it.** The clip-matrix
scalar `k` is currently tied to `RenderHeight/192`. Make it a free parameter and
`k` *is* the zoom: `k = native_extent / desired_world_extent`. Terrain and fog
are host-rendered, so they scale by resampling. That is the C&C behaviour the
goal describes, and it is one number shared by three subsystems.

## What has to be true for this to work, and what is not yet established

**Established** (each with a doc and a measurement):

* the level grid, metatile table, phase and camera are locatable at runtime and
  re-derived per level rather than hardcoded (docs/168, 172)
* the map-space visibility bitmask is located and agrees 100% (docs/247)
* the 3D layer carries every unit, building and effect, with alpha strictly
  0 or 31 — an exact stencil (docs/184)
* the clip matrix can be scaled to trade pixels for world coverage, measured on
  the Y axis (docs/251)
* `SetRenderWidth` already accepts up to 1728 (raised from 448)

**NOT established, and each is a real risk:**

1. ~~**Cost.**~~ **MEASURED AND FINE** -- 1.256 ms mean for the whole map per
   frame. See Phase 1. This was called "the single biggest unknown" and it
   turned out to be a non-issue; no caching is needed.
2. **Terrain animation.** `lb_field` reads tiles from VRAM, which the guest
   animates, so animation should follow for free. The 94.30% comparison is
   consistent with that and does not prove it.
3. **The guest's own culling.** The game culls by a viewport bitmask sized from
   `view+0x16C/+0x170` in tiles (docs/163), and `apply_cull_width` widens it.
   Under zoom this must track the *world* extent, not the window. If it does
   not, units outside the guest's idea of the viewport are never submitted and
   no renderer can draw them.
4. **Input.** Screen->world must be computed host-side under zoom and fed to the
   guest, which already has a repointed touch path but assumes a fixed mapping.
   The command-cursor bug reported on 2026-09-19 (sprite drawn one band width
   off) suggests at least one coordinate space is already inconsistent.
5. **The minimap and in-field HUD** are anchored to the old geometry.


## The frame-rate baseline, and the TWO confounds either side of it

Clean runs, each read beside `cache_absent_itcm = 0` (see docs/326):
**51–52 fps** — 51.16, 52.24, 51.62 across three runs of two builds.

A later build read 36.19 and 37.19 on the same route. That looked bimodal
rather than noisy and so read as a regression, but it was **competing CPU load
from other processes on the same machine**.

So an fps number here means nothing without **both** guards beside it:

* `cache_absent_itcm == 0` — or the interpreter fallback is being measured;
* the machine's process load — or other work on the machine is being measured.

## A confound that invalidates frame-rate readings

Every fps number in this project has to be read beside `cache_absent_itcm`.

docs/322: about one run in four falls into interpreting ARM9 ITCM and drops to
17-19 fps, at random, for reasons unrelated to whatever is being tested. It
caught this plan directly. Measuring host terrain with it on by default read
**19.12 fps**, which looks like a catastrophic regression, and the counter says
otherwise:

    htJ  19.12 fps   cache_absent_itcm = 523,530,086   pc 0x01FF80E8
    htG  49.0  fps   cache_absent_itcm = 0

Same build, same route. The 19.12 is the ITCM bug, not the renderer.

**So an fps reading without that counter beside it means nothing**, and a
before/after pair where either side is unchecked is worse than no measurement
-- it is a confident wrong answer. Two earlier numbers in this document (49.0
and 50.85) happen to come from clean runs; that was luck, not method.

This also promotes the ITCM bug above the rest of the renderer work. While it
is live, no performance claim about this port can be made in one run, and the
phases below all end in performance claims.

## Phases

Each phase ends in something observable, and none of them requires the next one
to have started.

**Phase 0 — RETIRED.** It was based on a wrong reading; see the retraction
above. The truncation bug it aimed at is still open and now has three
candidates excluded.

**Phase 1 — DONE. Host terrain is cheap.** `LB_FIELDBENCH=1` times
`lb_field_render` per frame (`lb_field_bench_tick`, lb_overlay.cpp):

    [lb_fieldbench] map 1536x1024  mean 1.256 ms  worst 2.579 ms
                    over 9120 frames   (60fps budget 16.7 ms/frame TOTAL)

That is the **entire** map, every frame, for **7.5% of a 60 fps budget** -- and
the visible region is a fraction of it. Worst case is stable at 2.579 ms across
9,120 frames.

**The visible-rect + dirty-cache design is therefore unnecessary**, which is
the whole reason this phase existed. Animated water, the thing that would have
made a per-level cache wrong, costs nothing to get right when the render is
per-frame anyway.

**Phase 2 — DONE. The bands are drawn from the level grid.**

`lb_hostterrain_prepare()` renders the map once per frame (hoisted beside
`lb_hud_scan`, because the row body is fanned out across workers) and
`lb_hostterrain_pixel()` supplies the compositor's BASE pixel. With it active
`lb_widemap_extends(1)` declines, so the band columns have **no BG1 source at
all** and can only be showing the host map.

Evidence: `screenshots/phase2/phase2-host-terrain-512x192.png` — forest and
rock continuous through the left band, grass into sand through the right, no
black band and no shroud wall at the native boundary.

Cost, measured: **49.0 fps** against a 57 baseline, and that still includes a
per-pixel `extern "C"` sampler call (98,304 a frame) and a whole-map render
rather than the visible rect. Both are easy wins when cost matters.

Two things this phase established that were not its goal:

* **`lb_field` agrees with the game.** With host terrain supplying the base and
  BG1 still drawing natively, `native 138/256 kept` — over half the guest's own
  columns came out the identical colour. The level grid really does reproduce
  the cartridge's terrain.
* **A colour-match counter cannot grade a composite.** `band 80/256 kept` was
  equally consistent with "correctly fogged" and "wrongly overdrawn", because
  units, HUD and other layers legitimately sit on top. Six attempts to answer
  it by counting failed; one frame answered it. A one-shot dump from inside the
  renderer is the tool for "does it look right", and it costs one write.

**Phase 3 — IMPLEMENTED, partly verified. Fog from the bitmask.**

It was not just a refactor. **Phase 2 shipped the host field fully revealed**:
`lb_hostterrain_prepare` called `lb_field_render` and never applied the shroud.

Careful about how far that goes, because the obvious next sentence is wrong.
Phase 2 declined only BG1, so **BG2's band reconstruction was still drawing fog
over that unfogged base**. The base being revealed was therefore latent rather
than visible, and I have not established what the bands actually showed before
this change. What is certain is that the host field — the thing phases 4–6 make
the single source of the battlefield — carried no shroud at all, and every
consumer added later would have inherited a fully revealed map.

What changed:

* `src/lb_fog.{h,cpp}` — a new TU owning the docs/247 chain
  (`0x021569E4` → `+0x10` → row table at `+0x14`, `row[y][x>>3] & (1<<(x&7))`),
  validated per battle against the map's own dimensions and the stride the
  setter computes. It is a **separate TU on purpose**: the same chain exists
  inside lb_widemap.cpp, and keeping a copy here means phase 4 stays a delete
  rather than an untangling.
* `lb_field.cpp` — the shroud now comes from that record.
  `refresh_from_record()` is preferred and the **minimap readback becomes the
  fallback**. `LB_SHROUDSRC=minimap` forces the old path.
* `lb_overlay.cpp` — the host field gets the shroud applied after render.
* `lb_widemap.cpp` — `lb_widemap_extends(2)` declines when the host field
  carries a record-sourced shroud, retiring the band fog reconstruction. Gated
  on `lb_hostterrain_fogged()`, not `_active`: dropping the reconstruction on a
  frame where the record did not validate would leave the bands **unfogged**.

**Verified.** The record validates and is in use, and the count corroborates
docs/247 independently:

    [lb_shroud] record 64x64: 3619 of 4032 cells hidden

4032 − 3619 = **413 revealed**, which is exactly the "413 of 4096 bits set at a
freeplay start" docs/247 measured separately. The hidden count falls as the
player explores (3619 → 3598), and the locate correctly fails at teardown, where
the fallback takes over.

**Cost: none measurable.** 51.62 fps with `cache_absent_itcm = 0`, against
51.16 / 52.24 for clean runs of the previous build.

### The decline was not enough, and a photograph is what found it

Phase 3 made `lb_widemap_extends(1)` return 0 when host terrain is active, on
the reasoning that "declining BG1's extension leaves the band columns with no
BG1 source, so they fall through to the base". **That reasoning was wrong**, and
counters could not show it — only a wide-window frame could.

The extend condition is an **OR**:

    (i == wide_bg_slot || lb_widemap_extends(hud_bgs[i])) ? extra : 0

BG1 *is* `wide_bg_slot` — it is the furthest-back layer, which is exactly why
upstream picks it. So upstream's own rule kept extending BG1 regardless of the
decline, and the band columns got BG1 content painted **over** the host field.

Evidence, three independent measurements agreeing:

* the host field itself, dumped after the shroud went on (`LB_FIELDDUMP=1`):
  **88.4% of it is the shroud colour**, matching the record's 3619/4032 = 89.8%
  hidden — so the shroud was applied correctly all along;
* `band 151/1276 kept` — only **12%** of band pixels came from the host;
* the frame: revealed terrain across the full 1536 px with a **hard-edged fog
  rectangle exactly 256 px wide**, which is docs/247's "the fog view is square
  around the original aspect" reintroduced by the base-pixel path rather than
  by the fog search.

**Fix:** `lb_widemap_hostowns(bg)` overrides *both* halves of the OR, because
the host owning a layer is a stronger statement than either rule for extending
it. BG1 only — this repo's comments disagree about whether the shroud is BG2
(lb_hud.cpp) or BG3 (the 0t stanza), and extending a fog layer over an
already-fogged base can only add fog, never reveal ground.

**Result: `band 1276/1276 kept`** — 100%, from 12%. The frame now shows a
ragged revealed blob around the player's base with dark unexplored ground
beyond it, which is what correct fog-of-war looks like.

### RESOLVED: the two fogs now match

Decided by sampling one wide frame rather than by argument:

| region | fog |
|---|---|
| band (host) | `(24,20,32)` — one near-black, flat |
| native (guest) | `(154,170,178) (138,146,154) (178,195,203) (146,162,178)` — a light blue-grey **texture** |

A flat fill cannot be made to match a four-tone texture by choosing a better
constant, so replicating the cloud host-side was the wrong direction. **The
guest draws its own fog over the widened columns instead** — `lb_widemap_extends(2)`
is kept, reverting phase 3's retirement of it. The art is then authentic
everywhere by construction and no cloud tile has to be reproduced.

The host field stays fogged underneath as a **safety net**: a column the
extension ever fails to reach falls through to a base that is already hidden,
rather than revealing unexplored ground.

Measured after: band fog reads `(154,170,178) (138,146,154) (178,195,203)` —
the same tones as native. The 1536 px frame shows one continuous cloud with no
seam at the band boundary.

**And the shroud colour was wrong for this surface.** It was `0xFF1A1420`,
chosen to match the *minimap's* unexplored fill `(40,32,48)`. The field does not
draw unexplored that way. Where the host shroud and the guest cloud disagreed by
a cell at the boundary, the near-black showed as **black notches** along the
edge of the explored region. Set to the cloud's own base tone `0xFF9AAAB2`, the
same disagreement is invisible. The minimap is a different surface with a
different palette and was never the thing to match here.

### Still open after that

* **RETRACTED: "the HUD is stranded at wide windows."** It is not. The main HUD
  is on the **top screen** and renders correctly — Eagle Command, the unit
  portrait at 1478/1500, the Builds panel and the resource bar (505 studs, 1/4
  population). What I took for a broken HUD were the bottom screen's corner
  touch buttons, which the anchoring system is deliberately moving to the
  window corners; its own log confirms the moves (`-640,-191`, `+648,+176`).
  Phase 6's HUD half was already working. I graded a HUD by looking at the
  wrong screen.
* A small black strip at the very top of the wide frame, at the map's top edge.
* Three isolated revealed patches inside the native strip where the record says
  hidden. Small, and not yet explained.

**Frame rate is fine.** The default window measured **55.91 fps with
`cache_absent_itcm = 0`** after all of the above — above the 51–52 baseline,
which also retires the suspicion that phase 3/6 cost frame rate. The 36 fps
readings were the competing CPU load described earlier.

**RETRACTED — "the grey stipple in those frames is water, not fog."** It is
fog. The wide-window frames show blue water distinctly beside grey stipple, so
the two are not the same thing, and the stipple is the guest's cloud. The
reasoning that produced the wrong answer was sound in shape — the camera *was*
inside the explored region at 512 px — but it was used to identify a colour,
which is exactly what phase 2 recorded as the thing a colour cannot settle. The
wide window settled it in one frame.

**Why this is also the fix for the panning shroud, by mechanism:** the
minimap readback classified any pixel that was not the dark unexplored fill as
explored — including the minimap's **camera box outline, (251,251,251)**. Cells
under the moving box flipped to revealed while the box was over them and
reverted when it moved on. That is the shroud re-appearing glitchily as the
camera pans, even in explored areas: the readback was sampling the game's own camera
indicator as if it were terrain. Needs a play session to confirm.

**Phase 4 — CORRECTED SCOPE, and downgraded in priority.**

This phase was written as "delete rather than disable: 6,930 lines whose purpose
has gone". **That was wrong, and measuring the export surface is what showed
it.** lb_widemap.cpp is 6,959 lines and exports 50 symbols; 48 of them have a
consumer outside the file. It is not one subsystem, it is four sharing a TU:

| group | examples | fate |
|---|---|---|
| band **geometry** | `lb_band_left/right/top/span/latch/render` | **keep** — the 3D path, HUD and input all use it |
| **footprint/placement** | `lb_fp_*` (14 symbols) | **keep** — this is the building-placement fix |
| **map queries** | `lb_widemap_map_size/metatile/cam_tile` | **keep** — lb_field.cpp depends on them |
| band **tile synthesis** | `lb_widemap_fill/tile/fog/shroud/extends/scan` | the actual retirement target |

Only the last group is what phases 2 and 3 made redundant, and phase 3 already
**disabled** it: `lb_widemap_extends` now declines BG1 and BG2 whenever the host
field is ready and carries a record-sourced shroud, so the synthesis produces
nothing for the bands.

So what remains is deleting code that is already unreachable. That is
maintenance — **it changes nothing the player sees**, and it is the riskiest
kind of edit to make immediately before a play session, because a mistaken
deletion in a 6,959-line file with 48 external consumers breaks things phases 5
and 6 depend on.

**Therefore phase 4 is resequenced to run after 5 and 6**, and its first step is
a reachability check rather than a deletion: prove the synthesis entry points are
never called in a route with host terrain on, then remove them. The goal is a
game that plays like a PC RTS; phases 5 and 6 deliver that, and this one does
not.

### VOID — the synthesis it proposed to delete is load-bearing

Resolving the two-fog problem settled this phase by removing its subject. The
two synthesis entry points serve different layers:

    if (engine == 0 && bg == 1 && lb_widemap_tile(...))   /* terrain */
    if (engine == 0 && bg == 2 && lb_widemap_fog(...))    /* fog     */

`lb_widemap_fog` is what supplies fog tile entries for the widened columns, and
that is precisely the mechanism that makes the cloud texture match across the
whole view. **It is now load-bearing for the fix.** lb_widemap.cpp cannot be
deleted, and "6,930 lines whose purpose has gone" was wrong twice over — first
about the export surface, now about the purpose.

What is genuinely superseded is the **BG1 terrain** half: host terrain draws
those columns and `lb_widemap_hostowns(1)` stops BG1 being extended at all.

**Recommendation: leave it.** Removing the BG1 half changes nothing the player
sees, cannot be verified by playing, and is an edit inside a 6,959-line file
whose fog half the wide view now depends on. That is a poor trade immediately
after a deployed, working build. It is dead code, not a defect — so it is
recorded here rather than silently dropped.

**Phase 5 — zoom. SMALLER THAN WRITTEN, and it must follow phase 6.**

Reading the shipped code rather than the docs again changes this phase twice.

**The clip scalar is already a free parameter.** frontend.cpp derives the guest
surface from the setting, every frame:

    float lb_pscale = lb_setting_pixel_scale();      // g_set.pixel_scale, 1..16
    int lb_want  = (int)(win_w / lb_scale + 0.5f);   // clamped 256..1728
    int lb_wanth = (int)(win_h / lb_scale + 0.5f);   // clamped 192..768
    nds_gpu3d_set_output_width(lb_want);

`RenderWidth` *is* the zoom control: a lower pixel scale renders more world into
the same window. The 3D frustum follows automatically, because both axes derive
from it — `SubmitVertex` scales X by `256/RenderWidth` and the clip matrix
scales Y by `192/RenderHeight`. So "make the clip scalar a free parameter" is
**already done**; what is missing is only a control bound to it.

**The host terrain/fog resample is unnecessary.** `lb_field_render` draws the
map in DS-density pixels (24x16 px metatiles) and the guest surface is in those
same units at every pixel scale — the scale changes how many of them fit the
window, not their density. So `lb_hostterrain_row` stays 1:1 at any zoom and
there is nothing to resample. This is the same shape as phase 1, where the
measurement removed the cache the phase existed to build.

**But phase 6 is now a prerequisite, not a sibling.** `lb_want` is capped at
1728 and the world is 1536 px wide, so zooming out far enough on a wide window
makes the view **wider than the world** — precisely the case where the guest's
`max = (tiles+1)*cell - view` goes negative and the camera clamp has no valid
interval. Binding a mousewheel to the pixel scale before that is fixed would
hand the player a control that drives the camera straight into the reported
glitch. **Phase 6 first, then this.**

**DONE.** The wheel is bound to `g_set.pixel_scale` in `lb_overlay_event`, the
same value the Magnification slider writes — so the two cannot disagree and it
persists with the rest of the settings. `SDL_MOUSEWHEEL` already reached that
handler and was being discarded; the panel still gets the wheel first, so
scrolling a settings list does not also zoom the battlefield.

Zooming out is bounded by the world rather than by a constant: the pixel-scale
floor beside `lb_world_extent()` stops it once the whole map is in shot.

**Not yet verified: that a unit at the edge of the view is still clickable at
every zoom step.** Injecting wheel events headlessly is not something the
harness does, so this needs a play session. The rendering half is verified;
the input half is not.

**Phase 6 — HUD and minimap** re-anchored to the new geometry, and the camera
clamp retuned to the world extent the new renderer shows.

### Where the clamp actually lives (read, not yet changed)

`apply_camera_view_size()` in lb_patches.cpp rewrites four ARM immediates — the
**view size** used by the game's own two clamps, `func_0203906C` and
`func_0203CED4`:

    max_x = (max_tile_x + 1) * 24 - view_w        immediate at 0x020390C4
    max_y = (max_tile_y + 1) * 16 - view_h        immediate at 0x020390D4

It is already self-healing and already recomputed from the live field rect, so
the simple reading of the camera-cap defect — "it is applied once at startup" —
is **wrong**. Two candidate causes survive, and they are distinguishable:

1. **The view grows wider than the world.** The map is 1536x1024 px. Once
   `gw > 1536 + 24`, `max_x` goes **negative** and `max_x < min_x`, so the
   clamp has no valid interval to settle in. That is the shape of both reported
   symptoms: the camera nudging around glitchily (two clamps settling on
   different sides of an empty interval) and the camera cap preventing further
   movement. `gw` is the guest surface width — window width ÷ scale — so this
   needs a large window at a low magnification, which is exactly the
   stretched-and-resized-window case. **The right fix is not a bigger immediate:**
   when the view is wider than the world there is no camera range, and the host
   renderer should show the whole world centred with panning disabled.
2. **All-four-or-none.** `apply_camera_view_size` returns without writing
   anything if any of the four values is unencodable as an ARM imm12. That is
   deliberate and correct — a half-applied pair makes the two clamps disagree —
   but its failure mode is silent, and it leaves the **previous** window's
   bounds in place, which reads exactly as the cap not updating on a resize.

**Decided by one log line, not by reasoning:** print `gw`, `gh`, the four
computed immediates and whether the write happened, on every resize. Candidate 2
predicts a resize with no `[lb_camview]` line; candidate 1 predicts a line whose
values are fine while the camera still misbehaves. That log is now in.

### Candidate 1 confirmed from the code, and fixed

The project had already written this down, inside `apply_camera_bounds()`:

> The view can be wider than the world — a 1706 px surface against a
> 1536 px map. Then no camera position keeps the view inside, and min would
> exceed max. The guest applies max first and min second, so **min would win and
> the camera would pin to the left edge with black on the right**.

That is the camera-cap symptom, stated exactly, and it corrects a guess made earlier on this
page: the two clamps do **not** oscillate, the outcome is deterministic. The
handling for it sits in `apply_camera_bounds`, which is **default OFF** because
it shrank the playable world, so nothing has been covering this case.

**Fix, in `apply_camera_view_size`:** cap the view size at the world extent
before encoding the immediates.

    max_x >= min_x  <=>  view_w <= (max_tile_x - min_tile_x + 1) * 24

At equality `max == min`, so the camera pins with the entire world in shot —
the right answer to "there is nothing to pan to". Worked through for the real
map: 64 tiles, `world_w = 1536`; a 1600 px view caps to 1536, the immediate
encodes to 1536, and `max_x = 63*24 + 24 - 1536 = 0 = min_x`.

**The world rect is READ, never written.** +0x150/+0x158 is world state —
the clamp, pathfinding, the minimap and the AI all read it — and writing it to
constrain the camera was a serious regression: it trimmed the world size
inward. A port may move the camera; it must not
rewrite simulation state to do it.

### Verified engaging — and the test needed a new tool to exist

The failing case could not be reached before. `tools/lb_exp.py --window-size`
drives a PowerShell resize *after* launch, and it **fails silently**: a run
asking for 1600x900 reported `ASKED 1600x900, GOT 1024x384` and the resize
printed nothing at all. Every frame-rate number on this page was measured at
1024x384, not at the size requested. lb_exp.py already carries a scar from
this — "A whole performance run was spent that way".

So `LB_WINDOW=WxH` now sets the window size at creation (apply_hooks 4z). With
it, the failing case reproduces on demand:

    [lb_window]  initial window 1700x600
    [lb_geom]    window=1700x600 guest=1700x600 scale=1.000
    [lb_camview] view 1700x600 -> using 1536x600; encodes to 1536x600;
                 world 1536x1024px  VIEW EXCEEDED WORLD -- capped, camera pins
    [lb_camview] clamp view size -> 1536x600; every camera path settles here

The world rect reads **1536x1024** — the known map size, so the read path is
confirmed against an independently known value — and a 1700 px view caps to
1536, giving `max_x = 63*24 + 24 - 1536 = 0 = min_x`: a valid interval where
there was an inverted one.

**What is NOT yet established:** that this changes observable camera behaviour.
Both before and after, the guest applies max then min, so the camera ends at
`min` either way; the fix makes the interval *valid* rather than inverted, and
whether the inverted case produced the reported glitching still needs an A/B at
a wide window with the cap toggled. The surface also stays 1700 px wide while
the world is 1536, so there is real estate beyond the world edge that nothing
has been asked to fill yet. Both are the next step, and neither is fixed by
claiming the log line is the whole answer.

**A hook that applies cleanly can still be dead.** The first LB_WINDOW hook
patched the `presentation.separate` branch. It built, ran, and printed nothing:
the shipped layout is not separate — the window is titled plain "LEGO Battles"
and carries both screens. Checking which window the run actually creates would
have caught it before a build.

## Why this is the right shape

The current design needs every subsystem to learn about the bands: terrain, fog,
sprites, HUD, touch, camera, minimap. Each has been a separate investigation and
several are still open. The proposed design needs **one** thing to be true —
that the field is drawn from map-space data at a host-chosen scale — and the
subsystems that currently need band-awareness stop needing it, because there is
no band. There is just a view.

It is also not a rewrite. The expensive parts — locating the grid, the metatile
table, the phase, the camera, the visibility bitmask, the 3D stencil, the wide
3D render — are done, measured, and documented. What is missing is wiring, a
cost measurement, and one symmetry fix in the clip matrix.

## Where the tools are

    Ghidra 12.1.3                        (with JDK 21; Ghidra needs 21+)
    Ghidra scripts                       LBSeed.java (seeds 9,315 entry points
                                         WITH ARM/Thumb mode), LBDecomp.java
    Ghidra project                       the analysed project, "LB"
    decompiled C                         one file per function
    DeSmuME 0.9.13                       the oracle

    $NDSRECOMP                           READ-ONLY. Runner changes go through
                                         port/runner/apply_hooks.py; recompiler
                                         changes through
                                         port/recomp/apply_finder_patch.py

    port/runner/src/lb_field.cpp         host-side whole-map renderer (1,581 ln)
    port/runner/src/lb_widemap.cpp       band synthesis (6,930 ln) — to retire
    port/runner/src/lb_overlay.cpp       composite, field panel, map dump
    tools/lb_field.py                    the Python reference for lb_field
    tools/lb_code.py                     disassembler + xref over the ROM image

Ghidra's C is a lossy rendering of the instructions, so read the ARM when the C
does not explain the behaviour. `tools/lb_code.py dis` is
usually faster for a single function; Ghidra earns its keep on control flow.
