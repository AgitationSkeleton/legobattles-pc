# 477 — Reports after 0.4.14: black 3D units, the sprite pool, other armies' sounds, fog, the top-screen minimap

Written for: whoever works on these subsystems next.

Status: BR-0076/0077, BR-0073, BR-0067, BR-0063, SG-0094 and SG-0096 done; the match log and the sprite-pool and fog detectors are always on. BR-0075 is explained and scoped, not built. The exactness gate is identical: counts in Enhanced and DS Vanilla, and 480 frames.

## 1. BR-0076/0077: every re-skinned 3D custom unit was black

**Cause.** 0.4.14's fix for BR-0071 (`encode_model_texture`, lb_cunits.cpp) treated a format-4 model's palette as one palette of up to 256 colours.
- Every stock model's palette is **9 banks of 16 colours** (144 colours, 288 bytes; surveyed over all 31 models in `Models/`).
- Banks 0-4 and 6 are the team colours. They differ only in entries 12-14, the team ramp.
- Bank 5 is the white hit flash. Banks 7 and 8 are effect tints.
- The game picks a bank per team, and texture.png is drawn from bank 0.

The encoder wrote the picture's colours over bank 0 and zeroed every other bank, so any unit not using bank 0 was black.

**Fix.** Each colour of the picture takes the role of a stock colour:
- the bank-0 entry it equals;
- else the nearest entry that no team bank recolours.

In each bank that colour is the picture's own where the bank keeps the stock bank-0 colour, and the bank's substitute where it does not (team ramp, flash, tint). The exported texture's three team blues therefore stay team-coloured.

**Verified** by compiling `encode_model_texture` and `read_png` out of lb_cunits.cpp unchanged and running them on the reporter's Hwacha. All 9 banks decode correctly: the orange body in every team bank, the stripes in each team's colour, the flash white.

The unit lab's how-to now says: 16 colours at most, keep the team blues, PNG only (a `texture.tga` was never read; the reporter's TGA unit only looked right because it kept the stock texture).

## 2. BR-0073: a unit drawn from another unit's sprites

The report: a Wolf cycling through Santa's frames, then through building sprites. It came from a spectated three-army Free Play.

**The sprite pool.** The level load's sprite step (`FUN_020353B4`) seats each roster unit's sheets in a pool of **33 slots** (pool object `[0x02151DD8]` +4).

Each slot has a fixed kind (table `0x02127B28`): per army 11 slots, kinds `0 1 2 3 1 2 3 4 1 2 3`. That is:
- one hero sheet (kind 0, which the hero's ten direction/action sheets stream through);
- three three-sheet sets for categories 1-3;
- one kind-4 slot for the mounted unit and buildings.

`FUN_0202DD18` hands out the first free slot of the right kind, from the army's range on (Free Play sets `session+0x74`, the per-army mode). With no free slot it returns one shared overflow word (`0x02151DDC`).

`FUN_0202E014` then writes the unit's **page bytes**. They are on the blueprint's shared record, `[[e+0xE0]+4]+0x2E+4k`. It writes them by looking the sheet up in the 33 slots. An overflowed sheet is not there, so its pages stay **0**: slot 0, the first army's hero sheet. That is the Wolf showing Santa's frames: Santa was the first army's hero and his sheets stream through slot 0.

**When it overflows.** The pool holds 9 three-sheet sets. A stock army needs exactly 3, so three stock armies fill it to the slot. Two things can push the demand past 9:
- a creature in the Mounted or Hero slot (docs/438 loads it as a category-2 set: a fourth set for that army);
- a duplicate. In the per-army mode a sheet is never shared between armies: each army seats its own copy, though the page bytes are per blueprint, so only the last copy is ever drawn.

Run sp73e (Wolf in the player's Mounted slot, three armies) overflowed. Four page sets pointed at slot 0: the Wolf, and the Wizard ranged unit for both armies that fielded it. Slots 30-32 held the same three sheets as 19-21.

**Fix** (`creature_sprite_load_override`, the cloak on `0x02035F7C`): before the load, `sprite_sets_wanted` counts the rosters' sets. When it exceeds 9, the load runs with `session+0x74 = 0`: one search from slot 0, and a sheet already seated is reused. It is put back when the step returns.
- Heroes are never reused (`FUN_0202DD18` excludes kind 0).
- Buildings were reused already.
- The flag table this changes (`0x02151DE0`) is read by `FUN_0202DD18` alone.
- Matches that fit are untouched.

Run sp73f, the same set-up: "the armies want 10 sets", the overflow word empty, no wrong pages, 30-32 now a distinct set.

**Detectors** (always on, in the session log):
- `[lb_sprpool]`: slots used per army range, and whether the overflow word was used.
- `[lb_sprpage] army A blueprint B`: each roster blueprint whose page slots are of the wrong kind.

Neither changes anything.

## 3. BR-0067: hearing the third computer's actions far away

Every entity and spell sound goes through `FUN_0208938C`, the positional window: `FUN_02089230` and `FUN_02089270` both test it. Those are building, training, attacking and spells, and they are windowed for every army alike.

Interface sounds (archives 0 and 1) have no position. The game guards each one played for a team's event with a local-player test (`FUN_02086634` bricks, `FUN_0208382C`). The exception is **Sim::ResearchUpgradeEntityCommand**: `FUN_020735F4` and `FUN_02073640` play sound 0x11 (archive 1, index 1) for any team, so every army's "upgrade complete" chimes at the player. A third computer adds its own.

Measured with `LB_SNDSTART=1`, human idle, three armies, 240 s: three of those chimes with the setting off, none with it on.

**Fix**, setting **Hear Only Your Own Upgrades** (Settings > Music, on by default, `LB_OWN_RESEARCH_SOUND`): `research_step_override` on the command's vtable slot 0 (`0x02073544`, ARM). For another army's research, the step runs with that sound's byte in the started array set (`0x02155878 + 21`), which `FUN_02089AAC` reads as "already started this pass". The byte is put back when the step returns. Audio only, and off during a replay, so the gate and recordings are unchanged.

## 4. BR-0063: enemy specials seen in explored fog

The field draw keeps an entity when its vtable `+0xC` test passes.
- **Most units** use `Sim::Entity::virtual_03`: their footprint's four corner cells against the fog record's visible grid (`FUN_020A17C0`, `[0x021569E4]+0x14`).
- **Transport and Siege units** (`FUN_0205FBFC`: category 5 or 6) take their own branch in `Sim::Unit::virtual_03` (`0x0205FC1C`). It tests the four corners of the **model's box** (`+0xBC..+0xD0`) instead.

A dragon's, a ship's or a catapult's box reaches past the cell it stands on, so a special deep in fog shows as soon as a corner of its box is in a visible cell.

The observer `unit_draw_test_override` logs each other army's special that passes while its own cell is fogged (`[lb_fogleak]`, with the box), plus a per-minute count.

**Measured** in run fog63b (human idle, three armies, 7 minutes): up to 36 draws a minute of another army's special whose own cell was fogged. The boxes reach about one cell sideways and three cells up, for example `x -28..23 y -46..16`. Spectated runs reveal the map, so they show none.

**Fix**, setting **Hide Enemy Specials in Fog** (Settings > Display, on, `LB_HIDE_FOGGED_SPECIALS`): such a draw answers 0, which is the rule a one-cell unit's corner test amounts to.

The method's only caller is the field draw (`Task_View_DrawField`); the other `+0xC` call site, `User::InfoTab`, is on UI widgets. So the per-machine answer cannot reach the simulation.

## 5. SG-0094: the top-screen minimap in the level palette

The battle's top screen (`FUN_02025F50`, once a battle) copies the world's 16 minimap colours (`0x02127A8C + world*0x20`) into slot 9 of the palette objects flagged 0x10 (`FUN_02028E38`).

They are hand-picked colours, not tileset entries. FactionWars changes only the three tileset NCLRs: its ARM9 and every other file but `Entities.ebp`, the LOC and the sound data are vanilla. So a FactionWars Mars minimap stays red and green, even on a DS.

`minimap_follow` (lb_palette.cpp) moves each of indices 1-10 the way the palette in force moves the vanilla tileset colour nearest it, and marks the object dirty. It writes only over the vanilla colours or its own last write, so a mid-battle change of Level Palettes follows.

## 6. SG-0096: a custom unit unlocked with a stock unit

A top-level unit.json number, `"unlocked_with"` (the lab's Stats tab, "Unlocked with"), names a stock unit.
- `lb_teams_unit_state` answers for the custom unit with that unit's state: not unlocked, or fieldable. A custom unit needs no buying.
- The army screen's listable and tier answers (`lb_teams_card_listable`/`_tier`) leave it out while it is locked.
- The computer's armies are not limited.

## 7. BR-0075: custom sprites take only the game's unit colours

Every unit sprite is drawn from one palette file, `KingFaction.NCLR`: 16 banks, team x highlight.
- Entries 0-11 are the same LEGO colours in every bank.
- 12-14 are the team ramp, and 15 the outline or highlight.

`Unit_BuildBillboard` sets the bank as team x 2 + highlight (`e+0x30`). A custom sheet can therefore only use those 12 colours and the team ramp; a colour outside them takes the nearest.

Colours of its own would need extra banks in texture-palette memory, and the custom unit's billboards pointed at them. Where `e+0x30` becomes the polygon's palette base is not traced yet. That is the next step.

## 8. Also in this release

- SG-0093 (trained at) and SG-0095 (damage readout): docs/476.
- SG-0091 (the AI builds transports only when needed) and SG-0092 (no change, with why): docs/478.

## 9. The match log

Every battle now logs:
- `[lb_match] start: <kind>; map "<name>" (<stem>)`. The kind is online hosting, online joined, Free Play offline with the number of computer opponents and whether it is spectated, or a story mission.
- `[lb_match] local player handle N; team T id I; ...`.
- `[lb_match] end after Ns: <kind>; team T standing/defeated; ...`.

So a report's session log says what kind of match it was.
