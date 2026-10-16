# Checking the four requirements against the widened viewport

`docs/164` found the master control -- the root viewport descriptor at
`02151F28+8` -- and showed the game fills a wider area when it is raised. This
checks that against what was actually asked for.

## The minimap camera box does not track the viewport

Measured directly, isolating the green minimap disc first so HUD whites do not
contaminate the sample:

```
root w=32   disc x[232..266] y[40..74]   camera box x[241..256] y[53..70]  16x18
root w=56   disc x[232..266] y[40..74]   camera box x[241..256] y[53..70]  16x18
```

**Identical.** The box is 16x18 whether the viewport is 32 tiles or 56, so it is
drawn from something other than the viewport descriptor.

Worth noting for whoever picks this up: 16x18 is not even proportional to the
native 32x24 viewport -- its aspect is 0.89 against the viewport's 1.33 -- so it
is closer to a fixed-size marker than a scaled rectangle. Making it match the
window means finding what actually sizes it, not scaling what is there.

**Requirement not met.**

## Units in the widened margin: still unproven

`gx_polygons` reads **3** before widening and **3** after. That is not evidence
either way: this match contains six entities -- two Heroes, two Builders, two
Bases -- and all of them sit inside the original 32-tile view, so no test done so
far could have shown a difference.

An attempt to aim the camera deliberately failed too: the entity fields guessed
at for world position, `+0x164` and `+0x166`, read **identically for all six
entities** (-26964, 532), so they are not positions. The real position offsets
are not known, which is what blocks a decisive test.

**Requirement unproven**, and the next step for it is finding the entity
position fields rather than repeating the polygon count.

## The HUD does anchor correctly

The top screen at 448 shows gold at the far left, population centred, and the
`0/0` counter at the far right, with panels spanning the width -- `docs/131`'s
anchoring working under the widened viewport. **This part is met**, for the
top-screen HUD specifically. The in-field buttons remain untested.

## Fog of war: the overrun is real and unaddressed

`docs/163` established the root descriptor is a one-bit-per-tile mask, and
`docs/164` raised its stride from 4 bytes to 7 without enlarging the backing
buffer -- 96 bytes allocated against 168 now implied. Nothing has been done about
that, and it must be resolved before any of this ships, because it is a write
past an allocation rather than a cosmetic defect.

## Summary

| requirement | status |
| --- | --- |
| units, buildings, 3D specials not culled | **unproven** -- needs entity position fields |
| fog of war / shroud | **latent buffer overrun** introduced by the widen |
| minimap box matches the window | **not met** -- box is fixed at 16x18 |
| edge-anchored UI | **met for the top-screen HUD**; in-field buttons untested |

The master control from `docs/164` is real and useful, but it is one of four
things, and on its own it satisfies none of them completely.
