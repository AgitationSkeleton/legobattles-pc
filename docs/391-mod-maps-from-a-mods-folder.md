# 391 — Mod maps from a mods folder (Free Play; multiplayer next)

Status: **Free Play implemented and verified; multiplayer not yet.** New
lb_mods.cpp; lb_patches.cpp, lb_overlay.cpp/.h, apply_hooks.py (main.cpp),
build.sh.

The design: mod ROMs (FUNLite, FactionWars, REbalance and so on) placed in a
`mods` folder have their non-vanilla maps appended automatically to the Free
Play and lobby map select. In multiplayer the custom maps are offered only when
both players have the mod; the map data could be sent to the other player as a
cache in their mods folder (`mods/cache`), so they need neither the ROM nor a
copy of it. The mods' maps are diffed against the game's stock multiplayer maps.

## What the mods hold

All eight mod ROMs were diffed against the retail ROM, with renders. docs/103
is corrected below.

* **The roster.** The mods do share one custom map roster, for the **MapDump** family. All four
  MapDump ROMs (VANILLA, FUNLite, FactionWars, REbalance) replace the same 18
  of the 30 multiplayer slots, byte for byte. Each replacement brings:
  * `Maps/mpNN.map`;
  * four minimap files at the root (`mpNNmini.NCGR/.NCBR`,
    `mpNNminiNT.NCGR/.NCBR`);
  * `BP/DetailTiles_mpNN.tbp`;
  * a new name in `LOC/American_English.lng`, id 1242 + 2(NN-1).

  The MapBase family changes no multiplayer map. REbalance's 14 changed maps
  are campaign `ck*` maps. The EUR and USA file systems are identical, so the
  maps are region-neutral.
* **Most are new maps.**
  * mp01 BetterPond and mp30 BetterRuthless remake their slots.
  * mp23 is the campaign map ma2_5, with its terrain identical and new
    events.
  * The other fifteen are new, and 11 of 18 change world.
  * Player counts differ from the slot's in 10 cases (mp28 has 3 starts in a
    4-player slot).
* **The map table.** It is at 0x02147DDC: 30 entries of 28 bytes (stem,
  players, name id, description id, 3, the Map_NN unlock blueprint, world).
  `FUN_02045E50` walks it once at boot: for each entry it builds a 0x168-byte
  mission and pushes it onto the registry at 0x021540E8.
  * The Free Play and multiplayer host lists (`SkirmishFactionSelect`, flag
    byte +0xA7) show every unlocked mission with mode < 3 and more than 2
    players.
  * Each label is `FUN_0203E1D4(name id)`.
* **The files.**
  * A chosen map loads from `"Maps/" + name + ".map"` (`FUN_02035FAC`).
  * Its previews come from `"%smini.NCGR"` and the like, its detail tiles
    from `"BP/DetailTiles_" + name + ".tbp"`.
  * Lookups are case-insensitive, first match in directory order
    (`FUN_02112060`, `FUN_021123C0`).
* **Multiplayer.** The host's settings packet carries only the map key (its
  name id). A joiner looks it up among its own missions, and writes through a
  null pointer when it is missing (`FUN_020F5168`).

## Free Play: what the port does

1. **The mods folder is read at launch** (`lb_mods.cpp`). The port scans
   `mods/*.nds` beside the game, reading only each ROM's tables and the
   files it needs. For every slot whose map differs from the player's own
   ROM it takes:
   * the map and its companions (a missing companion falls back to the
     stock one);
   * its name, with `_` shown as a space;
   * its player count, the distinct players of its EVNT records as the
     loader reads them (it agrees with the stock table 30 of 30);
   * its world, from the tileset the map names.

   Identical maps from several mods are one map, and the order is stable.
2. **Each map joins the cartridge's file system** as stem `z/NN`:
   `Maps/z/NN.map`, `z/NNmini.NCGR` ..., `BP/DetailTiles_z/NN.tbp`.
   * These are three new directories. Their files take new ids after the
     last one, so no stock file's id moves.
   * The FNT (with the three directories added) and the FAT (with entries
     appended) are rewritten into the padding after the last file (EUR has
     10 MB), the header is pointed at them, and its CRC is recomputed.
   * All of this happens after the ROM's SHA-1 check, and only when a mods
     folder has maps. `LB_NOMODS=1` skips it.
3. **Their names are strings of every `LOC/*.lng`.** The language files'
   FAT entries point at copies that carry two more strings per map (name,
   description) at ids 3064 and up.
   * The game's own fixup (`FUN_0203E0B4`) turns only the first 0xBF8 offsets
     into pointers: the count is the literal at 0x0203E0F8.
   * The port finishes the rest the same way once a frame
     (`mod_names_tick`).
4. **The game's own loop registers them** (`register_missions_override`).
   The entry to `FUN_02045E50` sets LR back to itself. When the stock pass
   returns there:
   * the first N entries of the table and their "mpNN" stems are overwritten
     with the mod maps ("z/NN", the same five bytes);
   * a frame like the function's own is laid on the stack, with the override
     as its return address;
   * the loop is entered at 0x02045E58 with r7 = 30 − N.

   The table and stems are then put back. The missions unlock with Map_01
   (The Pond's).
5. **The multiplayer host list does not offer them yet**
   (`map_list_fill_override`). While the list is built for multiplayer
   (+0xA7 = 1), the mod missions' +0x10 is 3, which the game's own filter
   rejects; for Free Play it is restored.

The Display tab shows what the folder gave ("18 mod map(s) from 3 ROM(s)").

## Verified

* **The scan** (run t391b; the mods folder held FactionWars_MapDump,
  REbalance_MapDump and FUNLite_MapBase):
  * 18 maps. The two MapDump ROMs' copies are one map each, and FUNLite's
    MapBase adds none.
  * Names, player counts and worlds are as the research found: Crater lake 2
    4 players, Pirate; Foxtrot Tunnels 3, Mars; and so on.
  * The file table went from 4,006 to 4,114 files (966,564 bytes added), and
    the registry holds 48 missions.
* **The Free Play list** (t391c): after the stock maps come BetterRuthless,
  Foxtrot Tunnels, Forest of Fate ..., each with its own preview and name.
* **A battle on Forest of Fate** started at 59.7 fps. The map object reads
  `z/16`, 96x96, KingTileset.
* **The multiplayer host list** (t391m) holds no mod map.

## Gate

The gate is defined without mods. With no mods folder the cartridge is
untouched and every new override is a pure tail call.

On the final binary:

* counts identical to gate370b;
* the t384f recording twice, identical to each other and to rep390a;
* pictures 8/8 identical to gate381 and Vanilla 240/240, by pixels;
* 59.83 fps.

The same binary with the mods folder back (run t391d) registered the 18 maps
again and listed BetterRuthless with its name and preview.

## Not done: multiplayer

The design calls for multiplayer to offer the mod maps only when both players
have them, perhaps by sending the map to the other player's `mods/cache`.
What that needs:

* **The host chooses first.** The host picks the map before anyone joins, so
  "both have it" can only be checked at the join. A joiner must hold the
  host's map at the same key before the settings packet reaches the game, or
  it writes through a null pointer (above).
* **Identity.** Each map is identified by its content hash (maps, previews,
  detail tiles). The host's lobby advertisement would carry the hashes of
  the maps it offers.
* **Transfer.** A map's files are 5–25 KB stored, under 200 KB for all 18. A
  joiner without the map fetches them over the port's bridge into
  `mods/cache/<hash>/`. They then have to be registered: at the next launch,
  or by adding the missions after boot.
* **Heap.** Registration allocates on the game heap at boot, so two players
  with different mod folders have different heaps. Players' heaps already
  differ through menu history and profiles, so this should not matter, but a
  two-instance match with one mods folder has not been run.

## Also

* **Per-Mod palettes** stay the ROM's own. A map shared by FactionWars and
  REbalance has no single palette of its own.
* **Limits.** At most 30 mod maps (one pass of the loop); more are left out.
  Names come from American_English (the mods edit only that file). The 32x32
  MirrorMatch has not been played at a wide window.

**docs/103 corrected in place:** the maps are in the MapDump family, and
REbalance's changed maps are campaign maps. MapDump adds 5 files (case
duplicates). The loader is located.
