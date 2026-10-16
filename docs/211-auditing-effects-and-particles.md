# 211 — Auditing effects and particles in the widened columns

Goal: the last unaudited part of adaptive screen size, "effects and particles". docs/210
settled units by naming them out of the entity pool. Effects are not in that
pool, so this needed a different handle.

## Where effects live

RTTI answers it in two lines:

    Battles::Effect         -> Battles::DListItem
    Battles::ParticleSystem -> Battles::DListItem

They are linked-list items, not pool entries — there is no array to index and no
count to read. But every one carries a vtable pointer in its first word, and
`out/eur/rtti.json` already knows all 41 of those addresses (40 `Effect`
subclasses plus `ParticleSystem`). So main RAM can be scanned for them directly.

* `tools/lb_gen_fxvtables.py` generates `port/runner/src/lb_fxvtables.h` from the
  RTTI extract. Generated, not hand-written, because the addresses are ROM data.
* `LB_FXLOG=1` scans main RAM once a second and reports every live effect object
  with a window of its fields.

The `+04`/`+08` fields of consecutive `ParticleSystem` objects point at each
other and back — `022469A8 -> 02246A18 -> 02246A88`, each one's `+08` naming its
predecessor. That is the `DListItem` next/prev, which confirms both the class
identification and the list structure.

### The scan had to skip the code image

The first version scanned all of main RAM and reported 24 "live effects". Every
one of them was a false positive: they sat below `0x02012000` and the words
following disassembled as ARM — `E92D4010` is `push {r4, lr}`. Those were vtable
addresses in literal pools and in constructors that *store* the vtable, not
objects that *have* one. The ARM9 static image is `0x150400` bytes, so the scan
now starts above it.

## An idle field has no effects to audit

A story mission at its start is quiet. 90 seconds of lingering in Knights Act 3
found exactly **one** live effect the whole time, an `ArrowHighLightEffect` that
never changed. Nothing to measure.

Free Play has an AI opponent that builds and fights. 150 seconds there:

    716  ParticleSystem
    116  BuildConstructionEffect
     38  JiggleEffect
     27  HeroStartSpellEffect
     18  UpgradeEffect
     16  FlashEffect
      8  HealthBoostTrailStarEffect
      3  FireBallEffect

...and the simultaneous count hit the scanner's own cap of 16. The scanner works,
and **Free Play, not story mode, is where effects can be audited at all.**

## What the audit establishes

**1. ~~There is no separate cull for effects.~~ RETRACTED 2026-09-23
(docs/350).** There are two, and both are 256-px limits. `func_02016070` (one
anchor point; the draw of eighteen Effect classes) and `func_02016110` (a
rect's corners; construction, spell-icon and hero-spell effects and both
projectile list draws) subtract the camera and then compare the SCREEN
position with literal `#0`, `#0x100` and `#0xC0`, returning before anything is
submitted when it fails. The scan below looked for a camera read paired with a
view-width ADD, which is the gather rect's shape; these have no add, so the
scan could not see them -- and `0x02016098`, the camera read inside
`func_02016070`, is one of its 30 sites. The original text, for the record:

> Scanning the whole ARM9 for the
camera-x read (`ldrsh rD, [rN, #0x5E]`) finds 30 sites, and exactly **one** of
them pairs with a view-width add: `0x02038FC4`, inside `func_02038F28` — the
entity gather rect docs/208 and docs/210 already widened on both sides. Effects
are drawn by the scene traversal and are not gathered through a second rect, so
there is no second 256-px limit to fix.

**2. Dynamic content does reach the widened columns.** Two captures ten seconds
apart with a static camera, differenced and counted per band:

    LEFT-EXT        0 changed of   73920  (0.00%)
    NATIVE       2040 changed of  195965  (1.04%)
    RIGHT-EXT      16 changed of   73150  (0.02%)

Terrain does not change between frames; only units and effects do. The right
widened column carries changed pixels, so it is not a static picture out there.
The left column's zero is **not** evidence of a defect in this scene — the
action was at surface x 242, and nothing was happening on the left to render.
docs/210 separately showed the King and his guard rendering in the LEFT column,
so both bands are known to work.

## What this does NOT show, and why it stopped here

**No capture yet centres a large, unambiguous effect inside a widened column.**
An explosion out there would settle it beyond argument; 16 changed pixels is a
real signal but a small one.

Getting that needs the camera aimed at an effect, the way `--aim-hero` aims at a
Hero — and that needs each effect class's position offset. Those offsets are
**per class**, not shared: `ParticleSystem` keeps plausible coordinates around
`+1C`/`+20`, while `FireBallEffect`'s `+1C` holds a pointer. An attempt to
identify the offset statistically was **confounded and is not reported as a
result**: most apparent "hits" were zero-valued fields, and zero minus the
camera x lands in the same range as a genuine on-screen coordinate.

So the honest status is: **structurally sound and consistent with the pixels,
not visually proven for a specific effect.** Finishing it means reverse
engineering `Battles::Effect`'s own layout — its position is most likely on the
common base rather than per subclass, which is the thing to check first.

## Note for future runs

`--linger N` sits in gameplay for N seconds capturing, because a run that
reaches a battle and immediately starts probing photographs an idle field.
Effects only exist while something is happening.
