# One field, six meanings: the collectable reference

**Status: solved. `CollectableItemBlueprint+0x60` is a polymorphic reference
whose interpretation is chosen by the category at `+0x5C`. All 342 records
account for, and every branch is verified against an independent source.**

This also yields the game's **red brick table** and the **minikit reward
mapping** — both of which are gameplay behaviour, not just format.

## How it was found, and a correction to `docs/26`

`docs/26` ran `lb_field.py` on `+0x60` and reported nine hits: the two
build-time readers, "plus seven in the `0x02000C..` blueprint-parser region".
**That description was wrong**, and the way it was wrong matters.

Those seven are not the parser. They are a run of three- and five-instruction
ARM functions sitting between `CollectableItemBlueprint::virtual_01` and
`Vector2u8::virtual_00` — an **accessor block**. And the first one gives the
game away:

```arm
func_02000C6C:
    ldr  r0, [r0, #8]
    ldrh r0, [r0, #0x6c]      ; <- the unlock slot, from docs/22
    bx   lr
```

`+0x6C` is `CollectableItemBlueprint`'s unlock slot. So `r0+8` is a
**`CollectableItemBlueprint`**, not a `UnitBlueprint` — these are
`Sim::CollectableItem` accessors.

### The pitfall: `Entity+0x08` is not one type

This is the limitation of the idiom search, and it is worth stating plainly
because it will recur. `docs/25` established that every entity holds its
blueprint at `+0x08` — but `Sim::Entity` has **three** subclasses, and each
points at a *different* record type:

| entity | blueprint at `+8` | stride |
| --- | --- | ---: |
| `Sim::Unit` | `UnitBlueprint` | `0x7C` |
| `Sim::Projectile` | `ProjectileBlueprint` | `0x74` |
| `Sim::CollectableItem` | `CollectableItemBlueprint` | `0x70` |

So **a displacement search over the idiom silently mixes three layouts.**
`UnitBlueprint+0x60` is build time, a `u16`; `CollectableItemBlueprint+0x60` is
the reference field, a `u32`. Same offset, same idiom, unrelated meanings.

The tell is available and was ignored the first time: **the access width**.
The build-time readers use `ldrh`; the accessor block uses `ldr`. `docs/26`'s
own control run showed `+0x5C..+0x6B` being read **byte-wise** by
`func_02053F90` and `func_02054438` and correctly called that
`ProjectileBlueprint`'s flag block — the same reasoning applied to the `0x02000C`
functions would have caught this immediately.

**Rule going forward: an idiom hit is only attributable to a blueprint type
once the access width matches that type's field, or the function's class is
known.** `lb_field.py` prints the mnemonic per displacement in `--profile`
mode for exactly this reason.

## The field

`+0x60`, a `u32`, `0xFFFFFFFF` when it does not apply.

| category | count | `+0x60` means | range |
| --- | ---: | --- | --- |
| Minifig | 85 | **global blueprint index of the unit it depicts** | 0–181 |
| Minikit | 90 | **global blueprint index of the Minifig awarded** for that act's full set | 18 distinct |
| Hint | 21 | a `LOC` string id — the hint text | 2878–2898 |
| Map | 30 | a `LOC` string id — the map's display name | 1242–1300 |
| ConceptArt | 66 | index into the table at `0x021269A4`, stride 8 | 0–65 |
| RedBrick | 18 | index into the table at `0x02125FF4`, stride 16 | 0–17 |
| HealthPowerup, ManaPowerup, Stud, Ticket, MissionObject | 32 | — | `0xFFFFFFFF` |

342 records, every one accounted for.

The dispatch is visible in the code too. `func_02000C94` is a two-arm switch
that reads `+0x60` **or** `+0x64` depending on its second argument and passes
either to the same lookup, `func_02001D74` — which is three instructions of
`table[index]` over a pointer array. One field, read through different
interpretations, is exactly what the data says.

## Minifig → unit: 85 of 85

Taking `+0x60` as an index into the 551 blueprint records in file order, and
comparing the resolved name against the minifig's own name with the `Minifig_`
prefix stripped: **84 exact matches out of 85.**

The single mismatch is a **typo in the game's data**, not a failure of the
reading: `Minifig_E_SigeThree` — missing the second `e` in "Siege" — resolves
to `E_SiegeThree`. The index is right; the name string is misspelled. That is
85 of 85 semantically, and finding a shipped typo this way is a decent sign the
mapping is real.

## Minikit → the reward, and the six bonus races

The 90 minikits take only **18** distinct values, five per value on average,
and the grouping is exactly six campaigns × three acts. Resolved:

| campaign | act 1 | act 2 | act 3 |
| --- | --- | --- | --- |
| King | `Minifig_Dwarf` | `Minifig_DwarfMineDefender` | `Minifig_DwarfKing` |
| Wizard | `Minifig_Troll` | `Minifig_TrollBattleWheel` | `Minifig_TrollKing` |
| Pirate | `Minifig_Islander` | `Minifig_IslanderWarCanoe` | `Minifig_IslanderChief` |
| Imperial | `Minifig_Ninja` | `Minifig_WarJunk` | `Minifig_NinjaKing` |
| Earth | `Minifig_SpacePolice` | `Minifig_SpacePoliceShip` | `Minifig_SpacePoliceCaptain` |
| Alien | `Minifig_SpaceCriminal` | `Minifig_SpaceCriminalShip` | `Minifig_SpaceCriminalLeader` |

**Collecting an act's full minikit set awards a minifig**, and the pattern is
rigid: act 1 gives the bonus race's basic infantry, act 2 its vehicle, act 3 its
leader. Six bonus races — Dwarves, Trolls, Islanders, Ninjas, Space Police,
Space Criminals — one per campaign.

Two independent checks pass. The **per-group counts are 5/6/4, 6/5/4, 5/5/5,
5/5/5, 6/5/4, 5/5/5**, matching `docs/22`'s minikit distribution — derived there
from names and cross-checked against the `LOC` achievement strings — exactly.
And all 18 targets are distinct, and all 18 are in the Minifig category.

This also closes a loose end from `docs/26`. The 16 Siege-category units that
read `12.0` at `+0x10` while the buildable siege line reads `0.3333` include
`TrollBattleWheel`, `IslanderWarCanoe`, `SpacePoliceShip`, `WarJunk` and
`DwarfMineDefender` — **they are the bonus-race units**, not part of any
faction's build tree. The `+0x10` split separates the standard buildable siege
and naval line from everything else, which is a more coherent story than the
category boundary was.

## The red brick table — 18 cheats, named

`0x02125FF4`, stride 16: `{ u32 index, char *, u32 loc_string_id, u32 }`.
Indexed by a RedBrick's `+0x60`:

| # | blueprint | display name |
| ---: | --- | --- |
| 0 | `RedBrick_TurnOffFow` | Reveal Map |
| 1 | `RedBrick_OneHitKill` | One Hit Kill (Heroes) |
| 2 | `RedBrick_FastProduction` | Fast Building |
| 3 | `RedBrick_QuickMagic` | Fast Magic |
| 4 | `RedBrick_DoubleStuds` | Double LEGO Bricks |
| 5 | `RedBrick_RevealMinikit` | Show Minikit |
| 6 | `RedBrick_RevealCoins` | Show LEGO Studs |
| 7 | `RedBrick_Regeneration` | Regenerating Health |
| 8 | `RedBrick_IncreaseSpellRange` | Long Range Magic |
| 9 | `RedBrick_IncreaseSpellTime` | Super Magic |
| 10 | `RedBrick_RevealRedBrick` | Show Red Bricks |
| 11 | `RedBrick_AutoUpgrade` | Upgraded Towers |
| 12 | `RedBrick_SuperExplosions` | Super Explosions |
| 13 | `RedBrick_FastHarvest` | Fast Harvesting |
| 14 | `RedBrick_FullAllowance` | Full Unit Cap |
| 15 | `RedBrick_InvincibleHero` | Invincible Hero |
| 16 | `RedBrick_FastMining` | Fast Mining |
| 17 | `RedBrick_EnemiesOnMinimap` | Show Enemies |

Every internal name matches its display text semantically —
`TurnOffFow` is "Reveal Map" (fog of war), `FullAllowance` is "Full Unit Cap",
`AutoUpgrade` is "Upgraded Towers".

**The `LOC` ids are not in table order** — rows 10, 11 and 12 read 2814, 2812,
2813. That is what rules out coincidence: an accidental alignment between two
lists would have to be monotonic, and this one is not.

The ConceptArt table at `0x021269A4` has the simpler shape
`{ u32 index, char *name }` and holds asset names — `S_Character_1`,
`S_Character_2`, ….

## Where the red bricks take effect — partly

`docs/25` found that `Sim::ConstructProgressAction` and
`Sim::RepairStructureAction` **halve build time** when a byte read from the
game-settings object (`func_020092A8`) at `+0x14` is set. With "Fast Building"
now confirmed as a red brick, that is very likely the flag it sets.

Scanning every call to `func_020092A8` and the load that follows it, the
settings object has **32 distinct byte flags** read across the ARM9, at `+0x01`
through `+0x20`. Several are clearly attributable:

| flag | read by |
| --- | --- |
| `+0x02`, `+0x13`, `+0x14` | `ConstructProgressAction`, `RepairStructureAction`, `ConstructStructureEntityCommand`, `ConstructMultipleEntityCommand` |
| `+0x10` | `ConstructStructureEntityCommand::virtual_09` |
| `+0x0C` | `User::WorldViewTopScreen::virtual_09` |

> **Resolved by `docs/28`.** All 18 mappings are recovered, and the caution
> below was justified: the id order is *not* the flag order, so the obvious
> guess would have been wrong on five of eighteen.

**The red-brick-index → flag-offset mapping is not established, and is not
being guessed.** The obvious candidate — flags starting at `+0x12` so that
index 2 lands on the build-time flag at `+0x14` — fails its own cross-check:
`+0x13` would then be `OneHitKill`, yet it is read by eighteen functions, all
construction-related. So the block is not a straight image of the red brick
order, and the mapping stays open.

`func_02007F3C` is the way in and is the next target: a **155-arm jump table**
(`cmp r0, #0x9A`) dispatching on a rule id, with nearly every settings byte read
from one of its arms. Walking that table yields the whole rule enum in one
pass. Note for whoever does it: the arms disassemble as a run of nonsense
`lsls` instructions, because they are the half-word jump table being decoded as
Thumb code.
