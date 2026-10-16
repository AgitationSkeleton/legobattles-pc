# Units and buildings: how LEGO Battles is actually balanced

Everything here is read from `BP/Entities.ebp` by `tools/lb_blueprints.py`. The
full table is [`docs/data/entities.tsv`](data/entities.tsv), 551 rows. The format
and the evidence for each field name are in `docs/11`.

## The shape of the roster

551 blueprints:

| class | count | what |
| --- | ---: | --- |
| `Battles::UnitBlueprint` | 182 | **units and buildings both** — the game does not distinguish them by class |
| `Battles::ProjectileBlueprint` | 27 | |
| `Battles::CollectableItemBlueprint` | 342 | pickups, minikits, mission objects |

Of the 182: **120 are faction units — exactly 20 per faction, for six
factions** — and 62 are neutral or special (wildlife, ruins, bridges, gates,
walls, bonus heroes such as `Santa` and `ClassicSpace`).

The faction prefix on every name is the same six-letter scheme the campaign uses
(`docs/08`): `K` King, `W` Wizard, `P` Pirate, `I` Imperial, `E` Earth,
`A` Alien. That the roster and the campaign agree on the letters is a useful
independent confirmation of both.

## The category enum

`UnitBlueprint+0x5C`. The **grouping** below is read off the roster, not off a
table: each of categories 1, 4, 9, 13, 14, 15 contains **exactly six members, one
per faction**, and the members of each are plainly the same thing in six
costumes. That structure is what identifies them.

The **names** are the game's own, recovered later from `Sound/sound_data.sadl`
(`docs/16`). Four names in an earlier version of this document were inferred and
slightly wrong — `CloseCombat` is `Melee`, `Special` is `Siege`, `LumberMill` is
`Mill`, and `SpecialFactory` is `VehicleCenter`. The grouping was right
throughout.

| id | name | count | King | Wizard | Pirate | Imperial | Earth | Alien |
| ---: | --- | ---: | --- | --- | --- | --- | --- | --- |
| 0 | Hero | 25 | King, Queen | Wizard, Sorceress | Captain, Lady Pirate | Governor, Camilla | Commander ×2 | Commander ×2 |
| 1 | Builder | 6 | Engineer | Engineer | Builder | Engineer | Engineer | Engineer |
| 2 | Melee | 11 | Swordsman | Swordsman | Oarsman | Swordsman | Melee | Melee |
| 3 | Ranged | 10 | Archer | Crossbowman | Crossbowman | Rifleman | Ranged | Ranged |
| 4 | Mounted | 6 | Knight | Skeleknight | Horseman | Cavalry | Mounted | Mounted |
| 5 | Transport | 7 | TransportShip | TransportShip | TransportShip | TransportShip | TransportShip | TransportShip |
| 6 | Siege | 25 | Ballista, Catapult, Gryphon | Ballista, Giant, Dragon | Barracuda, Battleship, FlyingDutchman | … | … | … |
| 7 | Base | 9 | Castle | SkeletonTower | PiratesHold | Fortress | EagleCommandBase | CraterBase |
| 8 | Mill | 9 | LumberMill | LumberShack | LumberShack | LumberMill | Harvester | Harvester |
| 9 | Mine | 6 | Mine | Mine | Quarry | Quarry | WellCap | WellCap |
| 10 | Farm | 9 | Farm | Graveyard | ShantyShack | Tavern | Biodome | Hatchery |
| 11 | Barracks | 15 | Barracks | Mausoleum | Tavern | Barracks | Barracks | BreedingPit |
| 12 | VehicleCenter | 8 | Stables | Cauldron | Jetty | Drydock | VehicleFactory | CreationPit |
| 13 | Tower | 6 | Tower | Tower | LogTower | StoneTower | RocketTowers | Spire |
| 14 | Tower2 | 6 | Tower2 | Tower2 | LogTower2 | StoneTower2 | RocketTowers2 | Spire2 |
| 15 | Tower3 | 6 | Tower3 | Tower3 | LogTower3 | StoneTower3 | RocketTowers3 | Spire3 |
| 16 | Shipyard | 7 | Shipyard | Shipyard | Shipyard | Shipyard | Shipyard | Shipyard |
| 17 | Bridge | 6 | `BridgeSmall/Medium/Large` × horizontal, vertical |
| 18 | Gate | 2 | `GateH`, `GateV` |
| 19 | Wall | 1 | `Wall` |
| 255 | none | 2 | `MarsFalvour` *(sic)*, `SpaceCriminalShipColor` |

**This enum is not the order of the game's own category strings.** `LOC` ids
242–262 are `Close Combat, Ranged, Mounted, Hero, Builder, Special, Special One,
Special Two, Special Three, Transport, Base, Mill, Mine, Farm, Barracks, Special
Factory, Naval Factory, Tower, Wall, Bridge, Gate`, so the enum→string map is a
permutation: `0→Hero(245)`, `1→Builder(246)`, `2→Close Combat(242)`,
`3→Ranged(243)`, `4→Mounted(244)`, `5→Transport(251)`, `7→Base(252)`,
`13/14/15→Tower(259)`, `16→Naval Factory(258)`, `17→Bridge(261)`,
`18→Gate(262)`, `19→Wall(260)`.

A `u32[21]` table at `0x0214E370` holds exactly `242…262` in ascending order, so
that is the *string list* and **not** the permutation. The table that performs
the permutation has not been located. Recorded as an open question rather than
assumed away.

## The finding that matters for balance

**Every building has identical cost and identical health in all six factions.**

| category | cost | health |
| --- | ---: | ---: |
| Base | 1000 | 1500 |
| Mine | 600 | 1250 |
| Mill | 400 | 750 |
| Barracks | 200 | 750 |
| VehicleCenter | 350 | 1000 |
| Shipyard | 350 | 1000 |
| Farm | 75 | 350 |
| Tower | 300 | 400 |
| Tower2 | 500 | 500 |
| Tower3 | 800 | 700 |

Not "similar" — identical, across all six. The factions are a **reskin at the
building layer**: the same economy, the same build costs, the same structure
hit points, with different names and art.

Units are nearly as uniform. Cost is identical per category across factions —
Ranged 150, Mounted 250, Transport 250, Hero 500 — with the sole variation in
the Builder (45-55) and Melee (90-120) slots. Health is where the factions
actually differ:

| category | cost | health range |
| --- | ---: | --- |
| Hero | 500 | 900–1100 |
| Builder | 45–55 | 140–160 |
| Melee | 90–120 | 225–350 |
| Ranged | 150 | 200–300 |
| Mounted | 250 | 300–450 |
| Transport | 250 | 750–850 |
| Siege | 350-900 | 450-700 |

So a faction's identity in this game is **health, damage, attack interval, range
and its three Siege units** - not its economy. That is a useful thing to know
before porting the roster anywhere else, and it explains why the community's
rebalance patch concentrates on `+0x62`, `+0x68`, `+0x6D` and `+0x6F` and barely
touches cost.

## The King faction in full

The whole roster is in the TSV; one faction here to show the shape.

| name | display | category | cost | health | mana | speed | melee | range | projectile |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `K_King` | King | Hero | 500 | 1000 | 1000 | 410 | 40 | 1 | — |
| `K_King_F` | Queen | Hero | 500 | 1000 | 1000 | 478 | 30 | 1 | — |
| `K_Engineer` | Builder | Builder | 50 | 150 | 0 | 410 | 5 | 1 | — |
| `K_Swordsman` | Guardsman | Melee | 100 | 350 | 0 | 410 | 10 | 1 | — |
| `K_Archer` | Archer | Ranged | 150 | 300 | 0 | 478 | 0 | 5 | `Arrow` |
| `K_Knight` | Knight | Mounted | 250 | 300 | 0 | 614 | 40 | 1 | — |
| `K_Ballista` | Ballista | Siege | 350 | 450 | 0 | 478 | 0 | 7 | `BallistaBolt` |
| `K_Catapult` | Catapult | Siege | 500 | 550 | 0 | 410 | 0 | 8 | `Boulder` |
| `K_Gryphon` | King's Dragon | Siege | 900 | 700 | 0 | 819 | 0 | 3 | `Fireball` |
| `K_TransportShip` | Transport Ship | Transport | 250 | 850 | 0 | 683 | 0 | 4 | `Arrow` |
| `K_Castle` | Castle | Base | 1000 | 1500 | 65535 | 0 | 0 | 1 | — |
| `K_LumberMill` | Lumber Mill | Mill | 400 | 750 | 0 | 0 | 0 | 1 | — |
| `K_Mine` | Mine | Mine | 600 | 1250 | 0 | 0 | 0 | 1 | — |
| `K_Farm` | Farm | Farm | 75 | 350 | 0 | 0 | 0 | 1 | — |
| `K_Barracks` | Barracks | Barracks | 200 | 750 | 0 | 0 | 0 | 1 | — |
| `K_Stables` | Special Factory | VehicleCenter | 350 | 1000 | 0 | 0 | 0 | 1 | — |
| `K_Tower` | Tower | Tower | 300 | 400 | 0 | 0 | 0 | 5 | `TBolt` |
| `K_Tower2` | Tower II | Tower2 | 500 | 500 | 0 | 0 | 0 | 6 | `TBoulder` |
| `K_Tower3` | Tower III | Tower3 | 800 | 700 | 0 | 0 | 0 | 7 | `TFireball` |
| `K_Shipyard` | Shipyard | Shipyard | 350 | 1000 | 0 | 0 | 0 | 1 | — |

Details worth noticing, all of them consequences of the data rather than
opinions about it:

- **Movement speed lives at `+0x0C`, is a `u32`, and is in 20.12 fixed point** — `docs/26` shows the twelve distinct values are 2 to 7 tiles per second on clean quarter-tile boundaries, a third independent derivation of the 30 Hz tick. The
  King roster runs 410 for the King, the Builder and the Swordsman, 478 for the
  Queen and the Archer, 614 for the Knight, 683 for the Transport Ship and 819
  for the Gryphon. Every structure reads `0xFFFF`. (An earlier version of this
  document called `+0x15` the speed; it is not, and `docs/11` records why.)
- **Build time is `+0x60`, in 30 Hz ticks, and every value is a whole number of
  seconds** — 150 for a Builder (5 s), 900 for a Castle (30 s), 1260 for a Tower
  III (42 s). All 180 are exact multiples of 30 (`docs/25`).
- **Only heroes have mana.** `+0x64` is exactly 1000 for all 25 Hero-category
  units and 0 for every other unit and building, which matches the game: heroes
  are the only things that cast the 34 spells.
- **Ranged units carry no melee damage at all.** `+0x68` is 0 for every unit with
  a projectile. All of their damage lives in the `ProjectileBlueprint`.
- **The three tower tiers escalate on every axis**: cost 300/500/800, health
  400/500/700, range 5/6/7, and a different projectile each
  (`TBolt`/`TBoulder`/`TFireball`).
- **The Gryphon has range 3** — shorter than an Archer's 5 — so the game's
  flying Special is a close-range attacker that happens to fly, not an artillery
  piece. The Catapult, at range 8, is the longest-reaching thing the King has.
- **The Transport Ship shoots `Arrow`**, the same projectile as the Archer.

## Projectiles

27 of them, and the naming maps cleanly onto the factions and the tower tiers:

```
Arrow  CrossbowBolt  TBolt  TBoulder  TFireball  BallistaBolt  SiegeBolt
Boulder  OgreBoulder  Fireball  ImperialShot  PirateShot  TPirateShot
ICannonBall  PCannonBall  ELaser  ALaser  TLaser  PlasmaBall  LaserCannon
ProjectileSpell  AirBallistaBolt  AirFireball  AirLaser  SharkBite  Gift
ProjectileNoEffect
```

The `T…` prefix is the tower variant of a projectile and the `Air…` prefix the
airborne one, so a single weapon concept exists in up to three forms. `Gift` is
Santa's. `ProjectileNoEffect` is the null.

Their stat fields are not decoded yet. The rebalance patch edits projectile
offsets `0x61`–`0x72`, which is where the damage and speed live.

## Collectables

342 `CollectableItemBlueprint` records — by count the largest group in the file,
and almost all of them mission content: the minikit sets (`Minikits_K1_1` …
`Minikits_I3_5`, matching the per-act minikit counts the achievement strings
list), quest objects such as `CK1_3_Scroll`, and pickups like `Ticket`. Most
carry display-name id 385, `"[missing BP name]"`, because they are never named
in the UI.

## What this does not yet cover

- Population/supply cost, if the game has one — no field is identified for it.
- What builds what. The build menus must be a table somewhere; nothing here says
  a Barracks produces a Swordsman.
- Upgrades (`Battles::UpgradeBlueprint` exists in the RTTI but is not one of the
  three types in this file) and spells (24 named in `LOC` 263–286).
- Armour. `+0x6A` is 0–20 and non-zero only on melee units, which is the right
  shape for it, but there is no second source and it is left unnamed.
