# `BP/Factions.fbp` — rosters, and how the campaign gates them

**Status: solved.** `tools/lb_factions.py`, table in
[`docs/data/factions.tsv`](data/factions.tsv).

```
records 130  pool at 0x2084  pool bytes 1692
base factions:
   124  King       26 of 26 slots filled
   125  Wizard     26 of 26 slots filled
   126  Pirates    26 of 26 slots filled
   127  Imperial   26 of 26 slots filled
   128  Earth      26 of 26 slots filled
   129  Aliens     26 of 26 slots filled
```

## Derivation

Same route as `docs/11`, and it took one pass. `func_020025D4` requests the
file; `func_02002644` dispatches the completion code; `func_0200268C` stores the
buffer; and `func_02002974` is the loop:

```arm
mov  r5, #0
mov  r6, r5
loop:
  ldr  r0, [r4, #8]     ; the file buffer
  add  r0, r0, #4       ; past the 'FANZ' magic
  add  r0, r0, r6
  bl   func_020029AC    ; per-record fixup
  add  r5, r5, #1
  cmp  r5, #0x82        ; 130 records
  add  r6, r6, #0x40    ; of 0x40 bytes
  blo  loop
```

The fixup names the string-pool base as a literal:

```arm
func_020029B8:
  ldr r2, [r0]
  ldr r1, [buffer]
  add r1, r1, #0x84
  add r1, r1, #0x2000   ; +0x2084
  add r1, r1, r2
  str r1, [r0]
```

`0x2084 == 4 + 130 * 0x40` **exactly**, so the pool begins immediately after the
last record. Count and stride confirm each other, the same way the entity file's
walk confirms itself by landing on its pool.

## Record layout, 0x40 bytes

| offset | width | field |
| --- | --- | --- |
| `+0x00` | u32 | name offset from the pool at `0x2084` |
| `+0x04` | u8 | variant index |
| `+0x05` | u8 | base faction — **a record index in this same file**, 124–129 |
| `+0x06` | u16 | display-name string id in `LOC/*.lng` |
| `+0x08` | u32 | zero throughout |
| `+0x0C` | u16[26] | entity blueprint index per role slot, `0xFFFF` = unavailable |

The six base factions' display-name ids are `LOC` 567–572 = *King, Wizard,
Pirates, Imperial, Earth, Alien* — the same six sides the campaign uses
(`docs/08`) and the same six letters the entity names use (`docs/12`). Three
independent parts of the ROM now agree on that list.

## The 26-slot role template

Every one of the six base factions fills all 26 slots, and slot *n* holds the
same role in each. The Alien roster, in full:

| slot | role | Alien | | slot | role | Alien |
| ---: | --- | --- | --- | ---: | --- | --- |
| 0 | Hero | `A_AlienKing` | | 13 | Barracks | `A_BreedingPit` |
| 1 | Builder | `A_Engineer` | | 14 | VehicleCenter | `A_CreationPit` |
| 2 | Melee | `A_Melee` | | 15 | Shipyard | `A_Shipyard` |
| 3 | Ranged | `A_Ranged` | | 16 | Tower | `A_Spire` |
| 4 | Mounted | `A_Mounted` | | 17 | Tower2 | `A_Spire2` |
| 5 | SiegeOne | `A_SiegeOne` | | 18 | Tower3 | `A_Spire3` |
| 6 | SiegeTwo | `A_SiegeTwo` | | 19 | Wall | `Wall` |
| 7 | SiegeThree | `A_SiegeThree` | | 20–25 | Bridges | the six shared `Bridge*` |
| 8 | Transport | `A_TransportShip` | | | | |
| 9 | Base | `A_CraterBase` | | | | |
| 10 | Mill | `A_Harvester` | | | | |
| 11 | Mine | `A_WellCap` | | | | |
| 12 | Farm | `A_Hatchery` | | | | |

Slots 19–25 point at the **shared neutral** `Wall` and `Bridge*` blueprints —
the same seven entity records in all six factions, which is why those show up in
`docs/12` as neutral rather than faction-prefixed.

This is the third structure to independently reproduce the same role list: the
category enum at `UnitBlueprint+0x5C`, the roster grouping, and the slot
template. A **fourth** followed later and brought the names with it — the sound
symbol header (`docs/16`), whose `BANK_UNIT__*` and `BANK_STRUCTURE__*` symbols
give ten unit roles and ten structure roles per faction. The role names above
are that header's, not this project's.

It also answers what this document previously left open. The template has
**one** Hero slot yet every faction has two heroes (`K_King`/`K_King_F`); the
sound banks name them `HEROM` and `HEROF`, so the second hero is not a roster
slot at all but a gendered variant of the same one.

## The variants are the campaign's tech tree

124 of the 130 records are **per-mission variants**, named
`<Faction><Campaign>M<NN>_<Who>`:

```
KCM01_MinimalForces  KCM01_User  KCM01_User_LockedOut  KCM02_Enemy  KCM02_User …
WCM01_KingsMen       WCM01_User  …
ECM01_Aliens         ECM06_Team3 …
KC_King_NoDragon     KC_Wizard_NoDragon
```

Each is the 26-slot template with entries blanked to `0xFFFF`. So a mission
does not "unlock" units by a flag — it **selects a whole roster record**, and the
missing slots simply are not there. The extreme cases:

| variant | base | slots available |
| --- | --- | ---: |
| `KCM06_User_LockedOut` | King | **0** |
| `ECM01_Aliens` | Aliens | **0** |
| `KCM04_User_LockedOut` | King | 1 |
| `KCM05_User_LockedOut` | King | 1 |
| `KCM03_User_LockedOut` | King | 3 |

`KC_King_NoDragon` and `KC_Wizard_NoDragon` are the same idea named plainly: the
full roster minus one Special.

For the port and the mod scopes this is the useful shape — **a mission's
buildable set is one row of a table**, and adding or restricting units is
editing that row, not touching code.

## What this still does not say

The template gives what a faction *has*, not **what builds what**. Nothing here
says a Barracks produces the Melee unit.

### Negative results on the producer mapping

Two candidates were checked and eliminated, so the next attempt does not repeat
them:

**`UnitBlueprint+0x20` is not it.** It is the one `u16` in the record that is
populated for every structure and empty for every mobile unit, which is exactly
the shape a "what I produce" field would have. But its value is **identical for
the same category across all six factions** — `Base` 0, `LumberMill` 250, `Mine`
18, `Farm` 12, `Barracks` 26, `VehicleCenter` 232, `Tower` 7, `Tower2` 175,
`Tower3` 180, `Shipyard` 224 — so it encodes something about the *category*, not
about a faction's roster. A per-faction producer link cannot be faction-invariant.

Reading those values as blueprint indices produces confident nonsense
(`K_Barracks → W_Ballista`, `K_Farm → K_Mine`), which is the trap `docs/11`
warns about: a small integer lands inside the 0–550 index range by default.

**`Battles::User::BuildTab::virtual_08` is not a roster lookup.** It is a 95-way
jump table dispatching on a `u16` action id read from a UI structure at `+0x02`,
biased by `0x2C8`. That is button handling, not roster construction.

Still to try: `Battles::ProduceEntityCommand` and
`Battles::CreateUnitFactionCommand` (vtables at `0x0213B32C` and `0x0213B1C0`),
and the runtime faction object rather than the serialised one — the mapping may
be built at load time from the slot template rather than stored at all, which
the rigidity of that template would make natural.

Also unidentified: upgrades (`Battles::UpgradeBlueprint` is in the RTTI but is
not one of the three types in `Entities.ebp`), and the 24 spells named in
`LOC` 263–286.
