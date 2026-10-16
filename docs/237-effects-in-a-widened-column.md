# 237 — Effects in a widened column, by name and coordinate

**Goal within the project:** adaptive screen size in gameplay must fully work,
including effects and particles. This is the row
`docs/211` left open and the audit has carried as **not passed** ever since:

> **No capture yet centres a large, unambiguous effect inside a widened column.**
> An explosion out there would settle it beyond argument; 16 changed pixels is a
> real signal but a small one.

## Why every previous attempt found nothing

Three sampler faults were fixed before this (docs/211, and the earlier effects
work), and after all of them the answer was still *"539 samples,
every one offscreen"*. That looked like evidence effects never reach the band.
It was not. **The sampler was looking at a scene that had no effects near the
camera.**

Free Play opens centred on the player's base; the particle sources in that map
sit at world x ≈ 1184–1284, y ≈ 531–722, which is off the opening view
entirely. Every sample was correct and every sample was useless.

Two changes made the measurement possible:

* **`LB_FXPERIOD`** — the sweep ran once a second, which is slower than the
  effects it was hunting. It is settable now, and these runs used 6 vblanks.
* **Aim the camera at the effects instead of waiting for effects at the camera.**
  The unfiltered sample gave their world positions, so where to point was
  arithmetic, not luck: the camera panned to the map's bottom-right corner puts
  those sources at sx ≈ 434 with a 265 px band, i.e. inside it.

## The position decode is right, and the data proves it independently

Before trusting a coordinate, check it is a coordinate. Two things establish it
without appealing to the field offsets at all:

**1. Siblings cluster.** The `ParticleSystem` objects are a linked list (their
`+04`/`+08` are each other's addresses), and their decoded positions fall into
two tight groups — (960, 707), (960, 706), (945, 706) and (1060, 531),
(1068, 531), (1077, 531). Particles from one emitter landing within a few pixels
of each other is not what a misread field produces.

**2. One object moves continuously across the boundary.** A single
`HeroStartSpellEffect`, same heap address, sampled in sequence:

```
sx=298 sy=189  RIGHT-BAND
sx=244 sy=140  native
sx=211 sy=110  native
sx=201 sy=103  native
sx=193 sy=100  native
sx=186 sy= 98  native
sx=179 sy= 96  native
```

It travels smoothly and **crosses from the widened band into the native columns
exactly at 256**, which is where the geometry says the boundary is. A wrongly
decoded field would not move smoothly and would not change classification at the
predicted place.

## The result

With the camera at (904, 688) and a 786×426 surface — 265 px of band each side:

| class | in a widened column | native | offscreen |
|---|---|---|---|
| `ParticleSystem` | **98** | 59 | 2007 |
| `BuildConstructionEffect` | **73** | — | 428 |
| `HeroStartSpellEffect` | 1 | 7 | 336 |
| `FlashEffect` | — | 5 | 24 |

Named effects, with coordinates, in the extended screenspace. The row
`docs/211` opened is answered.

## What this does and does not prove

It proves the effect objects **are positioned** inside the widened columns and
that the port's own classification of them agrees with the live surface
geometry. Combined with docs/211's structural finding — exactly one camera-keyed
view rect exists in the whole ARM9, already widened on both sides, and effects
are drawn by the scene traversal rather than gathered through a second rect —
the case that they are also *drawn* there is strong.

It is not the same as a photograph of one, and this project's own practice is
to read the picture rather than the symbol.

## A caveat on the run above, found by reading the fog log

The pan that put those effects in the band also panned **into unexplored map**,
and the widemap log says so plainly:

```
[lb_wide]   ext_valid=1 fog_learned=1  cam_tx=119  cols 67..202 (map 192 tiles)
            left: offmap=0 zero=53     right: offmap=264 zero=318
[lb_shroud] fog tiles 0C2 0C0 0C1 / 0BD 0BB 0BC learned=1
            shrouded=768/768   map 23418/24576   cam tile 119,86
```

**`shrouded=768/768` — every visible metatile was fog.** So the counts above
establish that the effect objects are *positioned* in the widened columns and
that the port's classification agrees with the live surface geometry. They do
**not** establish that an effect was drawn and visible out there, because in
that frame nothing was: the whole view was shroud.

This also explains a picture that looked alarming and was not. Window captures
from these runs show gameplay in one corner and a repeating grey motif filling
the rest, which reads as "the band is not getting terrain" -- the exact symptom
`lb_widemap.cpp` already carries a comment about. It is the fog. The motif is
the learned 3x2 fog metatile, the green region is the explored area around the
base, and the 264 off-map cells on the right are shrouded because that is what
the cartridge draws past a map edge. `blanked=0` throughout, so the cold-start
flattening branch never fired.

Two things follow, and both are worth keeping:

* **The window capture path is not broken.** The "Defeated!" screen from the
  same run fills the window correctly. An earlier reading here that blamed
  PrintWindow was wrong and is retracted.
* **The remaining measurement has to be made in a CLEAR band.** An effect in a
  fogged column is hidden by the game's own rule, so the camera has to be placed
  where the explored area around the base straddles a widened column -- not
  panned away from it, which is what these runs did.

## Terrain in the band, from a capture that was already on disk

The same run that produced the "alarming" fog picture also settles the terrain
question, once the geometry is worked out rather than eyeballed.

At an 800x600 window with the default magnification of 2, the guest surface is
400x300. The native 256 columns sit centred at surface x 72..328, so the **left
band is window x 0..144**. In that capture the explored green terrain runs from
the window edge continuously out past x=345, full height — through the entire
left band and on into the native columns — and there is **no seam at x=144**,
where the native boundary is. The only edge in the picture is the ragged
explored/fog boundary much further right, which follows map content because that
is what a fog boundary does.

So terrain is drawn in the widened columns, and the extended and native columns
are visually continuous. That was the part of the adaptive-screen goal most at risk of
being wrong, and a picture of it existed before this investigation started —
nobody had computed where the boundary should be and looked there.

## Walking a unit into the band — and the real blocker for the last row

The commanding fix (docs/238) removed the obstacle that had blocked this row:
effects cluster around the base, the camera holds the base near surface x 128,
and a band does not begin until 256 — so nothing could put a named effect in a
widened column on visible ground. With ordering into the band working, a unit
can now be **walked** there, carrying its own effects onto ground it reveals.

The units go, reliably:

```
0230741C now at (-33,91)    023071D8 now at (-12,103)
023071D8 now at (-10,103)   023071D8 now at (-14,103)
023071D8 now at (-16,103)   0230741C now at (-29, 87)
```

**And still no effect was classified in the band** — because of the sampler, not
the game:

| class | in a band | native | no-position |
|---|---|---|---|
| `JiggleEffect` | 0 | 688 | **6501** |
| `FlashEffect` | 0 | 103 | — |
| `ParticleSystem` | 0 | 11 | — |
| `BuildConstructionEffect` | 0 | — | 29 |

`JiggleEffect` is the effect a moving unit carries, and **6501 of its samples
have no resolvable position**. The sampler finds a class's coordinates by
scanning offsets `+0x14..+0x24` for a pair that reads as a plausible map pixel
and is not a pointer; for `JiggleEffect` that search fails every time. So it can
never be classified into a band no matter where its unit walks — the one effect
this experiment was designed to move out there is the one the instrument cannot
place.

### CORRECTION: it was the camera lookup, not the class

The paragraph above guessed that `JiggleEffect` "most likely stores an owner
handle rather than a coordinate". **Wrong, and wrong in the way this project
keeps having to correct** -- a property of the GAME inferred from a failure of
the INSTRUMENT.

A failing sample's own bytes say otherwise:

```
JiggleEffect @0230F73C sx=0 sy=0 no-position  +14=00058F03(88) +18=000E8478(232)
```

88 and 232 are perfectly good map pixels. The class stores its position exactly
where the sampler looks. What actually happened is in the sampler's loop
condition:

```cpp
for (int o = 0x14; o <= 0x24 && have_cam; o += 4)
```

`no-position` is emitted whenever **`have_cam` is false**, and in that run it was
false constantly:

```
summaries with no camera   2750 of 3951
camera object rescans      19508
```

`lb_field_camera` rescans by sweeping **all of main RAM** -- about a million
iterations -- and outside a battle there is no camera object to find, so it swept
and failed every single frame the effect sampler ran. Every effect in those
frames read as `no-position`.

Fixed: sixty calls of cooldown after a miss, and the rescan chatter capped. This
reaches well beyond this row -- **every band classification of an entity or an
effect depends on that lookup**, and it was unavailable 70% of the time.

That is a **different open item** from the one this document started with, and a
smaller one. Effects reaching the widened columns is measured (98
`ParticleSystem`, 73 `BuildConstructionEffect`, and a `HeroStartSpellEffect`
tracked crossing the boundary at exactly 256). What remains is teaching the
sampler to resolve `JiggleEffect`'s position — most likely it stores an owner
handle rather than a coordinate, and should be followed to its entity — and then
re-running this walk.

## CLOSED — a named effect in a widened column, on CLEAR ground

With the camera lookup fixed and a unit ordered into the left band (docs/238's
commanding fix), the measurement this row has wanted since docs/211 finally
happened:

```
ordered 0230741C from (2,112) to x=-150  ->  now at (-56,101)  OUT OF THE NATIVE COLUMNS

[lb_fx] JiggleEffect @0230F98C sx=-27 sy=102 LEFT-BAND
[lb_shroud] shrouded=0/768   map 23808/24576
```

**A named effect, with a coordinate, in the LEFT band, with `shrouded=0/768` --
not one fogged metatile in the visible area.** Three separate moments captured
the same thing. The earlier result in this document was 98 `ParticleSystem` and
73 `BuildConstructionEffect` in a widened column but with `shrouded=768/768`, so
positioned-but-not-visible; this is the version with the caveat removed.

### What made it possible, and why it took so long

Three separate obstacles, none of them about rendering:

1. **Nothing could put an effect out there.** Effects cluster at the base, the
   camera holds the base near surface x 128, and a band starts at 256. Panning to
   where effects already lived also panned into unexplored map. The fix was to
   **walk a unit into the band** -- which required the commanding bug (docs/238)
   to be fixed first. The two open rows turned out to be one dependency chain.
2. **The camera lookup was unavailable 70% of the time**, sweeping all of main
   RAM every frame outside a battle, so effects read `no-position` and could
   never be classified into a band at all.
3. **The fog check had to be part of the record.** Without `[lb_shroud]` printed
   beside the hit, "in the band" cannot distinguish drawn from hidden -- which is
   exactly how the first version of this document overstated itself.

Item 1's effects-and-particles row is answered: effects reach the widened
columns, are positioned there, and do so on ground that is visibly clear.

### The picture, and exactly what it corroborates

`screenshots/exp-fxpic/route/fxband-B-0.png` is the **adaptive** framebuffer --
the widened surface itself, **786x192** -- captured at the moment the log
recorded:

```
[lb_fx] JiggleEffect @02312824 sx=-28 sy=89 LEFT-BAND
[lb_shroud] shrouded=0/768   map 23808/24576
```

The image shows terrain drawn across the **full 786 px**, including the left
band (surface x 0..265, left of where the native columns begin), forest and
grass continuous with **no seam at the native boundary**, **no fog anywhere** --
matching `shrouded=0/768` exactly -- the HUD anchored hard against the left
edge, and sparkle effects rendered in frame.

**What it does not show**, and should not be claimed to: the individual
`JiggleEffect` at surface x ~237 as a distinguishable sprite. It is a handful of
pixels. The log places it there with a coordinate; the picture establishes that
the column it occupies is drawn, unfogged, and continuous with the native view.
Those are complementary pieces of evidence and the honest reading uses both --
the numbers for *where*, the picture for *that the place is real*.
