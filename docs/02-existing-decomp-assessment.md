# Assessment of the third-party decompilation

`Lego-Battles-Decomp-master` (a local copy of the upstream repository), snapshot
dated 2025-10-20. A hand-written matching decompilation targeting the European
ROM, forked from [the Phantom Hourglass decomp](https://github.com/zeldaret/ph)
as a template, built on
[`ds-decomp` (dsd)](https://github.com/AetiasHax/ds-decomp),
[`ds-rom`](https://github.com/AetiasHax/ds-rom) and the Metrowerks `mwccarm`
compiler.

## What it has

The **delinking configuration is complete and correct**, and that is not a small
thing. `config/eur/` describes:

- the ARM9 static module's six sections, with exact boundaries;
- the ITCM and DTCM autoloads;
- all 91 ARM9 overlays, each with its own delink, symbol and relocation files;
- a content hash per module, so a wrong ROM is caught rather than silently
  mis-analysed.

Its section boundaries were checked independently by this project. `tools/nds_extract.py`
BLZ-decompresses the ARM9 and splits it by the SDK `MODULE_PARAMS` structure,
with no knowledge of that config, and produces:

| section | address | size | dsd config says |
| --- | --- | --- | --- |
| static | `0x02000000` | `0x150880` | `.text`…`.data` end `0x02150880` ✔ |
| static `.bss` | `0x02150880`–`0x0216C068` | | `.bss start:0x02150880 end:0x0216c068` ✔ |
| ITCM | `0x01FF8000` | `0x320` | ✔ |
| DTCM | `0x027E0000` | `0x60` (+`0x20` bss) | ✔ |

Two independent derivations agreeing exactly is the strongest evidence available
that both are right. **This configuration is trusted and is being reused.**

It also carries a **function corpus**: 9,158 functions in the ARM9 static module
and 364 across the overlays, each with an address, a size, and — critically —
its ARM/Thumb mode. The LSW2 playbook is emphatic that recovering that mode by
guessing at prologue bytes is wrong four times in 5,860, and every error lifts
to something entirely wrong. Having it recorded is worth real time.

## What it does not have

| measure | value |
| ---: | --- |
| ARM9 static functions catalogued | 9,158 |
| overlay functions catalogued | 364 |
| functions given a meaningful name | **45** (0.5%) |
| overlays with any symbol naming | **0** |
| `.cpp` files written | 20 |
| lines of C++ across all of them | **834** |
| source files claimed in `delinks.txt` | 12 |

And the 834 lines overstate it. `src/Battles/Map/Map.cpp` is representative:
`Load()` is a real dispatcher, correctly recovered, and every function it
dispatches to is an empty stub:

```cpp
u8 *Battles::Map::LoadTerrain(u8 *data) {
    return data;
}
```

Nine of that file's fourteen functions are stubs of that kind. The project is
scaffolding plus a first exploratory sitting, honestly labelled as work in
progress by its own README.

## Two concrete defects found

1. **`lb_usa.sha1` does not describe LEGO Battles.** It still contains
   `4c8f52dd719918bbcd46e73a8bae8628139c1b85 ph_usa.nds` from the Phantom
   Hourglass template. There is no USA configuration at all; `config/` has only
   `eur/`. The correct USA hash is `f25d090feb840e9b4b07546b6929d897e3fb3965`.
2. **`config/eur/arm9/delinks.txt` claims 12 source files**, but only the
   sections actually delinked are described; the vast majority of the ARM9 is
   one undivided `.text` run. That is expected at this stage and is noted so it
   is not mistaken for a file-level split that already exists.

## What was learned from it that is worth keeping

Beyond the configuration, its 45 names are real reverse engineering and they
point at structure:

- `Battles::Map::{OnLoad, Load, LoadTerrain, LoadMines, LoadEvents, LoadTriggers,
  LoadMarkers, GetEventWithId, GetMarkers, LoadMarker, LoadTriggerTeam}` —
  and the string constants beside them named the **map chunk tags**: `TERR`,
  `EVNT`, `TRIG`, `MINE`, `MARK`, and `PAM`.

  That was enough to confirm the container layout in one step. A decoded
  `Maps/ck1_1.map` is:

  ```
  0x00000  "MAP"          3-byte magic
  0x00003  "TERR" …       terrain
  0x05118  "EVNT" …       events
  0x05321  "TRIG" …       triggers
  0x05343  "MARK" …       markers
  0x053C3  "MINE" …       mines
  0x053D0  "PAM"          3-byte terminator — the magic, reversed
  0x053D3                 end of file
  ```

  Chunk lengths are implicit: each loader consumes what it needs and returns the
  pointer past it, which is why every `Load*` in that file returns `u8 *`.
- `Battles::Save::{SaveGame, BitStream, SaveHasher, PlayerData, ConquestData}` —
  the save system is bit-packed and hashed.
- `SetTeamEnemy`, `SetTeamFriendly`, `SetTeamNeutral`, `SetTeamStartingBricks`,
  `TeamManager` — team relations are a small, findable module.
- `MissionObjectives`, `ObjectiveBase`, `DestroyMarkedUnitsObjective`,
  `RetrieveMarkedUnitsObjective`, `TriggerAreaObjective`,
  `CampaignMissionBase` — mission objectives are a class hierarchy with at least
  three concrete objective types.

## Verdict

**Adopt the configuration; do not wait on the decompilation.**

The delinking config, the section boundaries, the 9,522 function extents with
modes, and the 45 names are all directly useful and are being imported. At
0.5% after its first sitting, the hand-decompilation is not a path to a running
port on any timescale that matters, and it does not need to be — see
`03-tooling-decision.md`. The matching track is worth resuming *later*, against a
build that already runs, which is a far easier problem than doing it first.

There is no licence obstacle: the repository is released under **CC0 1.0**, a
public-domain dedication. Attribution is recorded in
`docs/reference/attribution.md` regardless, because knowing where a fact came
from is worth more than the licence requires.
