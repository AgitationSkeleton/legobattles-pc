# Spells and buffs: 24 names, nine behaviours

The RTTI (`docs/07`) names the whole magic system, and the shape it describes is
smaller than the spell list suggests.

## Nine implementations behind 24 names

`Battles::Sim::SpellBase` has **nine** subclasses:

| class | what the name says it does |
| --- | --- |
| `Sim::DamageSpell` | direct damage |
| `Sim::HealSpell` | healing |
| `Sim::StatBufSpell` | a stat buff |
| `Sim::ProjectileSpell` | fires something — with three of its own subclasses, `ProjectileSpellFireBall`, `ProjectileSpellLightningBolt`, `ProjectileSpellThunderHammer` |
| `Sim::TeleportSpell` | |
| `Sim::TrackingSpell` | |
| `Sim::HotWireSpell` | |
| `Sim::ForrestSpell` *(sic)* | the forest/jungle spawns |
| `Sim::EAttackSpell` | the Earth faction's attack spell |

Against that, `LOC` 263–286 lists **24 spell names** and `Sound/sound_data.sadl`
(`docs/16`) lists **34 spell banks**. So most spells are not distinct code: they
are parameterised instances of a handful of behaviours. All six of the
`LOC` buffs — Unit/Area × Speed/Damage/Armour — are one `StatBufSpell`, which is
exactly what the sound banks already implied by splitting each buff into
`…PERUNIT` and `…AREAEFFECT` variants of one bank.

**No spell factory exists.** Searching for a function that references more than
one `Sim::…Spell` vtable finds none, so the nine classes are not dispatched from
a table keyed by spell id — they are constructed at separate sites. Whatever
carries a spell's *parameters* is therefore not a spell blueprint file; there is
no `.sbp`, and `Entities.ebp` has only the three types in `docs/11`.

## Buffs are attached objects, not stat edits

The buff mechanism is named in full:

```
Sim::MagicInfluenceBase
  ├── Sim::StatBufMagicInfluence
  └── Sim::UpgradeMagicInfluence
Sim::MagicInfluencePool      (an IndexAndInstancePool)
Sim::MagicInfluencedUnitStats
Battles::ArrayMagicInfluenceRef
```

So a buff does **not** modify a unit's stats in place. An influence object is
allocated from a pool, referenced by the unit through an
`ArrayMagicInfluenceRef`, and the unit's effective numbers are computed into a
`MagicInfluencedUnitStats`. Upgrades use the same machinery
(`UpgradeMagicInfluence`, `Sim::AppliedUpgrade`), which is why researching an
upgrade and casting a buff can share one code path.

That matters for a port and for the mod scope: **a unit's blueprint numbers are
its base, and the number the simulation uses is derived.** Anything reading
`docs/12`'s table as "the" stats is reading the unbuffed case.

`Battles::SpellIconsEffect` has nine subclasses — Armour, Chopping, Heart,
HotWire, Mining, Speed, Sword, Tracking, TradeWind — which is the HUD side: the
icons drawn over a unit while an influence is attached. Nine icons for a system
with nine influence kinds is a consistent count, not a coincidence.

## A `u32` table of `LOC` ids, in enum order

At `0x0214DF48` there is a 66-entry `u32` table of `LOC` string ids, and it is
**not ascending**:

```
index  0..21   ->  291..312   Area Damage Boost … Load
index 22..39   ->  269..286   Roar … Cluster Bomb
index 40..43   ->  287..290   Remove Me … Unit Damage Boost
index 44..65   ->  313..334   Unload … <1>:<2>:<3>
```

Ascending runs in a shuffled order is the signature of an **enum-to-string map**
— the table is indexed by an internal id and yields the label. Its contents span
spells (269–286), buffs (287–295) and player commands (302–319: Attack, Patrol,
Move, Retire, Stop, Repair, Load, Unload, Build, Stand Ground, Set Rally Point,
Research Spell, Upgrade to Level 2/3), so the enum it serves is a single
**UI action space** covering all three.

That is a different table from the ascending `u32[21]` at `0x0214E370` holding
`242…262`, which `docs/12` correctly identified as the category *string list*
rather than a permutation. The permutation `docs/12` wanted is still not found;
this one is the action map, not the category map.

## What is still unknown

- **Spell parameters** — cost, cooldown, radius, duration. Not in `Entities.ebp`,
  not in a file named in the ARM9, and not behind a factory table. The likeliest
  home is constants compiled into each `SpellBase` subclass's constructor, which
  would mean there is no data file to find.
- Which of the 24 names maps to which of the nine classes.
- `Battles::UpgradeBlueprint` exists in the RTTI but is not one of the three
  types in `Entities.ebp` and has no file of its own. Same question, same likely
  answer.
