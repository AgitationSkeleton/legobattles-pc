# The twenty slots

**Status: the blueprint roster and the shipped sound header are joined, 120
cells, and they agree 240 times out of 240.** Three new verification checks; 50
of 50 pass. `docs/data/faction_roster.tsv` is the joined table.

Two files on this cartridge describe the same twenty things per faction, and
neither one references the other:

* `BP/Entities.ebp` — the simulation's blueprints (`docs/11`, `docs/12`).
  120 of its 551 records carry a faction, exactly 20 per faction.
* `Sound/sound_data.sadl` — the developers' own C header (`docs/16`,
  `docs/52`). It declares ten `BANK_UNIT__<FACTION>_<SLOT>` and ten
  `BANK_STRUCTURE__<FACTION>_<SLOT>` for the same six factions.

## The join, and the check

Take a faction's twenty blueprints **in index order** and lay them against the
header's ten unit slots followed by its ten structure slots. The blueprint's own
`category` field then names the slot:

```
HEROM        K_King            category Hero
HEROF        K_King_F          category Hero
BUILDER      K_Engineer        category Builder
MELEE        K_Swordsman       category Melee
RANGED       K_Archer          category Ranged
MOUNTED      K_Knight          category Mounted
SIEGEONE     K_Ballista        category Siege
SIEGETWO     K_Catapult        category Siege
SIEGETHREE   K_Gryphon         category Siege
TRANSPORT    K_TransportShip   category Transport
BASE         K_Castle          category Base          is_structure True
MILL         K_LumberMill      category Mill          is_structure True
MINE         K_Mine            category Mine          is_structure True
FARM         K_Farm            category Farm          is_structure True
BARRACKS     K_Barracks        category Barracks      is_structure True
VEHICLECENTER K_Stables        category VehicleCenter is_structure True
TOWER        K_Tower           category Tower         is_structure True
TOWER2       K_Tower2          category Tower2        is_structure True
TOWER3       K_Tower3          category Tower3        is_structure True
SHIPYARD     K_Shipyard        category Shipyard      is_structure True
```

**120 of 120 cells: the category names the slot. 120 of 120: `is_structure`
agrees with the slot's kind.** Two slots need a rule and it is the same rule in
both files — `HEROM`/`HEROF` are the two `Hero` rows in order, and
`SIEGEONE`/`SIEGETWO`/`SIEGETHREE` are the three `Siege` rows in order.

Nothing makes those agree except both files describing one roster. That is what
licenses using the header's vocabulary — which is the developers' — for the
blueprint rows, instead of names inferred here.

## The buildings are the same building six times

Ten structure slots x nine stats = 90 cells per faction. Compared across all
six factions:

```
structure   89 of 90 cells identical across all six factions
unit        53 of 90
```

**The single exception is the King's basic Tower, with attack range 5 where
every other faction's is 6.** One cell out of ninety. Everything else — cost,
health, build time, resource yield, the two upgraded tower tiers — is byte-for-
byte the same for King, Wizard, Pirate, Imperial, Earth and Alien.

```
slot            cost  health  build  yield  range
BASE            1000    1500    900      0      1
MILL             400     750    540      0      1
MINE             600    1250    690     25      1
FARM              75     350    360      0      1
BARRACKS         200     750    450      0      1
VEHICLECENTER    350    1000    600      0      1
TOWER            300     400    420      0      6   (King: 5)
TOWER2           500     500    840      0      6
TOWER3           800     700   1260      0      7
SHIPYARD         350    1000    600      0      1
```

The buildings differ only in name and model. `K_Castle` is a Castle, a Skeleton
Tower, a Pirate's Hold, a Fortress, an Eagle Command and a Crater Base; the
numbers behind all six are one row.

## Three stats are symmetric everywhere

`mana`, `build_time` and `resource_yield` are identical across all six factions
on **all twenty slots**, units included. `cost` is identical on 17 of 20 — only
`BUILDER`, `MELEE` and `SIEGETHREE` are priced differently.

So the economy is perfectly symmetric: same build times, same costs (nearly),
same yields. Differentiation is entirely in what the units *do* once built.

## Where the units differ

Health varies on 9 of the 10 unit slots, speed on 8, attack range on 5,
attack interval on 6, melee damage on 4.

```
health        King  Wizard  Pirate  Imperial  Earth  Alien
HEROM         1000    1100    1000      1100   1000   1000
MELEE          350     225     300       250    275    225
RANGED         300     250     200       200    300    250
MOUNTED        300     300     400       450    400    350
SIEGEONE       450     450     550       550    450    450

speed (tiles/sec)
MOUNTED        4.5     4.5     5.0       4.0    4.0    5.0
SIEGEONE       3.5     3.0     6.0      6.25    3.5    3.0
SIEGETHREE     6.0     6.0     3.5       3.0    6.0    6.0

attack range
RANGED           5       4       5         4      5      4
SIEGEONE         7       7       5         5      5      5
SIEGETWO         8       8       6         6      6      6
SIEGETHREE       3       3       7         7      7      7
```

The siege line inverts between the fantasy factions and the rest. King and
Wizard get slow, long-ranged `SIEGEONE`/`SIEGETWO` (a Ballista at range 7, a
Catapult at range 8) and a fast, short-ranged `SIEGETHREE` — a dragon, speed 6
at range 3. Pirate, Imperial, Earth and Alien get the opposite: fast ships early
(`SIEGEONE` at speed 6.0 to 6.25) and a slow long-range capital ship last
(range 7).

### A pairing that is *not* in the numbers

The campaign pairs the factions into three stories (`docs/08`), and it is
tempting to expect the stats to cluster the same way. **They do not.** Counting
identical cells over the ten unit slots x nine stats:

```
Wizard   Alien      71 of 90     most alike
King     Wizard     70
King     Earth      70
Earth    Alien      69
Pirate   Imperial   67
...
Wizard   Pirate     59 of 90     least alike
```

The spread is 59 to 71 out of 90 with no gap anywhere in it. Every faction is
broadly similar to every other, and the narrative pairing leaves no trace in the
balance data. Recorded because the opposite would have been an easy thing to
assert and it is not true.

## The roster, by name

| slot | King | Wizard | Pirate | Imperial | Earth | Alien |
| --- | --- | --- | --- | --- | --- | --- |
| `HEROM` | King | Wizard | Captain Brickbeard | Governor Broadside | Biff | Alien Commander |
| `HEROF` | Queen | Sorceress | Lady Pirate | Camilla | Gemma | Alien Queen |
| `BUILDER` | Builder | Builder | Builder | Builder | Builder | Builder |
| `MELEE` | Guardsman | Skeleton | Oarsman | Swordsman | Trooper | Drone |
| `RANGED` | Archer | Crossbowman | Gunman | Rifleman | Commando | Laser Trooper |
| `MOUNTED` | Knight | Skeleknight | Horseman | Cavalry | Gripley | Grav Runner |
| `SIEGEONE` | Ballista | Siege Tower | Barracuda | Clipper | Claw Tank | Alien Attack Ship |
| `SIEGETWO` | Catapult | Troll Giant | Battleship | Battleship | Starfighter | Alien Strike Ship |
| `SIEGETHREE` | King's Dragon | Wizard's Dragon | Brickbeard's Bounty | Flagship | Starship 1 | Alien Assault Ship |
| `TRANSPORT` | Transport Ship | Transport Ship | Transport Ship | Transport Ship | Transport Ship | Transport Ship |
| `BASE` | Castle | Skeleton Tower | Pirate's Hold | Fortress | Eagle Command | Crater Base |
| `MILL` | Lumber Mill | Lumber Shack | Lumber Shack | Lumber Mill | Harvester | Harvester |
| `MINE` | Mine | Mine | Quarry | Quarry | Well Cap | Well Cap |
| `FARM` | Farm | Graveyard | Plantation | Quarters | Biodome | Hatchery |
| `BARRACKS` | Barracks | Mausoleum | Shanty Shack | Barracks | Barracks | Breeding Pit |
| `VEHICLECENTER` | Special Factory | Cauldron | Jetty | Drydock | Vehicle Factory | Creation Pit |
| `TOWER` | Tower | Tower | Tower | Tower | Tower | Spire |
| `TOWER2`/`TOWER3` | Tower II/III | Tower II/III | Tower II/III | Tower II/III | Tower II/III | Spire II/III |
| `SHIPYARD` | Shipyard | Shipyard | Shipyard | Shipyard | Shipyard | Shipyard |

`Builder`, `Transport Ship` and `Shipyard` are not renamed by any faction. Only
the Alien faction renames the towers.

## What the sound header adds to each row

From `docs/52`, every one of the 120 rows has a sequence archive, and the event
set it raises says what kind of thing it is:

* **All 60 units raise five events** — `ONSELECT`, `ONDEATH`, `ONSTRIKE`,
  `ONHIT`, `ONSPECIAL`. Every unit has a special ability, including the Builder
  and the Transport Ship.
* **All 60 structures raise three** — `ONSELECT`, `ONDEATH`, `ONHIT`. **No
  structure raises `ONSTRIKE`, towers included.** A tower's shot belongs to the
  projectile, not to the tower.

## Open, and stated as open

> **Answered by `docs/58`.** The Mill is a *multiplier on harvesting* and the
> Farm raises a *population cap*, so neither produces and neither needs a
> `resource_yield`. The numbers: the Minifigure allowance is
> `min(20, 4 + 4 x completed Farms)`, and "Special units" is categories 5 and 6
> (Transport and Siege). Left below as it was written.

`resource_yield` is 25 on the `MINE` and **0 on the `MILL` and the `FARM`** for
every faction. Whatever makes a lumber mill or a farm produce is not this field.
The three are plainly economic buildings — same cost tier, same build times —
so the mechanism is elsewhere, and this document does not know where. Flagged,
not guessed.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | **99.1%** | 3,878 of 3,915 files |
| decompilation | 0.157% | 377 functions byte-verified |
