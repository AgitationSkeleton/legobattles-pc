# 387 — Level Palettes, Force Tileset, and one faction's music always

Status: **implemented and verified live** (new lb_palette.cpp and
lb_palette_data.h from tools/lb_genpalette.py; lb_patches.cpp, lb_music.cpp,
lb_overlay.cpp, lb_demo.cpp, lb_teams.cpp, lb_widemap.cpp). Three features:

* a Display-tab **Level Palettes** option, [Per-Mod/Vanilla/Faction Wars/
  Random], default Per-Mod, that switches on the fly between the level colour
  palettes of Faction Wars and Vanilla (both baked in), or uses a mod's own
  palettes when its maps are loaded;
* a Display option **Force Tileset**, [Off/Castle/Pirates/Mars/Random],
  default Off, that overrides the level type the map would load;
* Wizard/Pirate/Imperial/Earth/Alien options for the music override, which
  always pick that faction regardless of the map.

The ROM was read twice, once for palettes and once for tilesets. Every address
below was checked against `out/eur/arm9/section_static_02000000.bin`.

## Music: Always <faction>

The Music tab's Mode gains six choices, Always King / Wizard / Pirate /
Imperial / Earth / Alien (modes 4–9). They write func_02089c38's side, the
same one-word override the other modes use, in every battle; menus keep their
own theme. King is included because "Vanilla" plays King's tracks only in Free
Play and multiplayer, and a campaign level has its own.

Verified (run t387m, a castle map, Always Pirate): the game's own SetMusicSide
picked King (0), the port substituted 2, and the music state read side 2,
kind 1 (the battle track).

## Level Palettes

### What a level's palette is

* **The files.** Each world has one palette: `KingTileset.NCLR`,
  `PirateTileset.NCLR`, `MarsTileset.NCLR`. Each is an uncompressed 552-byte
  RLCN holding 256 BGR555 colours at +0x28. The tileset NCGRs use all 16
  banks.
* **Only two sets exist.** FactionWars changes 158, 132 and 151 of the colours.
  REbalance's three files are FactionWars's byte for byte, and FUNLite's are
  vanilla's.
* **No map carries a palette of its own.** A map names only its tileset.
* **Loading.** FUN_020368DC loads the palette at level load into a palette
  object at `[[0x02152034]+0x18]`. The file buffer stays on the heap for the
  whole level, with its colours at `[[obj+0x34]+0xC]`.
* **Uploading.** FUN_02028B7C copies all 512 bytes to 0x05000000 whenever the
  object's byte +0 is 2 (dirty).
* **The game edits that copy itself:**
  * View_SetupLevelBGs sets index 255 and the team colours 55–63;
  * the palette cycler rotates ranges one step left and marks the palette
    dirty, about two frames in three. The ranges are 8–15, 16–39 and 40–47 in
    every world, plus 67–71 and 239–244 (King), 199–204 (Pirate) or 71–79
    (Mars).

### What the port does (lb_palette.cpp)

Each frame it compares the wanted set with the one in the RAM copy. When they
differ:

* every index outside the cycled ranges and the runtime ones gets the target
  colour;
* each cycled range gets the target table at the rotation it is in now. That
  rotation is found by matching the range against the table it holds, and it
  is unique for every range of both tables;
* byte +0 is set to 2, and the game uploads the palette itself.

A write to palette RAM would be undone by the next upload, so nothing is
written there. The sets are:

* **Vanilla**, from the player's own ROM (the new `lb_rom_read`);
* **FactionWars**, vanilla plus the bundled differences (`lb_palette_data.h`:
  index/colour pairs from `tools/lb_genpalette.py`, credited);
* **Per-Mod**, whatever the loaded content brings. The port loads no mod maps
  yet, so today that is the ROM's own, vanilla;
* **Random**, one of the two per level.

The palette is cosmetic and local to this machine; it never reaches the
simulation.

Verified:

* A castle map at FactionWars turns wintry: pale ground, icy trees (run
  t387p, 5 cycled ranges kept in phase).
* A Pirate-forced map switched from the panel mid-battle, to FactionWars and
  back to Vanilla, changed at once both ways (run t387u, 4 ranges in phase;
  `exp-t387u/win/pal_ab.png`).

## Force Tileset

### How a map names its tileset

There is no number. The `.map`'s TERR chunk holds the name as `char[32]` at
file offset 0x0B, and all 122 maps name one of the three tilesets.
FUN_020360B0 (load state 5) passes the decompressed buffer to the map parser
0x020A369C (Thumb, r0 the map, r1 the buffer, reached only by dispatch). Its
LoadTerrain copies the name to map+0x18. Everything after reads that:

* `BP/<World>Tiles.tbp` (table A, `*(map+0x338)`);
* `<World>Tileset.NCGR` and `.NCLR`;
* FUN_020A44C0's world index, which sets the minimap's world colours, the
  pickup sprites and the palette cycles;
* lb_music's scan, so "Map-specific" music follows the forced world.

So a pre-hook on the parser that writes the chosen name into the buffer makes
all of them agree.

### Gameplay is the map's

The tileset holds only pixels, colours and tile entries, and only the
metatile decoder reads its tables. Everything simulated comes from the map or
from fixed indices:

* the terrain classes (plane A, read through FUN_020A30AC at 16 sites);
* the tree and resource mask (block D);
* walls and bridges (fixed metatiles 0xD0–0xFC).

Passability, water, cliffs, mines and buildable ground therefore do not
change.

### What draws wrong, and the fix

The three worlds' table A are laid out in parallel, so base terrain translates
coherently: King water becomes Pirate sea or Mars liquid. Two things do not:

* **A map's own detail cells.** `BP/DetailTiles_<map>.tbp` (table B,
  `*(map+0x33C)`, 8.3% of cells overall) is made of tile numbers into its own
  world's NCGR.
* **A few base slots** hold a blank tile in the forced world: Pirate 44, 128,
  98, 165 and 170; Mars 44 and 128; King 118.

A pre-hook on Terrain_FillWindow (0x020A3234, the decoder's only caller)
patches both once per forced level, before the terrain draws:

* each blank slot takes a stand-in with the same terrain role (89, a plateau
  interior; 96, base ground; 116, grass);
* each table-B entry k takes the table-A metatile most common around the
  cells that use k. It counts the 8 neighbours; a detail neighbour votes once
  its own index is settled; ties go to the lowest index; up to 10 passes.

The fill works per index because plane E is not ours to edit. Over all maps
it picks what a per-cell fill would for 80% of detail cells, and looks as good
or better.

> **Corrected by docs/392.** "Looks as good or better" missed three things that
> then showed on screen as wrong squares (extra water, well caps and the like):
> * eight Mars-only slots that are not blank but draw another role (Mars 98,
>   118, 160, 165, 170, 205 into King and Pirate), now stood in when the map
>   is from Mars;
> * the vote ignored terrain class, so a detail patch beside a shore took
>   water; it now counts only neighbours of the index's plane-A class;
> * a grass-to-ground transition piece is class 0 too, so interior patches
>   took edge pieces; interior neighbours now vote first.
>
> Interior detail cells drawn with an edge piece fell from 5.7-7.1% to
> 1.4-2.8% per pair over all 122 maps. The tables arrive after the parse, so the patch waits for table A's
5,120 entries. One patched entry in each table is re-checked on every fill,
because a reloaded table of the same size can land at the same address.
lb_widemap's band cache is told to rebuild.

### Limits

* **Load-time only.** A change applies from the next battle.
* **Single player only for now.** The worlds' files differ in compressed size,
  so the heap would differ between machines. A two-instance match has not yet
  shown that harmless, so the option does nothing while the network is busy.
* **Replays.** Recordings carry `tileset=N palette=N pickseed=S`. Random
  draws per level from that seed and the count of maps loaded since boot.
  **Correction (docs/388):** the header is written before the settings file
  is read, so `tileset=`/`palette=` held the defaults. The values also stopped
  applying when the replay's last input had been delivered, before the level
  that input starts. Recordings now correct both with `FRAME s K V` lines, and
  a finished replay keeps its settings.

Verified:

* The castle map forced to Mars (run t387t): red ground, crystal forests, a
  red minimap, and no garbage tiles. "253 detail entries refilled for 659
  detail cells, 2 blank base slot(s) stood in".
* The same map forced to Pirates by the setting itself (run t387u): palms,
  jungle and beaches; 5 slots stood in.
* Random (run t387r) drew Pirates.

## Found, not changed

`lb_field.cpp` fills host-drawn shroud with 0xFF9AAAB2, King's fog index 5.
That is already wrong for vanilla Pirate and Mars, and differs again under
FactionWars. It only shows where the host's shroud and the game's cloud
disagree by a cell. Deriving it from live palette index 5 would fix it, but
would change the reference pictures, so it is left for its own change.

docs/21 is corrected in place. The tilesets are 8bpp with 1,024 tiles, not
4bpp with 2,048. DetailTiles is indexed by plane E at or above 440.

## Gate

All three default to off, which is the harness's setting. With them off,
both new overrides are pure tail calls, and the palette tick only reads. The
results match the reference runs:

* the exactness gate: counts identical to gate370b, and 8 of 8 layer files
  identical to gate381;
* the t384f replays: identical to each other and to rep384c;
* Vanilla: 240 of 240 identical to van-hudmask;
* 59.86 fps.
