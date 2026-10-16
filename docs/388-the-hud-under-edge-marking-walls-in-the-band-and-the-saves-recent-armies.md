# 388 — The HUD under edge marking, walls in the band, the save's recent armies

Status: **implemented and verified** (apply_hooks.py stanza 0zzzzzzzzzzzzzzz;
lb_hud.cpp, lb_patches.cpp, lb_widemap.cpp, lb_overlay.cpp, lb_demo.cpp,
lb_music.cpp). The defects and requests, found over several full Free Play
matches:

1. Zoom could affect the rendering of newly built buildings: they did not
   update until zooming in or out, and could stay unrendered at some zoom
   scales. Seen on The Pond with Wall structures, which appeared only after
   zooming in.
2. The HUD's rendered elements (not the clickable ones) frequently snapped
   back to their original-aspect positions and did not return to their
   adaptive positions until the game was restarted. In that state the
   elements could not be clicked where they were drawn; the clicks still
   went to the adaptive positions (e.g. the blue brick in the upper left).
3. The team builder keeps a recent cache of at most five teams loaded from
   the save file, with a date stamp, that the ImGui cannot overwrite, and a
   team picked in the dropdown (saved, cached, current, etc.) loads without
   the Load button.

## 1. The HUD snapped back: edge marking, which the first ship turns on

### What the cartridge does

`Building_Draw3D` (0x0203B3AC) is misnamed. It is called from the building
pass of `View_DrawEntities` (0x0203A9E8) only for an entity whose blueprint
category (`[[e+0x160]+8]+0x5C`, docs/12) is 5 or 6 (`FUN_02036CD0`):
**Transport and Siege**, the ships, catapults, ballistas, dragons and
gryphons it draws as textured models. Before it draws, it does

    0203B41C  DISP3DCNT &= 0xCFEF          anti-aliasing off
    0203B434  DISP3DCNT = (x & 0xCFFF) | 0x20   edge marking on

Nothing turns them back. The other four writers of 0x04000060 are the SDK's
VRAM-bank setters (FUN_0210B3A8 and FUN_0210B668, which touch bits 0 and 14),
`G3X_Init` (0x0210C184, at boot, which sets AA) and the scene setup
`FUN_02024758`, which ORs in bit 3 and leaves bit 5 alone. So from the first
transport or siege unit on screen to the end of the session, every frame is
rendered with DISP3DCNT 0029 instead of 0019: edge marking on, AA off. That is
why it lasted until the game was restarted.

docs/360 is corrected in place: its finding (no edge-marked frame in the
reference combat replay) stands for that replay, which has no ship or siege
unit, and not for a full match.

### What the port did with it

docs/375 splits a widened frame's HUD into its own raster, and refused to
when edge marking is on: edge marking compares each pixel with its four
neighbours, and the HUD set is cleared only on the HUD's own rows. docs/385
then left every unsplit row's HUD where the game drew it, carrying the last
split frame's capture forward. Under 0029 every row is unsplit, so the moved
HUD survived only until a box changed. After that the HUD sat at its native
spot, while the hit test (docs/383) still sent clicks to the moved spot.

Reproduced by writing 0029 into DISP3DCNT from the debug server
(scratch `edge388.py`), exactly the value Building_Draw3D leaves: the bricks
and Deselect jumped back to the native corner (run t388g, `e1.png`).

### The fix

Edge-marked frames split as well, and still render the unsplit picture.

* **The HUD set is cleared on the rows next to the HUD's rows** (and the
  bottom border). The HUD's own final pass therefore reads clear neighbours.
* **`lb_EdgeMerge` computes a HUD row, and each row next to one, in the HUD's
  columns.** It works the way the unsplit raster would. First the HUD is blended over the world as
  lb_FinishRow merges them. Then edge marking runs exactly as ScanlineFinalPass
  does, against the depth and polygon ID the unsplit buffers would hold:
  * the HUD's, where a HUD pixel wrote depth in front of the world;
  * the world's, otherwise.
  * A translucent pixel keeps the opaque ID and edge flags of what it covers
    (`PlotTranslucentPixel`). That is the world's when no opaque HUD pixel
    is under it.

  The world-only row stays the world's own, with the world's own edges, for
  the spots lb_hud vacates.
* **Fog or AA on as well** makes the merge inexact, and the frame falls back
  to the two passes as they are, logged once. Neither has been seen:
  Building_Draw3D clears AA, and battles have no fog.
* **`lb_SplitRows` returns 3** for a frame the renderer looked at and found no
  HUD polygon in (the frames after a camera step, docs/349). Only those carry
  docs/385's capture. A split-0 row that is left (a copied row, the compute
  renderer, `LB_HUDSPLIT=0` or `=ab`) moves the HUD from its own pixels, as
  before docs/385. The compute renderer never splits; under docs/385 it would
  have left the HUD at its native spot on every frame.

### Verified

* **The snapped-back state, reproduced and fixed** (runs t388g/t388h, same route,
  0029 written mid-battle). Before the fix, the HUD sat at its native spot.
  After it, the bricks and Deselect stay in their corners, and the base shows
  the edge-marked outline the ROM draws: t388h `e1.png`, `e2.png`.
* **Exactness.** `LB_EDGECHECK=1` renders every split frame unsplit first, on
  the render thread, and counts the pixels the real frame differs by:
  * t388h: 1,381 unmarked split frames with 0 px different, then 301
    edge-marked with 0 px different.
  * The game's own polygons are nearly all ID 0, and edge marking only draws
    where IDs differ, so that result says little. `LB_TEST_POLYID=1` gives
    every polygon an ID from its index, in both passes. With the units walked
    into the native Deselect rectangle, `LB_EDGEMERGE=0` (the two passes as
    they are) differs by 4 px a frame, all on rows next to HUD rows: world
    pixels beside the HUD (run t388k0). With the merge: 0 px in 661
    edge-marked frames (t388k1). The check sees a difference, and the merge
    removes it.

## 2. Walls built in the band stayed invisible until a zoom

A wall is terrain. The retile (docs/367) rewrites plane E and asks for a
redraw through `FUN_02039DC0`, which sets the view's +0x66 byte.
`Task_View_StreamBG1` (0x020396AC) then calls `Terrain_FillWindow` for the
native window, and `Task_View_CommitBGs` clears the byte. lb_widemap builds
the band beyond the native 256 columns from plane E itself. `fill_terrain`
cached that table on the camera, the map and the band's bounds. A retile under
a camera that had not moved therefore kept the old cells until the camera
crossed a tile, or a zoom changed the bounds. That is "until I zoomed in".

The Terrain_FillWindow pre-hook (docs/387) now also bumps docs/387's
`g_terrain_gen`, so every guest refill rebuilds the band. The byte is set only
on a camera cell crossing, a forced refresh or a redraw request, so nothing
extra is rebuilt while the camera is still.

Verified with scratch `wall388.py`, which rewrites metatile rows 12-14 across
the map and sets +0x66, as a retile does:

* the deployed docs/387 build, run as a copy (t388w0, `w2.png`): the new row
  appears in the native 256 columns only;
* this build (t388w1, `w1.png`): across the whole view.

## 3. The save's recent armies, and choosing a team loads it

* **The cache.** Once a second, whether the tab is open or not, the loaded
  profile's committed army is recorded, unless it is King's default (always in
  the list). It goes to the front of `team_save_cache` in settings.json, stamped
  `YYYY-MM-DD HH:MM`, at most five, newest first. A team met again moves to the
  front with a new stamp.
* **The list.** It shows "Recent save team, <stamp>" after "Save File Team".
  Everything before the player's presets is read-only: Save cannot write over
  those entries and Delete is disabled.
* **Choosing loads it.** Picking any entry in the dropdown loads it, so the
  Load button is gone. The choice is kept by its label, since a new recent
  entry shifts the indices.

Verified (scratch `teamcache388.py`, run t388t): armies A, then B, then A
again were written into record 0 with the committed flag. The cache read A,
then B/A, then A/B, with the harness profile's own army at the end: A moved
to the front, and nothing was duplicated.

## 4. Replays keep the recorder's settings

The recorded play session could not be replayed faithfully, for three reasons.

* **The header holds defaults.** It is written before the settings file is
  read. A recording made now, with Music at Faction-aligned, says `music=0`,
  and the value changes two frames later. docs/385's `cage=`/`ownhero=` and
  docs/387's `tileset=`/`palette=` held defaults the same way. That recording's
  `tileset=0` is probably this, not a change mid-session.
* **Settings changed later were not recorded at all.**
* **A replay's settings ended with its last input.** A Free Play recording ends
  on the Start press, so the level loaded after it with this machine's
  settings. That was true of docs/387's header values too.

So:

* **`FRAME s K V`**: setting K changed to V on that frame. K is 0 Force
  Tileset, 1 Level Palettes, 2 Music, 3 the cage fix, 4 Prioritize Own Hero
  Spells. The first frames carry one for every setting the header got wrong.
* **The header** adds `music=N sets=1`.
* **A replay that runs to its end** keeps the recorded Force Tileset, Level
  Palettes and Music. Stopping it hands them back.

Verified:

* the t384f recording with `100 s 0 3` and `100 s 2 5` added (run t388s): the
  level loaded forced to Mars and played Always Wizard;
* a fresh recording (run t388r) wrote `music=0 ... sets=1`, then `2 s 2 2`.

## Gate

On the final binary (scratch `verify388b.ps1`): counts identical to
gate370b; the t384f recording twice, identical to each other and to rep384c;
pictures 8/8 identical to gate381; Vanilla 240/240 identical to van-hudmask;
59.84 fps.

The pictures are compared by decoded pixels. Python 3.14's zlib gives the
harness's PNG writer (tools/lb_prune_shots.py) different bytes for the same
pixels. By hash, 4 composites and all 240
Vanilla shots "differed", with every pixel identical.

## Next, researched

* **The Custom and Random opponent portraits.** The Options cog
  (`Iconic_MainMenu` record 0x4C) and the LEGO Store "?" (`Iconic_LegoStore`
  0x50) are 56x56 in the portraits' own style. They
  can be pasted into the loaded `Iconic_FreeplayFaction` texture with two new
  frame records (scratch `selart/`).
* **Mod maps.** All four MapDump ROMs carry the same custom multiplayer roster:
  18 of the 30 `mp` slots, byte for byte. The map table is at 0x02147DDC (30 ×
  28 bytes), registered by FUN_02045E50. A joiner that lacks a map key writes
  through a null pointer (FUN_020F5168). Scratch `modmaps/`.
