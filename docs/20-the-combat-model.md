# The combat model, and the 30 Hz tick

**Damage is a random range, attack interval is in 1/30-second ticks, and the
whole thing is confirmed by an identity that reproduces a third-party stat
sheet's DPS column exactly on 19 of 22 units.**

This is the piece that turns `docs/12`'s table of numbers into behaviour.

## The three fields

| field | offset | meaning |
| --- | --- | --- |
| melee damage | `UnitBlueprint+0x68` | **minimum** |
| melee spread | `UnitBlueprint+0x6A` | maximum is `min + spread` |
| projectile damage | `ProjectileBlueprint+0x6C` / `+0x6E` | **minimum and maximum**, directly |
| attack interval | `UnitBlueprint+0x6D` | **ticks at 30 Hz** |

A unit with a projectile (`+0x66` ≠ `0xFFFF`) has **zero melee damage** and all
its damage comes from the projectile record; a melee unit has no projectile.
There is no unit that does both.

## The identity that proves it

The community rebalance sheet carries a `DPS` column. For every unit it names:

```
DPS = average_damage × 30 / attack_interval
```

where `average_damage` is `(min + max) / 2` — the midpoint of whichever range
applies. Computed against the **patched** `Entities.ebp` and compared to the
sheet:

| unit | damage source | sheet DPS | computed |
| --- | --- | ---: | ---: |
| `K_King` | melee 40+10 | 67.50 | **67.50** |
| `W_Wizard` | `TFireball` 80–90 | 63.75 | **63.75** |
| `P_Captain` | `ICannonBall` 50–75 | 55.15 | **55.15** |
| `E_Commander_F` | `ALaser` 25–35 | 75.00 | **75.00** |
| `DwarfKing` | melee 200+0 | 100.00 | **100.00** |
| `AgentChase` | `AirLaser` 185–195 | 114.00 | **114.00** |

**19 of 22 exact.** The three misses (`A_AlienKing` and `Conquistador` both
reporting a DPS of exactly 1.00, and `ClassicSpace`) are rows whose numbers the
PDF extraction split wrongly, not disagreements about the model.

Three separate things fall out of that one identity holding:

1. `+0x68`/`+0x6A` really are **(minimum, spread)** and not (maximum, minimum).
   Both readings fit the raw data — `+0x6A` is non-zero exactly when `+0x68` is,
   and always `≤` it — and only this one reproduces the DPS.
2. The projectile pair at `+0x6C`/`+0x6E` really is **(min, max)**.
3. **The constant is 30, so the attack interval is in 30 Hz ticks.**

## The simulation runs at 30 Hz

The DS refreshes at ~59.83 Hz, so a game tick of 30 Hz means **the simulation
steps once every two frames**. That is not a detail — it is the number a port has
to preserve. The port's goal is to *target 60 fps while
preserving the game's own DS gameplay tick at all times*. For LEGO Battles that
tick is 30 Hz, and every interval in the blueprints is denominated in it.

It also makes the interval values legible. `K_Tower3`'s interval of 60 is **two
seconds** between shots; `K_King`'s 20 is **two thirds of a second**;
`K_TransportShip`'s 42 is 1.4 s.

## The King roster as behaviour

| name | category | damage | interval | DPS | from |
| --- | --- | --- | ---: | ---: | --- |
| `K_King` | Hero | 40–50 | 20 | 67.5 | melee |
| `K_King_F` | Hero | 30–40 | 20 | 52.5 | melee |
| `K_Engineer` | Builder | 5–5 | 30 | 5.0 | melee |
| `K_Swordsman` | Melee | 10–15 | 30 | 12.5 | melee |
| `K_Archer` | Ranged | 15–20 | 24 | 21.9 | `Arrow` |
| `K_Knight` | Mounted | 40–50 | 30 | 45.0 | melee |
| `K_Ballista` | Siege | 65–75 | 24 | 87.5 | `BallistaBolt` |
| `K_Catapult` | Siege | 50–60 | 30 | 55.0 | `Boulder` |
| `K_Gryphon` | Siege | 50–60 | 30 | 55.0 | `Fireball` |
| `K_TransportShip` | Transport | 15–20 | 42 | 12.5 | `Arrow` |
| `K_Tower` | Tower | 35–45 | 24 | 50.0 | `TBolt` |
| `K_Tower2` | Tower2 | 65–75 | 40 | 52.5 | `TBoulder` |
| `K_Tower3` | Tower3 | 85–95 | 60 | 45.0 | `TFireball` |

Things this makes visible that the raw numbers did not:

- **The Ballista is the King's best damage dealer at 87.5 DPS** — more than the
  Catapult (55) and the Gryphon (55), and it costs 350 against their 500 and 900.
  Its edge is the 24-tick interval; the Catapult and Gryphon hit harder per shot
  only in the sense of having a larger *minimum*, and fire 25% less often.
- **The Engineer's 5 damage has zero spread.** It is the only unit in the roster
  with a deterministic hit.
- **Tower tiers do not escalate in DPS.** 50 → 52.5 → 45. Tower III is *worse*
  damage-per-second than Tower I; what it buys is range 7 versus 5, health 700
  versus 400, and an 85-minimum shot that one-shots weak units. A tier upgrade
  in this game is reach and burst, not throughput.
- **A Swordsman loses to an Archer straight up** — 12.5 DPS against 21.9 — and
  costs more per point of it. The melee unit's advantage has to be elsewhere:
  350 health against 300, and no dependence on range.

## What is still unnamed

> **Two of these are answered by `docs/59`.** `ProjectileBlueprint+0x6B` is
> **"explodes"** — read off a `cmp/beq` in `func_0206E450` and then found set on
> exactly the 11 splash weapons and clear on all 16 direct ones. And the second
> damage pair is the **impact** damage where the first is the **direct hit**:
> `func_0206E408` passes `+0x6C`/`+0x6E`, `func_0206E450` and `func_0206E60C`
> pass `+0x70`/`+0x72`. The data could never have shown that, because all 27
> records carry the two pairs equal; the distinction is which code path reads
> which. **`docs/66` then closed the effect id at `+0x5D`: it is an
> animation-set id into an 18-byte-record table, and 0 means "no sprite" --
> which is why it groups by projectile kind. `docs/64` identified `+0x71` as
> `interaction_range`.** **`docs/67` then closed `UnitBlueprint+0x70`: it is the target-priority score the four combat commands maximise. All five questions in this section are now answered.** Left below as
> written.

`UnitBlueprint+0x70` and `+0x71` remain unidentified, as do the 14 boolean flags
at `ProjectileBlueprint+0x5E`–`+0x6B` and the effect id at `+0x5D` (values 0–13,
grouping by projectile kind: `Boulder` and `TBoulder` share 13, `Fireball`,
`AirFireball` and `SharkBite` share 0).

The `ProjectileBlueprint` also carries **a second damage pair** at `+0x70`/`+0x72`
that is byte-identical to the first in all 27 cartridge records and in every one
the community patch edits. Whatever distinguishes the two is unobserved, so it
is recorded rather than named.
