# The effect id is a sprite set

**Status: `ProjectileBlueprint+0x5D` is named — an animation-set id into an
18-byte-record table at `0x02127398`, with 0 meaning "no sprite". That is the
fifth and last of `docs/20`'s open questions to be closed by reading a
branch.** Two new checks; **88 of 88 pass**.

`docs/20` recorded the field and what it looked like:

> the effect id at `+0x5D` (values 0–13, grouping by projectile kind: `Boulder`
> and `TBoulder` share 13, `Fireball`, `AirFireball` and `SharkBite` share 0)

It groups by projectile kind **because it is the sprite set**. Things that look
alike share one.

## Two readers, and one of them is the whole answer

`lb_field.py` finds exactly two functions reading `+0x5D` through the
entity→blueprint idiom.

`func_0203AB00` is the draw gate:

```asm
ldrb r1, [r1, #0x5d]
cmp  r1, #0
beq  skip                 <- id 0 draws nothing
bl   #0x20ebf34
```

`func_020EBF34` passes it to two accessors, and `func_0200CE1C` states the
layout in three instructions:

```asm
mov  r2, #0x12
mul  r2, r0, r2           record stride, 18 bytes
ldrb r1, [r3, r2]         a frame count at +0
bl   modulo               index mod count
add  r0, r4, r1, lsl #1
ldrh r0, [r0, #2]         frames[] at +2, halfwords
```

**18 = 2 + 8 × 2**, so a record is a count byte, a pad byte, and up to **eight**
halfword frame ids.

## The table

```
 id  frames  ids                                          projectiles
  0       0  []                     Fireball, ProjectileSpell, AirFireball,
                                    SharkBite, ProjectileNoEffect
  1       5  0 2 4 6 8              Arrow, CrossbowBolt, TBolt
  2       1  10                     ImperialShot, PirateShot, TPirateShot
  3       5  272 274 276 278 280    BallistaBolt, SiegeBolt, AirBallistaBolt
  4       8  80 .. 94               ICannonBall, PCannonBall
  5       8  256 .. 270             PlasmaBall
  7       4  216 218 220 222        Gift
  8       8  128 .. 142             TFireball
 10       8  192 .. 206             LaserCannon
 11       5  12 14 16 18 20         ELaser, ALaser, TLaser, AirLaser
 13       8  64 66 68 70 72 74 76 64  TBoulder, Boulder, OgreBoulder
```

**Every group is a family that looks the same.** Three arrows and bolts share
set 1. Three bullets share set 2. **All four lasers share set 11.** All three
boulders share set 13. The two cannonballs share set 4.

And set 0 is **empty** — zero frames — which is why `func_0203AB00` skips the
draw when the id is zero. The code and the table agree from opposite
directions: the gate says "0 means don't draw" and the table says "0 has nothing
to draw". Neither was inferred from the other.

`Fireball`, `AirFireball` and `SharkBite` carry set 0 not because they are
invisible but because their visuals come from somewhere else — they are the
`+0x6B` splash weapons and the spell effects (`docs/24`), which have their own
animation classes.

## Five frames for eight directions

`func_020EBF34` buckets the projectile's angle into **sixteen** (`func_020EBE54`
with `#0x10`) and then folds:

```asm
cmp   r1, #4
ble   ...
cmp   r1, #8
rsble r1, r1, #8          r1 = 8 - r1
movle r6, #1              and set a mirror flag
...
cmp   r1, #0xc
bgt   ...
mov   r5, #1              a second flag
```

Sixteen directions folded onto a handful of sprites with **two mirror flags**.
That is why a set has 5 frames rather than 8, or 8 rather than 16: a boulder
tumbling is drawn from 8 stored frames, an arrow from 5 plus mirroring.

The exact fold — which flag is horizontal, which vertical, and how the two
combine to cover all sixteen buckets — **is not established here**. The two
`cmp/mov` pairs are recorded rather than interpreted.

Set 13's frame list ends `... 74 76 64`, reusing its first frame in the last
slot. With a count of 8 that is a deliberate 8-entry list whose last entry
repeats the first, not an off-by-one — the accessor takes `index mod count`, so
a wrong count would show up as a visible skip rather than a silent one.

## The five open questions from `docs/20`, now closed

| question | answer | where |
| --- | --- | --- |
| `ProjectileBlueprint+0x6B` | "explodes" — set on exactly the 11 splash weapons | `docs/59` |
| the second damage pair `+0x70`/`+0x72` | impact damage, where `+0x6C`/`+0x6E` is the direct hit | `docs/59` |
| the effect id `+0x5D` | an animation-set id; 0 means no sprite | here |
| `UnitBlueprint+0x70` | still open | — |
| `UnitBlueprint+0x71` | `interaction_range`, read by the harvest and command code | `docs/64` |

Four of five. `UnitBlueprint+0x70` is the one left, and `docs/11` notes only that
it is "accumulated into `Entity+0x44` in the combat path".

## Tips and pitfalls

* **Pin the base register when searching for a field.** A raw scan for
  `ldrb .., #0x5d]` returned six functions; three were reading a *stack* local
  at `sp+0x5D`. `lb_field.py` searches for the `[entity+8]` hop first and found
  the two real ones. The same class of mistake the tool's own history records
  for `pc`-relative loads.
* **A stride in a `mul` is a fact, not a guess.** `mov r2, #0x12; mul r2, r0, r2`
  is the record size, stated. Deriving 18 from the data would have been a
  guess that happened to be right.
* **Two directions agreeing is worth more than one.** The gate skipping id 0 and
  the table having nothing at id 0 are independent, and either alone would be
  weaker.
* **A name that explains an earlier observation is probably right.**
  `docs/20` noticed the grouping without being able to explain it; "it is the
  sprite set" explains it exactly.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 53.7% | 2,501 listed + 2,497 derived, 42 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
