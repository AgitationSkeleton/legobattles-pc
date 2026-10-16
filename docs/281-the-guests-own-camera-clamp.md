# 281 — The guest's own camera clamp, and the two immediates that size it

The goal: instead of jittering at the edge or corner of the world, stop panning there as the vanilla game does, by extending the cartridge's own renderer setup outward.

docs/278 went looking for this and found an entity renderer instead (retracted
there, and in docs/280). This is the real thing, found by anchoring on the
camera OBJECT rather than on the `+0x5E` offset.

## The anchor

`lb_field.cpp` locates the camera object by finding the callback record holding
`kScrollCallback = 0x020396E0`. Two of the ten `strh ...,[rN,#0x5E]` stores sit
in that neighbourhood — `0x0203905C` and `0x02039954` — rather than in the
`0x0203B0xx` entity renderer. Those two are the real camera writers.

## The object

| offset | meaning | evidence |
|---|---|---|
| `+0x5C` | dirty byte | `strbne r2,[r0,#0x5c]` when the position changed |
| `+0x5E` | camera X (s16) | docs/112 |
| `+0x60` | camera Y (s16) | docs/112 |
| `+0x62` | **view width** | `ldrsh r3,[r4,#0x5e]` + `ldrsh r1,[r4,#0x62]` → right edge |
| `+0x64` | **view height** | `ldrsh ip,[r4,#0x60]` + `ldrsh r3,[r4,#0x64]` → bottom edge |
| `+0x150` | world bound MIN, as a **tile** point | consumed by `func_0201479C` |
| `+0x158` | world bound MAX, as a **tile** point | same |

`func_02039038` is `SetCameraPos(obj, pt)`: it compares `+0x5E`/`+0x60` against
the requested `pt->x` / `pt->y` (at `+4` / `+6`), sets the dirty byte if they
differ, and stores.

## The clamp: `func_0203906C`

`func_020398B8` computes the desired camera, calls `func_0203906C`, and stores
what comes back to `+0x5E`/`+0x60`. `func_0203906C` is the bound:

```
func_0201479C(sp+0x1C, obj+0x150)    ; MIN tile point -> fixed-point pixels
func_0201479C(sp+0x10, obj+0x158)    ; MAX tile point -> fixed-point pixels
func_0200E634(sp+0x0C, sp+0x14, 0x18)   ; *p += 24<<12      (one tile wide)
func_0200E634(sp+0x08, sp+0x18, 0x10)   ; *p += 16<<12      (one tile tall)
func_02039134(sp+0x04, sp+0x14, 0xF8)   ; *p -= 248<<12     <-- VIEW WIDTH
func_02039134(sp+0x00, sp+0x18, 0xC0)   ; *p -= 192<<12     <-- VIEW HEIGHT
```

The two helpers are three-instruction in-place arithmetic, so the constants land
in the bound itself:

```
func_0200E634(out, p, n):  *p += n<<12;  *out = *p
func_02039134(out, p, n):  *p -= n<<12;  *out = *p
```

and `func_0201479C` converts a tile point to pixels — `ldrb [r1,#4]`,
`smulbb` by `0x18`, `lsl #12`.

Then four conditional stores, which is the whole clamp:

```
0x020390E8  cmp r0, r1,asr#12 / strhgt r2,[r4,#4]    x > max_x -> x = max_x
0x020390F8  cmp r0, r1,asr#12 / strhgt r0,[r4,#6]    y > max_y -> y = max_y
0x0203910C  cmp r0, r1,asr#12 / strhlt r0,[r4,#4]    x < min_x -> x = min_x
0x02039120  cmp r0, r1,asr#12 / strhlt r0,[r4,#6]    y < min_y -> y = min_y
```

So, in one line each:

```
max_x = world_max_tile_x * 24 + 24 - 248
max_y = world_max_tile_y * 16 + 16 - 192
min_x = world_min_tile_x * 24
min_y = world_min_tile_y * 16
```

**248 and 192 are the view size**, written as immediates at `0x020390C4`
(`mov r2,#0xF8`) and `0x020390D4` (`mov r2,#0xC0`). 192 is the DS screen height
exactly; 248 is 256 − 8.

## Why this is the right lever

`apply_camera_clamp()` in `lb_patches.cpp` corrects the camera at vblank, after
the guest has already set it. docs/278 measured the cost: it acts on
**1441/7068 frames**, and at a pinned edge it would act on nearly all of them —
two authorities writing one field, which is a shake where there was previously
a bump.

The guest's clamp bounds the camera against *its own view size*. Feed it the
widened view size and it clamps the widened picture, by its own arithmetic, with
no second authority:

* **Right and bottom edges** — patch the immediates. `0xF8` → `248 + band_right`,
  `0xC0` → `192 + band_bottom`. A larger view size yields a smaller maximum,
  which is the correct direction: a wider view reaches the world's right edge at
  a smaller camera X.
* **Left and top edges** — the minimum is `world_min_tile * 24` with no
  immediate to patch, so it moves via the object's bound rect at `+0x150`. That
  is data, reachable the same way `apply_caps` reaches the allowance table.

The port already has the machinery for the code half: `kTouchPatches` in
`lb_patches.cpp` is a table of `{addr, old_word, new_word}` triples, and it is
already used to widen two recogniser allocations.

## NOT established, and to be measured before implementing

1. **That `+0x62`/`+0x64` are not also read by something that must stay native.**
   They are the view size as used by `func_020398B8` to compute the right and
   bottom edges; whether that result feeds anything besides the clamp is
   unread.
2. **That the minimum's tile granularity is acceptable.** `min_x` moves in
   24 px steps, so a band that is not a multiple of 24 cannot be expressed
   exactly through `+0x150`; it would have to round outward (showing a little
   black) or inward (clamping a little early).
3. ~~**Which of the two rects at `+0x150`/`+0x158` is min and which is max.**~~
   **SETTLED.** `func_0201479C` reads the tile x as a byte at `[r1,#4]` and the
   tile y as a byte at `[r1,#5]`, scales them `x * 24` and `y << 16` (which is
   `y * 16` in 20.12), and hands both to `func_020F27D4` as the out-point. The
   out-point therefore carries x at `+4` and y at `+8`, which makes the stack
   aliasing exact rather than inferred:

       func_0201479C(sp+0x10, obj+0x158) -> max x at sp+0x14, max y at sp+0x18
       func_0201479C(sp+0x1C, obj+0x150) -> min x at sp+0x20, min y at sp+0x24

   and those are precisely the four slots the `strhgt` / `strhlt` pairs read.
   So `+0x150` is the MINIMUM and `+0x158` the MAXIMUM, with the tile bytes at
   `+0x154`/`+0x155` and `+0x15C`/`+0x15D`.
4. **Whether the clamp runs on every camera move** or only through
   `func_020398B8`. `func_02039038` writes `+0x5E`/`+0x60` with no clamp at all,
   so at least one path sets the camera unbounded.

## Point 4, answered by the call graph

```
callers of func_0203906C (the clamp):        5
callers of func_02039038 (unclamped set):    8
in BOTH lists:  0x0203CA74, 0x020ABDA0
```

Two callers appear in both, which is the expected shape: compute a point, clamp
it, store it. So the game does not clamp inside the setter -- it clamps at the
call site, and six call sites set the camera with no clamp at all
(`0x02048E48`, `0x02048EA8`, `0x020490E8`, `0x020AB5F0`, `0x020AC9B0`,
`0x020DC298`).

That is the right division for this port. The unclamped callers are *jumps* --
minimap clicks, centre-on-unit, scripted moves -- which go to a chosen target
rather than drifting outward. The clamped path through `func_020398B8`, reached
from `func_0203942C`, is the scrolling one, and scrolling is exactly where the
camera could repeatedly bump against the black world edge.

So patching the two immediates should be sufficient for the reported defect, and
`apply_camera_clamp` can be retired for panning rather than left to fight the
guest. Whether a jump path can still land the widened view outside the world is
a separate, smaller question, and it is a jump rather than a shake if it does.

## Cleared: patching those immediates is safe

Patching guest code makes live bytes differ from lifted bytes, which the
validation gate in `lookup_in()` (`runner/src/runtime_arm.cpp`) checks. It does
NOT halt. A failing entry is recorded as `inactive_candidate` and skipped;
`lookup_static_cached` then ends `return hit ? &slot : nullptr;`, and a null
lookup is the Tier-3 path -- its own comment says "Copied RAM code has no static
entry and is handled by Tier 3."

So a patched function degrades to interpretation, which is exactly how the eight
existing `kTouchPatches` sites already work. The cost here is that all 0xC8
bytes of `func_0203906C` become interpreted, since validation covers the whole
function. It is called a few times per frame, so that is negligible.

One constraint on the patched value: ARM immediates are an 8-bit constant with
an even rotate. 248 and 192 fit directly. A widened width above 255 must be
encodable -- multiples of 4 up to 1020 are (`imm8 << 2`), so the widened view
size should be rounded to a multiple of 4. At the world edge that is under one
tile of slack.

## Implemented as data, not as a code patch

The clearance above says patching the two immediates is safe. It is, but moving
the BOUND TILES is better, and that is what `apply_camera_bounds()` in
`lb_patches.cpp` does:

* **Both edges.** The immediates only reach the maximum; the minimum is
  `min_tile * 24` with no constant to patch. Moving the tiles moves both.
* **No Tier-3 cost.** Nothing in the code image changes, so `func_0203906C`
  stays lifted.
* **Still one writer of `+0x5E`.** The guest's clamp holds the camera; the port
  only says where the world ends.

`apply_camera_clamp()` is superseded and now defaults OFF (`LB_CAMCLAMP=1`
re-enables it, so the two can be A/B'd from one binary).

Three details the implementation has to get right, recorded because each is a
way it could have been silently wrong:

1. **Rounding outward.** The bound is tile-granular, so the adjustment rounds
   UP -- the camera stops up to one tile early rather than showing black past
   the world edge, which is the defect being fixed.
2. **The wider-than-world axis.** A 1706 px surface against a 1536 px
   map has no camera position that keeps the view inside. The guest applies the
   maximum first and the minimum second, so a min above the max would win and
   pin the view to the left edge with black on the right. Both bounds collapse
   onto one centre tile instead, which fixes the camera mid-map.
3. **Not ratcheting.** Re-deriving the adjustment from an already-adjusted
   value would walk the bounds inward a tile per frame. The stock four bytes are
   remembered and re-read only when the object holds something the port did not
   write -- so a guest rewrite re-baselines, and a guest that restores stock
   every frame still cannot ratchet.

## Measured at the reference geometry: one write, then held

The run that matters, `demos/user-combat-2026-09-14.lbdemo` on the reference
settings (1706x696 client, pixel_scale 2, so an 852x348 guest
surface):

```
[lb_cambound] obj 0224B448  stock x 0..95 y 0..95  -> x 13..82 y 5..90
              d 13,5  half 298,78  surface 852x348  wrote 1/1 frames
[lb_cambound] holding: 1 writes in 3600 frames with a camera
[lb_cambound] holding: 1 writes in 7200 frames with a camera
[lb_cambound] holding: 1 writes in 10800 frames with a camera
[lb_cambound] holding: 1 writes in 14400 frames with a camera
```

**One write in 14,400 frames.** Set the bounds once, then hold. The contrast
with what this replaces is the whole point:

| | acted / frames |
|---|---|
| `apply_camera_clamp` (post-correction) | 1441 / 7068 -- about one frame in five |
| `apply_camera_bounds` (this) | 1 / 14400 |

A clamp that writes the camera every fifth frame is a shake by construction, and
at a pinned edge it would approach every frame. Writing the bound once is not a
smaller fight; it is no fight, because after that single write the guest is the
only thing moving the camera.

The arithmetic at that surface, checkable by hand: `half_x = (852-256)/2 = 298`,
`dx = ceil(298/24) = 13`, so `x 0..95` becomes `13..82`; `half_y = (348-192)/2 =
78`, `dy = ceil(78/16) = 5`, so `y 0..95` becomes `5..90`. A 96x96 tile map is
2304x1536 px, comfortably wider than the 852x348 view, so the centred
wider-than-world case is not exercised here and remains untested.

Writes 2 through 4 land at frames 14494, 14665 and 14837 with surfaces of
306x216, 386x276 and 498x360 -- the harness resizing the window as the run ends.
One recompute per surface change is exactly right, and it is also the first
evidence that a resize re-derives from stock rather than ratcheting.

Run totals: 267 s, **54.7 fps**, **0 dispatch misses**.

### What this does NOT establish

* **60 fps.** 54.7 is short of the 60 fps target, and this change was never
  aimed at frame time.
* **The centred axis.** No surface tested so far has a view wider than the world
  on either axis, so that branch has not run.
* **That panning now feels right.** One write per level is the mechanism working;
  whether the edges feel like the cartridge has to be judged by playing a real
  build.
