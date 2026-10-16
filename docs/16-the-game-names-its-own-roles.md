# The cartridge ships a C header, and it names everything

`Sound/sound_data.sadl` is 49 KB and its first bytes are `#def`. `docs/04`
recorded that as "unread" and flagged it. It is **not audio** — it is a C
header, 1,311 `#define` lines, shipped inside the cartridge beside the 60 MB
`sound_data.sdat`, and it is the developers' own symbol names for the entire
sound archive.

That makes it the best source in the ROM for *the vocabulary they used*, and it
settles several names this project had inferred.

```
python tools\lb_sound_symbols.py out\eur\plain\Sound\sound_data.sadl --roles
```

| prefix | count | what |
| --- | ---: | --- |
| `SE_` | 784 | sound effects — one per entity per behaviour event |
| `SEQARC_` | 217 | sequence archives |
| `BANK_` | 200 | instrument banks: 60 unit, 60 structure, 34 spell, 34 colour-faction |
| `STRM_` | 98 | streamed music |
| `WAVE_` | 8 | wave archives, by content group |
| `PLAYER_`, `STRMPLAYER_` | 4 | |

All 1,311 lines parse as `#define`; none is anything else.

## The ten unit roles and the ten structure roles

`BANK_UNIT__<FACTION>_<ROLE>` and `BANK_STRUCTURE__<FACTION>_<ROLE>`, 60 of each
— six factions × ten roles — and **the ten role names are identical for all six
factions**, in the same order:

| # | unit role | structure role |
| ---: | --- | --- |
| 0 | `HEROM` | `BASE` |
| 1 | `HEROF` | `MILL` |
| 2 | `BUILDER` | `MINE` |
| 3 | `MELEE` | `FARM` |
| 4 | `RANGED` | `BARRACKS` |
| 5 | `MOUNTED` | `VEHICLECENTER` |
| 6 | `SIEGEONE` | `TOWER` |
| 7 | `SIEGETWO` | `TOWER2` |
| 8 | `SIEGETHREE` | `TOWER3` |
| 9 | `TRANSPORT` | `SHIPYARD` |

Ten plus ten is **exactly the twenty units per faction** `docs/12` counted from
the blueprints, and the roles line up one-for-one with the category enum at
`UnitBlueprint+0x5C` and with the faction slot template in `docs/13`. Four
independent structures now agree, and the fourth brought the names.

### Corrections applied

`docs/12` and `docs/13` named the categories by reading the roster, which got
the *grouping* right and four of the *names* wrong. Corrected in the tools and
the tables:

| this project inferred | the game calls it |
| --- | --- |
| `CloseCombat` | **`Melee`** |
| `Special` / `Special1,2,3` | **`Siege`** / `SiegeOne, SiegeTwo, SiegeThree` |
| `LumberMill` | **`Mill`** |
| `SpecialFactory` | **`VehicleCenter`** |
| `Tower1` | **`Tower`** |

`VehicleCenter` is the useful one. `docs/12` listed that slot as King *Stables*,
Wizard *Cauldron*, Pirate *Jetty*, Imperial *Drydock*, Earth *VehicleFactory*,
Alien *CreationPit* and called it a "special factory" because no single name
covers a stable and a cauldron. The game's own answer is that they are all the
**vehicle centre** — the building that produces the three Siege units.

`HEROM`/`HEROF` also explains something `docs/13` left open. The faction slot
template has **one** Hero slot, yet every faction has two heroes
(`K_King`/`K_King_F`). The sound banks split them by gender, so the second hero
is not a roster slot at all — it is the same slot with a male and a female
variant, which is why the `_F` entities exist without a template position.

## The behaviour vocabulary

`SE_<FACTION>_<ROLE>_<EVENT>` — and there are exactly **five events**:

| event | count | applies to |
| --- | ---: | --- |
| `ONSELECT` | 120 | units and structures |
| `ONDEATH` | 120 | units and structures |
| `ONHIT` | 120 | units and structures |
| `ONSTRIKE` | 60 | **units only** |
| `ONSPECIAL` | 60 | **units only** |

120 = 6 factions × 20 entities; 60 = 6 × 10 units. So every unit and every
building has a select, death and hit sound, and only units have a strike
(attacking) and a special.

This is the game's own event vocabulary for entity behaviour, and it is a short
list: an entity is selected, it attacks, it uses its special, it is hit, it
dies. Anything a port has to hook for audio is one of those five.

## The spell list, in full

34 `BANK_SPELL__*` entries, against the 24 spell names in `LOC` 263–286. The
extra ten are the buffs split into per-unit and area-effect variants, plus the
damage/impact halves of two projectile spells:

```
ARROWVOLLEY  ARTILLERY  CLUSTERBOMB  CRABSWARM  CRYSTALSPAWN  EARTHQUAKE  EMP
ESPATTACK  FIREBALL  FIREBALLEXPLOSION  FORESTSPAWN  HOTWIRE  ISLANDERSUMMONS
JUNGLESPAWN  LIGHTNINGBOLT  LIGHTNINGBOLTDAMAGE  MONKEYSWARM  NINJASUMMONS
ROAR  SPACELASER  TELEPORT  THUNDERHAMMER  TRACKING
CHOPPINGBUFF  MININGBUFF
BUFFARMOURINCREASE{PERUNIT,AREAEFFECT}
BUFFDAMAGEINCREASE{PERUNIT,AREAEFFECT}
BUFFSPEEDINCREASE{PERUNIT,AREAEFFECT}
HEALING{HERO,UNITEFFECT,AREAEFFECT}
```

`LOC` calls the buffs "Unit Damage Boost" and "Area Damage Boost"; the banks
confirm those are the same spell in two targeting modes. `CHOPPINGBUFF` is the
`LOC` "Logging Buff".

## Why this was worth doing first

It cost one `head -c 1400`. `docs/04` had already identified the file, recorded
that it begins `#def`, and written *"read it before assuming anything about the
streamed-audio path"* — and then it sat in the open-questions list for three
working passes while harder things were done.

**An open question that says "this is 49 KB of text" should be read before any
question that needs a disassembler.** The cost is bounded and known in advance;
the payoff was the developers' names for four things this project had guessed
at, a confirmation of the roster structure from a fourth independent direction,
and the complete behaviour-event vocabulary.
