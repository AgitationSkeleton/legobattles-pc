# 278 — The vanilla view setup is a RECT, not a camera, and that is what to extend

After two ticks of the port correcting the camera after the fact, the approach
changed: compare the renderer setup to the vanilla game's from the decompiled
code, and extend its logic outward properly, so the camera and rendering feel
and look like the game even in motion, only extended.

This is the right framing and the measurements agree with it. What follows is
where the game's own view logic lives.

## Why post-correcting the camera cannot be the answer

`apply_camera_clamp()` (docs/277's commit) writes the camera at vblank so the
widened view stays inside the world. It works, and it is worth keeping — with it
the band is a constant 721 in gameplay, and without it the band marches in 16 px
steps as the camera pans:

    LB_CAMCLAMP=0, gameplay only:  left = 916, 932, 948, 964, 980, 996, ...
    LB_CAMCLAMP=1, gameplay only:  left = 721, constant

But it corrects a RESULT. A counter on the clamp itself:

    acted 1441/7068 frames   raw 656,252 -> 640,252 (dx -16)
                             raw 644,250 -> 640,252 (dx  -4)

Twenty per cent of all frames, and at a pinned edge it will be nearly every one:
the guest advances the camera, the port writes it back, and the picture is drawn
from whichever won that frame. That is a shake where there was previously a
bump, which is not an improvement.

## RETRACTED (docs/280): this is an entity draw routine, not the camera

Everything below from "Where the game keeps its view" down to "The direction
this implies" is WRONG and is kept only so the error is legible.

`func_0203ABC8` is reached from exactly one caller, `func_0203ABBC`, itself
reached from `func_0203A9E8` -- and `func_0203A9E8` is a **draw loop over
entities**: `func_020EF1D0` returns a count, `func_020EF200` returns element
*i*, `func_020872DC` resolves it to an object, and the result is dispatched by
type to one of three renderers (`0203ABBC`, `0203BED0`, `0203B3AC`). So
`func_0203ABC8` draws ONE ENTITY, and its `+0x54..+0x5E` writes are that
entity's own screen rect. The `+0x5E` match against docs/112's camera X was an
offset collision between two different objects.

The "two arms of a clamp" reading is wrong too, and independently. The second
arm at `0x0203B0AC` loads `0x51EB851F` and does `smull` + `asr #5` -- the signed
magic-number divide by 100 -- applied to `r5 * byte[sb+0x22E]`. The two arms are
"plain" versus "scaled by a percentage", selected by a flag, which is a zoom
variant of a rect setter and not a bound at all.

The search that produced this was sound in method (a `strh` with a `0x5E`
immediate must be ARM, so ten hits is exhaustive) and wrong in what it assumed
the hits referred to. Finding the real camera clamp needs a search anchored on
the camera OBJECT, not on the offset.

## Where the game keeps its view

The camera pair is at view object `+0x5E` / `+0x60` (docs/112). Searching the
ARM9 static image for stores to `+0x5E` finds ten — `strh` with a `0x5E`
immediate is necessarily ARM, because 0x5E exceeds the Thumb `strh` imm5 range
(max 0x3E), so the encoding is fixed and the search is exhaustive:

    02026EB8  strh r2,[r13,#0x5E]    stack, a local struct
    0202A5B4  strh r2,[r13,#0x5E]    stack
    0203817C  strh r1,[r4,#0x5E]
    0203866C  strh r1,[r4,#0x5E]
    0203905C  strh r2,[r0,#0x5E]
    02039954  strh r1,[r4,#0x5E]
    0203B094  strh r5,[sb,#0x5E]     <- both inside one function
    0203B12C  strh r4,[sb,#0x5E]     <-
    0203C730  strh r1,[r4,#0x5E]
    020A5F88  strh r1,[r4,#0x5E]

Two stores to the same field in one function is the shape of a two-armed clamp,
and both sit inside **`func_0203ABC8`, size 0x704** — unnamed in
`port/config/lb_arm9.toml`.

## What that function actually maintains

Reading around the two stores, it is not "the camera" but a whole view record,
written as a rect in two representations at once:

| offset | written | note |
|--------|---------|------|
| `+0x54` | `strh r6` | rect, integer |
| `+0x56` | `strh r7` / `strh r3` | rect |
| `+0x58` | `strh r2` / `strh r7` | rect |
| `+0x5A` | `strh r4` / `strh r2` | rect |
| `+0x5C` | `strh r0` | camera Y half of the pair |
| `+0x5E` | `strh r5` / `strh r4` | camera X |
| `+0x1C`, `+0x1E` | mirrors of `+0x5C`, `+0x5E` | a second copy |
| `+0x34` | `r6 << 12` | 20.12 fixed point of `+0x54` |
| `+0x38` | `r2 << 12` | ...of `+0x58` |
| `+0x3C` | `r3 << 12` | ...of `+0x56` |
| `+0x40` | `r2 << 12` | ...of `+0x5A` |

So the renderer's view is **four s16 bounds plus a fixed-point copy of each**,
and the camera is one field inside that record rather than the thing the
renderer consults directly. A port that moves only `+0x5E` is changing one term
of a set the game keeps internally consistent — which is exactly why the guest
puts it back.

## The direction this implies

Extend the RECT outward by the band and let the camera fall out of the game's own
arithmetic, instead of pinning the camera and fighting for it. If `+0x54..+0x5A`
and their fixed-point copies describe the region the renderer draws and culls
against, widening them is exactly extending its logic outward, and the clamp at `0203B094` / `0203B12C` becomes the game's own clamp
operating on a wider rect — no second authority, nothing to fight.

## NOT established

1. **Which of `+0x54..+0x5A` is which bound.** Four s16 could be
   left/top/right/bottom or x/y/w/h; the register provenance is not read yet.
2. **Which arm of the function is the clamp** and what it clamps against. The
   `+0x5E` store at `0203B094` is preceded by a branch to `0203B13C`, and the
   one at `0203B12C` sits after a `tst r4,#1` / `mvnne` / `moveq` pair, which is
   an odd/even correction rather than a bound — so "two arms of a clamp" is the
   shape, not yet the proof.
3. **Who calls `func_0203ABC8`**, and whether it runs per frame or only on view
   changes.
4. **Whether the other six `+0x5E` stores touch this same object.** Two are to
   the stack and are certainly not; the remaining four are unexamined.
5. **Whether widening the rect is safe for the simulation.** docs/85 found no
   Sim->User calls, so the view is presentation — but the rect may also feed
   entity culling (`docs/197`: the guest culls to its own view), and culling
   that changes what the AI sees would not be presentation any more.

Point 5 is the one that decides whether this direction is available at all, and
it should be settled before any of the rest.

## Point 5, settled — and it shrinks the reason to touch the rect at all

docs/197 already answers it, including a retraction of its own title: **"'The
guest culls to its own view' was too strong."** Measured there under completely
stock settings, geometry renders out to x = 403 — 52 columns past the native
right edge at 351 — reproducible across five pan steps and four A/B readings.
Its capture at 1280x720 is blunter still: the terrain stops dead at the native
edge and the units carry on past it, standing on black.

So:

* **Units and buildings already render in the widened columns.** docs/197:
  "Nothing needs to be done to the entity cull to see units out there. They are
  there now."
* **What was missing out there was TERRAIN**, and that is what `lb_widemap`'s
  tilemap fill already supplies.

Two consequences. The simulation worry is much smaller than it looked: the cull
governs what is submitted for DRAWING, and docs/197 shows that boundary already
sits well outside the native view without anyone having moved it. (It does not
prove the rect is absent from every simulation path, so "not established" still
covers that — but the specific fear, that widening the view would change what
the AI can see, has no support and some evidence against.)

And the reason to widen the rect is now narrower than when this document was
written. It buys nothing for visibility, because visibility already works. Its
only value is making the game's own clamp operate on the wider view, so the
camera stops where the widened picture ends and there is no second authority to
fight — which is exactly and only the defect apply_camera_clamp still has.

That makes the next step smaller and more specific than "widen the rect": find
what the guest's own camera clamp bounds are computed FROM. If they derive from
the map dimensions, the port can feed it different ones the way apply_caps feeds
the allowance table, and the guest clamps itself. If they are immediates, the
rect is the way in. Either way the target is one authority, not two.
