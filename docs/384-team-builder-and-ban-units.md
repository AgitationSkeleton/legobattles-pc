# 384 — Team Builder and Ban Units

Status: **implemented and verified live** (src/lb_teams.cpp, the two tabs in
lb_overlay.cpp, three hooks in lb_patches.cpp, lb_demo.cpp, and apply_hooks
stanza 0zzzzzzzzzzzz). These are the Team Builder and Ban Units features,
specified on 09-14 (commit 356cf6f7) and not started until now.

## The specification, as checkable requirements

**Team Builder**
1. A new imgui tab, **Team Builder**.
2. A save/load preset dropdown, with a **Randomize** button beside it.
3. A grid of character icons **derived from the game ROM**, mimicking the
   Free Play army screen's layout and its subfolders (heroes, builders, ...).
4. The default roster is **King**, Free Play's own default.
5. A save holding a non-default team appears in the dropdown as **"Save File
   Team"**.
6. **Set Team** and **Set Enemy Team** buttons.
7. Set Team applies immediately only when the player is not in gameplay; the
   pause menu counts as gameplay. It never applies while connected to a lobby
   or while viewing the master server.
8. The Free Play enemy selector gains a **seventh option, Custom**, and an
   **eighth, Random**: each category shuffled independently.

**Ban Units**
1. A second new imgui tab, **Ban Units**, toggling each unit coloured/greyed.
2. A banned pick is not refused at selection time: the offending player is
   **switched to King as the match starts**.
3. **The King team cannot be banned, as it is the default.**

## What the cartridge does (research, 2026-09-26)

It was traced statically. Every address below was checked
against `out/eur/arm9/section_static_02000000.bin` (T = Thumb, A = ARM). The
port's ROM is SHA-pinned (`port/game.toml`), so EUR addresses are exact.

### The Free Play army screen (`Battles::User::CustomizeArmy`)

**Flow.** SkirmishSetup → `FUN_020B83D4` → CustomizeArmy → commit
`FUN_020B8EB4` → OpponentSetup → Start `FUN_020C0EB0`.

**Layout** (`UI/FE/CustomizeArmy.bin`, rects in native px). Nine slot buttons
in a panel on the left form the "subfolders". There are no tabs and no
category labels. Beside them is a 5×5 grid of candidates, cell k at
(112 + 28·(k%5), 11 + 28·(k/5)).

| slot | panel x,y | army field | the game's name for it (LOC) |
|---|---|---|---|
| Hero | 36,11 | +0x08 | 245 |
| Builder | 20,40 | +0x0A | 246 |
| Melee | 52,40 | +0x0C | 242 "Close Combat" |
| Ranged | 20,69 | +0x0E | 243 |
| Mounted | 52,69 | +0x10 | 244 |
| Siege One | 20,98 | +0x00 | 248 |
| Siege Two | 52,98 | +0x02 | 249 |
| Siege Three | 20,127 | +0x04 | 250 |
| Transport | 52,127 | +0x06 | 251 |

**Candidates** (`FUN_020B96FC`, T). Every minifig collectable (blueprints
356..440, 85 of them), in minifig order, whose unit belongs to the slot: the
unit's category for slots 0-4, and `FUN_020EB21C`'s tier for the sieges and
the transport. The counts are:

| Hero | Builder | Melee | Ranged | Mounted | Siege 1 | Siege 2 | Siege 3 | Transport |
|---|---|---|---|---|---|---|---|---|
| 25 | 6 | 8 | 10 | 6 | 6 | 12 | 6 | 6 |

The grid omits the unit currently in the slot, and a pick swaps it in.

**Unlocks decide how a unit looks, not whether it is listed.** The manager is
`*0x021585A8`, and a minifig's bit is its record's +0x6C (= minifig − 260).
* **Unlocked:** its +0x6B is 0, or its bit is set in the BitArray at mgr+4.
* **Bought:** its bit is set in the BitArray at mgr+0x34.
* `BitArray::Get` is 020EE9A0: `data = [ba+0xC]`, then bit i.

A unit that is not unlocked shows its faction's emblem. One unlocked but not
bought is drawn translucent (alpha 10/31). Only a bought unit can be picked.
A new profile buys the nine King minifigs (`FUN_020EB554`).

**The army record.** 0x02154E40, 20 bytes:
* the nine picks as u16 blueprint indices: siege 1-3 and transport at +0,
  then hero, builder, melee, ranged and mounted at +8;
* +0x12 "confirmed" flag;
* +0x13 base faction, which the player's army always has as 0x7C.

The King default is `06 07 08 09 | 00 02 03 04 05 | 01 | 7C`. Only record 0
exists: "records 1 and 2" alias live profile fields that the save writes
(0x02154E54 is profile +0x4C), so they must never be written. The commit
sets +0x12 on every Continue, so **the flag does not mean non-default**
(docs/23 corrected). "Save File Team" is flag set **and** picks unlike
King's.

**Icons.** The army screen draws `UI/MiniHeads` (4bpp NCGR, 16×10 heads of
24×24) with the palette `UI/WorldViewTop_Back.NCLR`.
* A unit's head position p is its blueprint's index in the u32 key table at
  0x0214E4E0 (count at 0x0214E41C; `FUN_020E0784`).
* Its sub-palette is byte +0x0F of the first frame of `UI/FE/UIMgrData.bin`
  record 0x252+p (`FUN_020E06FC`).
* The files are COMP-wrapped LZ11. The whole ROM is already in host memory
  (`main.cpp`'s `rom` vector).

### The Free Play enemy selector (`OpponentSetup`)

It is one 56×56 button (widget 0xFC, arrows 0xFD/0xFE).
* **Its state** is the button's frame index, a u16 at `[[this+0x60]+4]+0x24`,
  0x38..0x3D.
* **The cycler** `FUN_020C0E7C` (T; r0 this, r1 delta) wraps mod 6.
* **The mapper** `FUN_020C0374` (T; one caller, the Start handler) maps it to
  factions 124..129.

**The game half-built a custom opponent.** Its choice table (0x02128304) has
nine entries, the last three "Custom Army 1/2/3" = factions 132/133/134, but
the list is cut to six. The Start handler already builds a custom opponent:
it copies army record `id − 132` onto its stack and calls the roster builder
`FUN_02000E78` (A).

The AI reads its faction only through the roster (`FUN_020858F4`). No AI
table is indexed by faction. The atlas even has an empty seventh frame.

### Multiplayer armies

**Every player's multiplayer army is a custom army.** It is picked on
CustomizeArmy in mode 1 and stored as record 0. Each player's army travels in
the lobby's data sharing, and every machine holds all three at 0x0215B814 +
i·0x2C.

At Start, each machine rebuilds each player's roster in `FUN_020F5168`'s
start branch. A 20-byte record is built on its stack, and then
`BL FUN_02000E78` runs at 0x020F5428 (lr 0x020F542C). This is the one point
where every machine makes the same decision from the same data.

Nothing in the port carries host-decided settings to clients yet. The
panel's "host-decided" labels promise more than the code does.

## The plan

1. **ROM access.** apply_hooks hands `main.cpp`'s ROM vector to the port
   (`lb_rom_attach`). A new `lb_teams.cpp` reads NitroFS paths, unwraps COMP
   and LZ11, decodes the NCGR and NCLR, and builds the head atlas (plus a
   greyscale copy for banned units) as SDL textures. No cartridge data enters
   the repository.
2. **The unit table.** Each slot's candidates are listed in minifig order,
   from the game's own rule. Names come from the game's live LOC table
   (blueprint +0x06), head positions from the key table in guest RAM, and
   palettes from UIMgrData. Unlock state is read the way the game reads it.
3. **The Team Builder tab.** The game's panel and grid layout, with the
   preset dropdown (King default, "Save File Team", the player's own
   presets), Load/Save/Delete, Randomize, Set Team and Set Enemy Team.
   * **Set Team** writes record 0 (flag 1, base 0x7C). It is refused:
     - during gameplay (the battle view exists, pause included);
     - while the cartridge's network state is not idle (hosting, joining,
       the Join screen's master-server list, or a match);
     - while the army screen is open, since its own copies would overwrite
       it;
     - when a pick is not unlocked and bought, as the game refuses it.
   * **Set Enemy Team** stores the team in the port's settings.
4. **Custom and Random enemies.**
   * The cycler wraps mod 8. Custom and Random use two frames the screen
     already loads: the crown and the "?".
   * The mapper returns 133 for them and queues the army: the stored team,
     or a fresh per-slot shuffle.
   * A pre-hook on `FUN_02000E78`, from the Free Play call only, writes it
     into the Start handler's stack copy. Nothing is written to the alias.
   * The opponent is named after its builder's faction.
   * Recordings carry `enemy8=1` and each injected army, so they replay.
5. **Ban Units.**
   * A tab with the same icons, 85 units toggled coloured/greyed. The nine
     default King picks are not toggleable.
   * The host's ban list travels to clients in a new wireless-bridge datagram
     while hosting.
   * The pre-hook on `FUN_02000E78` from the multiplayer start
     (lr 0x020F542C) replaces an army holding a banned pick with the King
     default, on every machine, from the host's list: the host uses its own,
     a client the one it received.
   * The list is frozen while hosting.

## What was built

**`src/lb_teams.cpp`** (new) holds everything the tabs and hooks stand on:
* the ROM reader: NitroFS, COMP/LZ11, NCGR/NCLR;
* the head sheet, with a greyscale copy;
* the unit table, the army record, the unlock tests and Set Team's
  refusals;
* the Custom/Random army and the ban rules.

What each tab has:

* **Team Builder** (lb_overlay.cpp):
  - the preset dropdown: King (default), then Save File Team when the save's
    army is confirmed and not King's, then the player's presets;
  - Load, Randomize (each slot independently, from what this save can field),
    Save, Delete;
  - the army screen's slot panel and 5×5 grid at the game's own positions;
  - Set Team, and Set Enemy Team, with the current Custom opponent shown
    below.
* **Ban Units:**
  - the 85 units by slot, click to grey out;
  - the nine default King picks disabled, with "The King team cannot be
    banned, as it is the default.";
  - frozen while hosting or joined; a joined player sees the host's list.

The two tabs are stored in settings.json as `team_working`, `enemy_team`,
`team_presets` and `banned_units`.

**The hooks** (lb_patches.cpp; all pure tail calls unless they act):
* **`FUN_020C0E7C` (T), the selector's cycler.** It cycles mod 8, live and in
  recordings that say `enemy8=1`. For Custom and Random the button's rect is
  moved to the centred 24×24, because a button draws its frame at its rect's
  corner, and it is restored for the six portraits.
* **`FUN_020C0374` (T), the mapper.** Custom and Random answer 133 and queue
  their army. A live game builds the army and notes it as an `a` line in the
  recording; a replay takes the recorded one.
* **`FUN_02000E78` (A), the roster builder.** A pre-hook acts for two
  callers only:
  - the Free Play opponent's (lr 020C0FAE): the queued army goes into the
    Start handler's stack copy;
  - the multiplayer start's (lr 020F542C): bans.

**The bridge** (apply_hooks, wifi_net.cpp). A new datagram type, `LBRU`, is
sent every 250 ms while this instance hosts (lb_rules_outgoing), to the
siblings and the remote peer. It is intercepted before frame parsing, and a
player who is joining or joined keeps it (lb_rules_incoming). Rules older
than 3 s are not applied, so a lobby that was left cannot leak its list into
the next.

## Limits, stated

* **Locked units.** A unit the save has not unlocked is drawn as its grey head
  in the tab. The game shows the faction's emblem instead, from the UI_FE
  atlas, which the tab does not decode. Unlocked-but-not-bought is
  translucent, as in the game.
* **Set Team is written to RAM.** The cartridge writes it to the save file at
  its next save (a Free Play match ends with one). The port does not trigger
  a save itself: `FUN_0204FA6C` has not been called from the host.
* **Custom and Random use existing art.** The crown and the "?" are 24×24
  frames from the atlas the screen already has. No 56×56 portrait exists for
  them; the empty gold frame in `Iconic_FreeplayFaction` has no frame record.
* **The opponent is named after its builder's faction** (`+0x13`), because
  the Start handler takes the name from there.
* **Bans need the port on both ends.** A client on a build without this
  change ignores the rules. If a ban then applies, its rosters differ and the
  match desyncs. That is a broken match, not an advantage. The bridge holds
  one remote peer, so a third player over the network remains untested,
  though siblings on one machine all hear the host.
* **Only bans travel.** The other "host-decided" settings still have no
  channel. The `LBRU` datagram has a version byte and
  room for them.

## Three faults found by running it

1. **The minifig records are not at base + 4 + index·0x7C.** The runtime check
   said "DISAGREES" on the first run. Records are sized by their kind byte at
   +8: 0x7C for units, 0x74, and 0x70 for the minifig collectables. The unit
   table is now located by walking the records, and the check agrees for all
   85.
2. **The record the hooks rewrite is on the stack, and the stack is in
   DTCM.** A main-RAM pointer test refused it, so the first Custom match
   (t384c) fielded whatever the profile alias held. The lr test is what
   identifies the record, and the bus maps DTCM.
3. **A replay finished before its army was asked for.** Two replays of the
   t384f recording (rep384a/b) were identical to each other, but fielded King's
   default instead of the recorded Custom army. The recording's last timed
   event is the Start press (frame 4677). The Start handler asks for the army
   at 4680, and by then the replay was over, so the mapper built from the
   current settings. A replay now stays in Play until the frame after its last
   `a` line. This is the lesson of docs/383's replay fault again: two
   identical replays prove determinism, not fidelity.

## Verification

* **Team Builder at the main menu** (profile "Max" loaded, run t384b,
  `exp-t384b/win/teambuilder-vs-armyscreen.png`):
  - All 160 heads decoded from the player's ROM, and the unit table agrees
    with the blueprints in RAM.
  - The dropdown lists "Save File Team": the seeded save's army has the Space
    Criminal Leader.
  - Load showed it. Set Team wrote record 0, and Randomize then Set Team
    changed it: Space Police Captain, King's builder, Swordsman, Space
    Criminal, Gripley, Clipper, Starfighter, King's Dragon, Imperial
    transport.
  - The game's own army screen then opened on that team: its card reads
    "Space Police Captain". Its hero grid lists the same 24 heroes in the
    same order as the tab's.
  - Set Team was greyed there: "Close the game's army screen first."
* **Custom opponent** (run t384d; the team set to Space Criminal Leader,
  Alien builder, Troll, Islander, Skeleknight, Barracuda, Troll Blimp,
  Wizard's Dragon, Pirate transport):
  - Five right taps cycled Wizard → Pirate → Imperial → Earth → Alien →
    Custom, and Start built faction 133 from it ("army written").
  - The recording carries `enemy8=1` and the army's `a` line.
* **Random opponent** (run t384e). Its roster, read from the faction object
  after the start, is base 126 (Pirate, from the drawn builder), 17
  structures, then Queen, Pirate builder, Guardsman, Islander, Horseman,
  Clipper, Troll Giant, King's Dragon, Imperial transport. Every slot was
  drawn separately, and the roster equals the recorded `a` line.
* **Replays of a Custom match** (the t384f recording, rep384c/d, final
  build):
  - Both field the recorded opponent: hero 133, builder 102, base 129,
    "army written for faction id 133".
  - Both finish at frame 4681, the frame after the army line.
  - Their counts are identical at 2000, 3500, 4400 and 4675, and identical to
    the pre-fix replays, which diverged only after the Start.
* **The selector** (run t384f, `exp-t384f/win/selector.png`): Alien's
  portrait, then the crown (Custom), the "?" (Random), and the crown again
  after a left tap, centred between the arrows.
* **The Ban Units tab** (run t384g, `exp-t384g/win/ban-units-tab.png`):
  - Space Criminal Leader, Ghost and Troll Giant banned: greyed and
    translucent.
  - The nine King picks are disabled buttons, with the tooltip "The King team
    cannot be banned, as it is the default."
  - The slots carry the game's own names ("Close Combat" is LOC 242).
* **Bans, two instances** (lb_mp.py mp384, `mp-mp384/bans-both-king.png`):
  - The host banned 133, and both players' saves field the Space Criminal
    Leader.
  - The client logged "rules from instance 0: 1 unit(s) banned".
  - **Both machines** logged "player 0 switched to King at the start: banned
    Space Criminal Leader" and the same for player 1.
  - Both battles show King's castle and hero, and the harness's divergence
    check read **in sync** in all 5 samples of a moving match.
* **Gate** (the reference replay, which predates `enemy8`, so every new hook is a
  pure tail call):
  - exp-gate384b on the final build: counts identical to gate370b, the eight
    pictures under 5972 byte-identical to gate381, 59.92 fps.
  - exp-gate384 before the last replay fix gave the same, at 59.83 fps.
* **Vanilla** (van384): 240/240 identical.
