# 393 — The camera grid, and the opening opponent's portrait

Status: **both fixed and verified live.** Files: lb_patches.cpp, lb_demo.cpp,
lb_overlay.cpp. Defects found after docs/392:

* when the default Free Play opponent is set to Random, it still used the old
  smaller tile instead of the LEGO shop icon, and likely the same for Custom;
  'Random' in that dropdown is renamed 'Randomizer';
* build previews' checkmarks still did not align the way they do in vanilla
  (seen on a farm's ghost and its checkmark).

## 1. The opening Custom or Randomizer portrait

The 56x56 portraits (docs/390) are used only once both of these hold:

* records 0x4A/0x4B carry their frames;
* the faction atlas has been pasted.

The opponent screen uploads that atlas AFTER `OpponentSetup::v08` finishes.
Run t392n logged "opens on choice" before "portraits pasted". So docs/392's
opening choice took the 24x24 fallback, and nothing replaced it; the arrows do
replace it, which is why only the opening was affected.

The opening record is now watched (`opponent_open_portrait_tick`, once a
frame). It is given its portrait the first frame one is available. The watch
lets go when the player picks something else, when the portrait is already
up, or after ten seconds.

Verified (t393a, t393b): `opening choice 7 now drawn as its portrait (frame
0x4B)`, and choice 6 the same with 0x4A. The screen shows the LEGO Store "?"
and the cog.

The dropdown entry now reads "Randomizer".

## 2. The checkmark, the footprint and the camera grid

### What the cartridge does

* **The checkmark is pixel-exact.** `func_02036FBC` puts it at the site's
  true screen position: x = ox*24 - camx + 4, y = oy*16 - camy - 8.
* **The footprint snaps to 8-px cells.** `func_0203CBB8` paints cells
  floor(p/8), on BG3.
* **BG3's scroll is fixed.** `View_SetupLevelBGs` scrolls BG3 ONCE, to
  (4, 0), and nothing scrolls it again.
* **The terrain's scroll follows the camera.** `Task_View_CommitBGs` scrolls
  BG1/BG2 to (4 - camx%8, camy%16) every commit.
* **The UI cell mask clears BG3 under the checkmark.** BG3 has priority 0, so
  the cartridge has to clear it where a UI sprite should show
  (`SpriteUI_MarkCells`). The range is (x-4)>>3 to (x-4+w)>>3, end exclusive.

So the footprint lines up with the terrain, the checkmark and the mask only
when camx % 8 == 0. Only then does the checkmark sit exactly on the
footprint's corner and the mask cover exactly the cells under it.

### Why vanilla never shows the problem

The camera rests on that grid:

* the d-pad glides 32 px from rest to rest (1,2,4,8,8,4,2,2; LB_CAMEDGELOG,
  run t393q);
* a minimap tap masks the camera with 0xFFF8 (literal 0x0203CBA4);
* every stock bound is a multiple of 8 (min*24, min*16, max*24 - 232/248,
  max*16 - 176/192).

### What the port did

docs/373's widened bounds were span/2 + 4 and 252 + (span - span/2): a
minimum camx of 244 at scale 2.0 and 203 at 2.25. A Free Play battle that
starts against the map's west edge rests there.

Measured at scale 2.25 (t393l), in the guest's own frame and in the
window alike:

* the checkmark sat 5 px right of the footprint's corner;
* the mask cleared the cell left of it (a hole in the footprint);
* the footprint's next cell drew over the checkmark's lower right.

At 2.0 the same happens by 4 px (t393d). Mid-glide offsets are transient and
are vanilla too.

### The fix

All four immediates of the camera-edge patch are rounded up to multiples of 8
(`cam_edge_imm_grid`). That stops the camera at most 7 px sooner, never into
black: the near-black counts of the window's 16 leftmost columns are the same
before and after, at both scales (frame and HUD outlines only). Bounds at 2.25
are now "view size 456x280, min + 208,88".

Verified at both scales: the camera starts at 248 (2.0) and 208 (2.25); the
checkmark sits on the footprint's corner, the box whole, no hole (t393o,
t393p). Through d-pad, keyboard, edge-scroll and wheel moves every resting
camera was on the grid (t393q).

Recordings carry `camgrid=1`. A camedge recording without it replays the
unrounded bounds its taps were aimed at.

### Ruled out on the way

The port's picture of the footprint and checkmark matches the guest's own
frame, at both scales and in all four bands (t393d-j). The band harvest
shifts by whole metatiles, which keeps the phase. Nothing in the port writes
BG3's scroll.

## Gate

On the final binary, with the test mods folder parked:

* **The t384f recording.** Twice (rep393a/b): counts identical to each other
  and to rep392a. It has no `camgrid`, so it replays the old bounds.
* **The counts gate.** gate393: counts identical to gate370b; 8 of 8 pictures
  identical by pixels to gate381; 59.81 fps.
* **Vanilla.** van393: 240 of 240 pictures identical by pixels to
  van-hudmask.
