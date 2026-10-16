# The hero spell table lives in the unit blueprint, at +0x72

`UnitBlueprint + 0x72` is a **five-byte array of spell ids**, one per hero spell
slot, with **0 meaning "empty"**. It is zero for all 182 non-hero units and
non-zero for all 25 heroes, which is what identifies it.

Every vanilla hero has **exactly four** non-zero entries in the five slots. That
fact comes from play knowledge of the game, and it is what falsified the earlier sidebar reading in
`docs/104` -- a three-element array cannot hold four spells.

## The ids

The factory is `func_0207BC0C`: it loads the id with `ldrb r5, [r7]`, bounds it
with `cmp r5, #0x22`, and dispatches through a 35-entry jump table at
`0x0207BC88`. Case 0 branches straight to the epilogue and constructs nothing,
which is why an empty slot reads 0.

| id | class | | id | class |
|---|---|---|---|---|
| `0x00` | (none) | | `0x12` | DamageSpell |
| `0x01` | HealSpell | | `0x13` | StatBufSpell |
| `0x02` | HealSpell | | `0x14` | DamageSpell |
| `0x03` | HealSpell | | `0x15` | DamageSpell |
| `0x04` | StatBufSpell | | `0x16` | DamageSpell |
| `0x05` | StatBufSpell | | `0x17` | DamageSpell |
| `0x06` | StatBufSpell | | `0x18` | StatBufSpell |
| `0x07` | StatBufSpell | | `0x19` | DamageSpell |
| `0x08` | StatBufSpell | | `0x1A` | TeleportSpell |
| `0x09` | StatBufSpell | | `0x1B` | EAttackSpell |
| `0x0A` | ForrestSpell | | `0x1C` | DamageSpell |
| `0x0B` | ForrestSpell | | `0x1D` | EAttackSpell |
| `0x0C` | ForrestSpell | | `0x1E` | TrackingSpell |
| `0x0D` | DamageSpell | | `0x1F` | DamageSpell |
| `0x0E` | ProjectileSpellFireBall | | `0x20` | HotWireSpell |
| `0x0F` | ProjectileSpellLightningBolt | | `0x21` | (none) |
| `0x10` | ProjectileSpellThunderHammer | | `0x22` | (none) |
| `0x11` | StatBufSpell | |  |  |

`DamageSpell` reaches the factory through two helpers, `func_02076AB8` (ids
0x0D, 0x17, 0x19, 0x1C, 0x1F) and `func_02076A70` (ids 0x12, 0x14, 0x15, 0x16),
rather than being constructed inline.

## Cross-check against play knowledge

Play knowledge says, independently of the ROM, that Space Criminal Leader has
speed boost, attack boost, hotwire and missile, and that Agent Chase shares
hotwire with him.

| hero | slots | classes |
|---|---|---|
| SpaceCriminalLeader | `00 05 09 20 1F` | StatBufSpell, StatBufSpell, HotWireSpell, DamageSpell |
| AgentChase | `00 05 07 20 0B` | StatBufSpell, StatBufSpell, HotWireSpell, ForrestSpell |

`0x20` is the only id they share and it is `HotWireSpell`. The two commons are
`StatBufSpell`, and the fourth is a `DamageSpell` -- the "missile". Four
independent predictions, four matches.

The common pool is `{0x05, 0x07, 0x09}`, all `StatBufSpell`: every hero takes
two of the three, which matches the first two spells being common ones such as
speed or defense.

## Vanilla loadouts

| hero | 0x72..0x76 | spells |
|---|---|---|
| K_King | `05 00 09 0A 0D` | StatBufSpell, StatBufSpell, ForrestSpell, DamageSpell |
| K_King_F | `05 00 09 0A 0D` | StatBufSpell, StatBufSpell, ForrestSpell, DamageSpell |
| W_Wizard | `07 00 05 0E 0F` | StatBufSpell, StatBufSpell, ProjectileSpellFireBall, ProjectileSpellLightningBolt |
| W_Wizard_F | `07 00 05 0E 0F` | StatBufSpell, StatBufSpell, ProjectileSpellFireBall, ProjectileSpellLightningBolt |
| P_Captain | `05 00 09 14 15` | StatBufSpell, StatBufSpell, DamageSpell, DamageSpell |
| P_Captain_F | `05 00 09 14 15` | StatBufSpell, StatBufSpell, DamageSpell, DamageSpell |
| I_Governor | `07 00 09 0C 17` | StatBufSpell, StatBufSpell, ForrestSpell, DamageSpell |
| I_Governor_F | `07 00 09 0C 17` | StatBufSpell, StatBufSpell, ForrestSpell, DamageSpell |
| E_Commander | `05 00 09 1B 1C` | StatBufSpell, StatBufSpell, EAttackSpell, DamageSpell |
| E_Commander_F | `05 00 09 1B 1C` | StatBufSpell, StatBufSpell, EAttackSpell, DamageSpell |
| A_AlienKing | `07 00 05 1D 0B` | StatBufSpell, StatBufSpell, EAttackSpell, ForrestSpell |
| A_AlienKing_F | `07 00 05 1D 0B` | StatBufSpell, StatBufSpell, EAttackSpell, ForrestSpell |
| SpacePoliceCaptain | `00 09 07 1E 1C` | StatBufSpell, StatBufSpell, TrackingSpell, DamageSpell |
| SpaceCriminalLeader | `00 05 09 20 1F` | StatBufSpell, StatBufSpell, HotWireSpell, DamageSpell |
| IslanderChief | `00 07 05 0C 16` | StatBufSpell, StatBufSpell, ForrestSpell, DamageSpell |
| NinjaKing | `00 05 07 1A 19` | StatBufSpell, StatBufSpell, TeleportSpell, DamageSpell |
| DwarfKing | `00 09 07 11 10` | StatBufSpell, StatBufSpell, StatBufSpell, ProjectileSpellThunderHammer |
| TrollKing | `00 07 05 13 12` | StatBufSpell, StatBufSpell, StatBufSpell, DamageSpell |
| ForestMan | `00 05 09 1E 19` | StatBufSpell, StatBufSpell, TrackingSpell, DamageSpell |
| Ghost | `00 09 07 0E 0D` | StatBufSpell, StatBufSpell, ProjectileSpellFireBall, DamageSpell |
| Sheriff | `00 07 05 1B 17` | StatBufSpell, StatBufSpell, EAttackSpell, DamageSpell |
| Conquistador | `00 09 07 11 15` | StatBufSpell, StatBufSpell, StatBufSpell, DamageSpell |
| AgentChase | `00 05 07 20 0B` | StatBufSpell, StatBufSpell, HotWireSpell, ForrestSpell |
| ClassicSpace | `00 07 09 1D 1C` | StatBufSpell, StatBufSpell, EAttackSpell, DamageSpell |
| Santa | `00 05 09 0A 1A` | StatBufSpell, StatBufSpell, ForrestSpell, TeleportSpell |

## The balance mods change spell loadouts, not only stats

These five bytes are inside the unit record, so the bundled `Entities.ebp`
deltas (`docs/101`, `tools/lb_genbalance.py`) **already carry them**. Selecting a
balance patch in the port changes who casts what, not just numbers.

| mod | heroes given a 5th spell | other |
|---|---|---|
| Rebalance | 13 (W_Wizard, W_Wizard_F, E_Commander, SpacePoliceCaptain...) | - |
| FactionWars | 0 () | 25 heroes lose spells |
| Funlite | 15 (W_Wizard, W_Wizard_F, P_Captain, P_Captain_F...) | - |

**Rebalance gives Kahuka (`IslanderChief`) a fifth spell**: `00 07 05 0C 16`
becomes `14 07 05 0C 16`, filling the empty slot with a `DamageSpell`. That
matches play knowledge of the romhack, and it is why the five-slot array exists.

**FactionWars empties nearly every hero's spell array** -- 21 heroes to zero
spells, three left with one. This is not a misread: in the same records it
halves hero health (1000 -> 500), cuts cost (500 -> 300) and build time (600 ->
480), all coherent values at the correct offsets, and the name string ids are
undisturbed. FactionWars turns heroes into cheap line units.

## Consequences for the hotkey work

A spell hotkey **cannot assume four slots**. It must read the live list: vanilla
gives 4, Rebalance and Funlite give some heroes 5, FactionWars gives most heroes
0. `docs/104` already requires reading the tab's own container for this reason;
this is the concrete case that proves it.
