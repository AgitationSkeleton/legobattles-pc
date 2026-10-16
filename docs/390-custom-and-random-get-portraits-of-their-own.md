# 390 — Custom and Random get portraits of their own

Status: **implemented and verified** (lb_teams.cpp, lb_patches.cpp,
lb_overlay.h). The goal: give the Free Play Random and Custom enemy teams
portraits from existing UI elements the same size as the Free Play mugshots
for King/Wizard/Pirate/Imperial/Earth/Alien -- a settings icon for Custom, a
question mark for Random. The LEGO Store menu has elements of roughly that
size.

## What the six portraits are

The ROM was read statically (with rendered contact sheets); the parts this
relies on were checked live.

* **The atlas.** `UI/AllInOne/Iconic_FreeplayFaction.NCBR` + `.NCLR`
  (resource 0x4F) is a 256x256 8bpp linear bitmap with its own 256-colour
  palette. It is laid out as 64-px cells holding 56x56 art; rows 128..255
  are greyscale copies.
* **The records.** The FE UIMgrData holds them: records 0x38..0x3D on
  resource 0x4F (King at 64,0, Wizard at 0,0, and so on). A record is six
  u32 frame pointers, one per button state, and a u32 resource. An unused
  record has no frames and resource 0xFFFF.
* **The loaded table.** Its address is the word at 0x021581C0. Found live at
  the opponent screen: 022464FC, 1,277 records.
* **Drawing.** The cartridge draws the button's frame as a 3D textured quad.
  The atlas is decoded and handed to FUN_0204E1D0 (called from
  FUN_0203242C) with the image's name as its seventh argument. That call
  queues the transfer to texture VRAM.

Two 56x56 frames in exactly the portraits' style
exist:

* the **Options cog** on the main menu (`Iconic_MainMenu`, record 0x4C,
  cell 0,64);
* the **LEGO Store "?"** (`Iconic_LegoStore`, record 0x50, cell 64,0).

Only one Iconic atlas is loaded at a time (slot 0x24), so they cannot be
drawn from their own atlases on this screen.

## What the port does

* **The pixels.** When FUN_0204E1D0 is handed
  `UI/AllInOne/Iconic_FreeplayFaction`, the two cells are pasted into the
  decoded buffer before it leaves RAM:
  * the cog, for Custom, over the grey Wizard (0,128);
  * the "?", for Random, over the grey King (64,128).

  OpponentSetup never draws those two grey cells. Both images are read from
  the player's own ROM and remapped to the atlas's palette (nearest colour;
  index 0 stays transparent). `Iconic_FactionSelect` holds the same pixels
  and is left alone, because its grey cells are its locked factions.
* **The records.** Records 0x4A and 0x4B are unused. Once they are free and
  records 0x38/0x30/0x31 are as expected, they get the frame blocks of 0x31
  and 0x30 state 2 (those same two cells) in all six states, and resource
  0x4F. This is checked every 15 frames, because the table is reloaded
  after a battle.
* **The selector** then cycles the six factions, 0x4A (Custom) and 0x4B
  (Random). The docs/384 rect shrink is now only for the 24x24 fallback,
  which stays in use until both of the above are in place.

## Verified

The t384f recording replayed (run t390c) cycles the opponent selector to
Custom, Random and Custom again:

* the log reads `portrait records 0x4A/0x4B set` and `Custom and Random
  portraits pasted`;
* the presented frames show the cog and the "?" as full gold-framed
  portraits, the same size and style as Alien and Earth beside them;
* the Custom opponent built was the recorded one: hero 133, builder 102,
  "army written for faction id 133".

## Gate

On the final binary (scratch `verify390.ps1`, pictures by decoded pixels):

* **The reference demo:** counts identical to gate370b. It never reaches
  Custom.
* **Pictures:** 8/8 identical to gate381.
* **Vanilla:** 240/240 identical.
* **fps:** 59.81.
* **The t384f recording**, twice:
  * identical to each other: `rep390a` is the new reference for that
    recording;
  * identical to rep384c up to frame 3500;
  * different from frame 4400 on, which is expected. The selector reaches
    Custom at frame 4220, and from then on the button draws a 56x56
    portrait through its own quad instead of a shrunk 24x24 frame. ARM9
    +66,125 instructions at 4400; ARM7 -3 at 4675.

  Both replays still field the recorded army: hero 133, builder 102.
