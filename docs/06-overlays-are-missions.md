# Every overlay is one mission

There are **91 ARM9 overlays** and **90 campaign missions** — six campaigns of
fifteen. `ov001`…`ov090` are the mission scripts, one each, and `ov000` is
something else.

That accounting is exact and it is the reason the overlays can be set aside
early: they hold no engine code. The third-party decomp's `docs/overlays.md`
states the same conclusion from the other direction —

> In Lego Battles, each overlay contains the logic for a mission. The logic
> consists of a function that handles events (a single number) and sets up
> objectives based on it.

— and its author recovered 64 of the mappings by emulating the Thumb code and
reading registers at known points. The remaining 26 were not recovered because
"the wizard's campaign loading functions are all in ARM" and the emulator used
only handled Thumb.

## The map-name scheme

Map files are `<world><side><act>_<mission>.map`:

| prefix | world | side | missions in `Maps/` |
| --- | --- | --- | ---: |
| `ck` | Castle | King | 15 |
| `cw` | Castle | Wizard | 15 |
| `pi` | Pirates | one side | 16 |
| `pp` | Pirates | the other | 15 |
| `ma` | Space | one side | 15 |
| `mh` | Space | the other | 15 |
| `mp` | — | multiplayer / skirmish | 30 |
| `test` | — | `test1.map` | 1 |

Six campaigns × 15 = 90, matching the overlay count exactly. `pi` has one map
more than its campaign needs; which file is the spare is not yet determined.

The mission names recovered for each prefix make the world assignment certain —
`ma` and `mh` are the Space campaigns ("Impact!", "CryoLab Down", "Exodus";
"The Red Planet", "Mothership Connection", "The Motherlode"), `pi` and `pp` the
Pirates campaigns ("Royal Commission", "Mutiny on Brickbeard's Bounty"; "Put to
Sea", "Adrift", "Hidden Treasure"), `ck` and `cw` the Castle campaigns.

**Which side of each pair is which is not yet established from the ROM.** The
letters are suggestive — `ck` reads as Castle-King, `pp` as Pirates-Pirates,
`pi` as Pirates-Imperial — but the mission titles cut both ways ("Search For
Scum" under `mh` reads as law enforcement, not as the alien side), and this
project does not record a guess as a fact. `LOC/*.lng` names the campaigns and
will settle it.

## Act and mission are not the file order

The recovered table shows the overlay ID, the map, and the act/mission position
the game presents. They disagree constantly: `ov052` is `ck1_3` but is presented
as act 1, mission **4**, while `ov055` is `ck1_4` presented as act 1, mission
**3**. So the campaign's presentation order is data the loader holds, not
something derivable from the file names. That table is somewhere in the ARM9 and
is worth finding.

## What is known, by overlay

Reproduced from the third-party project's `docs/overlays.md` (CC0). IDs `65`–`90`
and `00` are **not** recovered there, and are left blank here rather than filled
in by inference.

| ID | mission | map | act | # |
| --- | --- | --- | --- | --- |
| `01` | Criminal Minds | `ma2_1` | 2 | 1 |
| `02` | Running on Empty | `ma3_1` | 3 | 1 |
| `03` | Impact! | `ma1_1` | 1 | 1 |
| `04` | CryoLab Down | `ma2_2` | 2 | 2 |
| `05` | Guidance | `ma3_2` | 3 | 2 |
| `06` | Shock and Awe | `ma1_3` | 1 | 3 |
| `07` | Hotwired | `ma2_3` | 2 | 3 |
| `08` | Freedom | `ma3_4` | 3 | 3 |
| `09` | Taken | `ma1_4` | 1 | 2 |
| `10` | Supply Lines | `ma2_5` | 2 | 4 |
| `11` | Feed The Fire | `ma3_5` | 3 | 4 |
| `12` | Captured | `ma1_6` | 1 | 4 |
| `13` | The Final Countdown | `ma3_6` | 3 | 5 |
| `14` | Missing in Action | `ma2_6` | 2 | 5 |
| `15` | Exodus | `ma1_7` | 1 | 5 |
| `16` | The Red Planet | `mh1_1` | 1 | 1 |
| `17` | A Hard Place | `mh2_1` | 2 | 1 |
| `18` | Ancient Treasure | `mh3_2` | 3 | 1 |
| `19` | Something's Out There | `mh1_2` | 1 | 2 |
| `20` | Drop Zone | `mh3_3` | 3 | 2 |
| `21` | Search For Scum | `mh2_3` | 2 | 3 |
| `22` | A Purple Lode | `mh1_3` | 1 | 3 |
| `23` | Purple Haze | `mh2_4` | 2 | 2 |
| `24` | Stranded | `mh2_5` | 2 | 4 |
| `25` | Head Start | `mh3_5` | 3 | 3 |
| `26` | Purple is the New Green | `mh1_5` | 1 | 5 |
| `27` | Mothership Connection | `mh3_6` | 3 | 4 |
| `28` | Retaliation | `mh1_6` | 1 | 4 |
| `29` | The Motherlode | `mh2_6` | 2 | 5 |
| `30` | Orders from Above | `mh1_7` | 1 | 6 |
| `31` | Monkey Business | `pi3_1` | 3 | 1 |
| `32` | The Prisoner | `pi2_1` | 2 | 1 |
| `33` | Royal Commission | `pi1_1` | 1 | 1 |
| `34` | Royal Territory | `pi3_2` | 3 | 2 |
| `35` | Sitting Ducks | `pi2_2` | 2 | 2 |
| `36` | The Canary | `pi1_2` | 1 | 2 |
| `37` | Abandoned Outposts | `pi2_3` | 2 | 3 |
| `38` | Raiding Party | `pi1_3` | 1 | 3 |
| `39` | Sneak Attack | `pi3_3` | 3 | 3 |
| `40` | Sabotage | `pi1_4` | 1 | 4 |
| `41` | Temple Raid | `pi2_4` | 2 | 4 |
| `42` | All Mine | `pi3_6` | 3 | 4 |
| `43` | Mutiny on Brickbeard's Bounty | `pi2_5` | 2 | 5 |
| `44` | Precious Cargo | `pi1_5` | 1 | 5 |
| `45` | End Game | `pi3_5` | 3 | 5 |
| `46` | The Maze | `ck3_1` | 3 | 1 |
| `47` | Dark Shadows | `ck1_1` | 1 | 1 |
| `48` | Old Allies | `ck2_1` | 2 | 1 |
| `49` | War Machines | `ck2_2` | 2 | 2 |
| `50` | Costly Distractions | `ck1_2` | 1 | 2 |
| `51` | Grave Mission | `ck3_2` | 3 | 2 |
| `52` | 1000 Tree Woods | `ck1_3` | 1 | 4 |
| `53` | Perilous Journey | `ck3_3` | 3 | 3 |
| `54` | Stand Fast | `ck2_3` | 2 | 3 |
| `55` | The Great Library | `ck1_4` | 1 | 3 |
| `56` | Confrontation | `ck3_4` | 3 | 4 |
| `57` | Knowledge Of The Ancients | `ck2_4` | 2 | 4 |
| `58` | Forgotten Passage | `ck2_5` | 2 | 5 |
| `59` | The Barren Wasteland | `ck2_6` | 2 | 6 |
| `60` | Bridge the Gap | `ck1_6` | 1 | 5 |
| `61` | Put to Sea | `pp1_1` | 1 | 1 |
| `62` | Adrift | `pp3_1` | 3 | 1 |
| `63` | Hidden Treasure | `pp2_1` | 2 | 1 |
| `64` | Outpost Offensive | `pp1_2` | 1 | 2 |
| `65`–`90` | **not recovered** — the rest of `pp` and all of `cw` | | | |

## Why this project can finish the table cheaply

The gap exists because the recovery method was a Thumb-only emulator and the
missing loaders are ARM. Neither Ghidra nor a static recompiler cares about the
instruction set, so the same information falls out of either track without
special effort — and once `ov000`'s role is known, the overlay dimension of this
game is closed.

The `ov000` question is separate and small: `0x350` bytes, four functions, at
`0x0216A2E0` — immediately after the ARM9 static module's `.bss`. Being first
and being adjacent to the static image both suggest something structural rather
than a mission.
