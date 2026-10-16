# How a hit resolves

**Status: `docs/20`'s two open questions are answered. `ProjectileBlueprint+0x6B`
is "explodes", set on exactly the 11 splash weapons, and the mystery second
damage pair is the *impact* damage where the first is the *direct hit* — same
numbers in all 27 records, different code paths.** Four new checks; **78 of 78
pass**.

`docs/20` closed with:

> `UnitBlueprint+0x70` and `+0x71` remain unidentified, as do the 14 boolean
> flags at `ProjectileBlueprint+0x5E`–`+0x6B` [...] The `ProjectileBlueprint`
> also carries **a second damage pair** at `+0x70`/`+0x72` that is byte-identical
> to the first in all 27 cartridge records [...] Whatever distinguishes the two
> is unobserved, so it is recorded rather than named.

`docs/28`'s red-brick table points at the answer without any searching:
`RedBrick_OneHitKill` and `RedBrick_InvincibleHero` both list `func_0206E970`
among their readers. **That function is the damage applier.**

## The formula

`func_0206E970(projectile, target, dmgMin, dmgMax)`:

```
span   = dmgMax - dmgMin
roll   = dmgMin + (span > 0 ? rand(0, span) : 0)
scaled = fixmul(roll << 12, target[+0x174])        20.12
dmg    = scaled >> 12
newHP  = hp > dmg ? hp - dmg : 0                   hp is u16 at target+0x1A0
SetHealth(target, newHP)
```

`target[+0x174]` is a 20.12 multiplier initialised to `0x1000` — 1.0 — by
`func_0205AE90`, which **tier L places in `Sim::Entity`'s translation unit**.
That is `docs/57`'s locality tier doing real work in an investigation: an
otherwise anonymous function got a home, and the home was the right one.

An unbuffed unit takes exactly the roll. The multiplier scales damage *upward*
as it grows, so it is damage-taken rather than armour. **Which of `docs/24`'s
buffs write it is not established here** and is not guessed at.

## The multiply rounds, and that matters for a port

```asm
smull ip, r2, r3, r2
adds  r3, ip, #0x800        + 0.5
adc   r2, r2, #0
lsr   r3, r3, #0xc
orr   r3, r3, r2, lsl #20
```

`func_0204F2D8` is this game's 20.12 multiply. It is a **64-bit `smull`** and it
**adds `0x800` before shifting** — round to nearest, not truncate.

Anything reimplementing this with a plain `(a * b) >> 12` will drift from the
original by up to a point of damage per hit, and in a deterministic lockstep
simulation (`docs/25`) a one-point divergence is a desync, not a rounding error.
The verification check pins the `smull`, the `+0x800` and the `lsr #12`
separately.

The final conversion back to an integer **truncates** (`lsl #4; lsr #16`), so
the rounding is in the product and not in the result.

## The second damage pair, answered

Three functions call the applier, with two different pairs:

```
func_0206E408   ldrh r2,[bp,#0x6c] ; ldrh r3,[bp,#0x6e]    the direct hit
func_0206E450   ldrh r2,[bp,#0x70] ; ldrh r3,[bp,#0x72]    on impact
func_0206E60C   ldrh r3,[bp,#0x70] ; ldrh sb,[bp,#0x72]    in the blast
```

**The data cannot show the difference and the code can.** All 27 records carry
the two pairs equal — the check re-confirms it every run — which is exactly why
`docs/20` could not name them. The distinction is not in the values; it is in
which path reads which.

That is worth naming as a method: **a field whose data is degenerate is not an
unknowable field, it is a field to find a reader for.**

## `+0x6B` is "explodes"

`func_0206E450` branches on it:

```asm
ldrb r1, [bp, #0x6b]
cmp  r1, #0
beq  single_target          otherwise -> func_0206E60C, the area path
```

The flag's meaning was read off the branch **before** the data was looked at.
Then the data:

| `+0x6B` | count | projectiles |
| ---: | ---: | --- |
| 1 | 11 | `Boulder`, `OgreBoulder`, `TBoulder`, `Fireball`, `TFireball`, `AirFireball`, `ICannonBall`, `PCannonBall`, `PlasmaBall`, `LaserCannon`, `Gift` |
| 0 | 16 | `Arrow`, `CrossbowBolt`, `TBolt`, `BallistaBolt`, `SiegeBolt`, `ImperialShot`, `PirateShot`, `TPirateShot`, `ELaser`, `ALaser`, `TLaser`, `AirBallistaBolt`, `AirLaser`, `SharkBite`, `ProjectileSpell`, `ProjectileNoEffect` |

**Everything that should explode does, and nothing else does.** Boulders,
fireballs, cannonballs, plasma and Santa's Gift; not one arrow, bolt, shot or
laser. One of `docs/20`'s fourteen unnamed flags is now named, and the split is
27-for-27 semantically right.

The predicted-then-checked order matters. Reading the branch first means the
partition was a prediction the data could have falsified.

## The blast

`func_0206E60C` sweeps the tiles around the impact in **three rounds**, and for
each entity it finds it scales the damage by a **distance falloff** before
applying it — so a splash weapon does not hit everything in range for full
damage. `RedBrick_SuperExplosions` doubles the result, clamped to **65,535**.

## The two hero cheats are asymmetric

```
RedBrick_OneHitKill      the ATTACKER is category 0 (Hero) and the local player
                         -> resulting health := 0
RedBrick_InvincibleHero  the TARGET is category 0 and the local player
                         -> resulting health := current health
```

Both overwrite the **resulting health**, not the damage, which is why they are
exact rather than "a very large number". And both test
`blueprint+0x5C == 0` — the Hero category from `docs/58` — which is why the
cheat is called "One Hit Kill (**Heroes**)": a Guardsman gets nothing from it.

## What a hit looks like

```
Arrow            15-20  ->  15, 16, 17, 18, 19, 20        uniform over the range
Arrow, scale 1.5 15-20  ->  22, 24, 25, 27, 28, 30        1.5x, truncated
Fireball         50-60  ->  50 .. 60, and splash on top
```

## Tips and pitfalls

* **A degenerate field is a reader problem, not a data problem.** Two fields
  identical in every record cannot be told apart by looking at records. Find
  something that loads them.
* **Read the branch before the data.** `+0x6B` was named from a `cmp/beq`, and
  the 11-vs-16 split then had the chance to prove it wrong. Had the data been
  read first, the same conclusion would be a much weaker claim.
* **Pin the rounding, not just the arithmetic.** `+0x800` before a `>> 12` is
  the difference between matching the original and desyncing from it.
* **The red-brick table keeps paying.** Three passes running it has named the
  code for a subsystem: the unit cap, the harvest payout, and now the damage
  applier. Eighteen cheats, eighteen entry points.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 53.3% | 2,501 listed + 2,460 derived |
| decompilation | 0.157% | 377 functions byte-verified |
