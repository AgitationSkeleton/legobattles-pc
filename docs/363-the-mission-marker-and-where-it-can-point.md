# 363 — The mission marker, and where it can point

Written for: whoever fixes the mission marker.

Status: **decomp traced; an observer added; not yet seen firing.** Nothing
changed in the picture or the game. The observer is deployed and silent by
default (`518a8a0e`, `build/LegoBattles.exe` sha256 `7b55cd1a...`).

## 1. What the marker is (decomp, the RTTI-named corpus)

- **The function** is `020DAAE4` (thumb, `push {r4-r7,lr}`), with arguments
  (owner, x, y). It sets a four-second expiry (`kGetMs` + 4000 at
  owner+0x1E8) and tests the point against the DS's own 256x192
  (`FUN_020A8DD0`, fourth argument 0).
  - A point **outside** that screen is placed at the point minus the camera
    (view +0x5E/+0x60, view = `**0x020DAC78`), clamped to [4,236] x [4,172].
  - For a point **inside** it, Ghidra shows the camera stored at
    owner+0x1F4/0x1F6. What the widget is given in that branch is not clear
    from the decompile.
  - The position is snapped to 8 px and handed as **two 8-bit values** to
    the widget at owner+0x1E4, through its virtual slot 0x10.
- **Its caller** is mission code at `02048A78` (`bl 0x020DAAE4`), which
  passes (obj+0x70, point.x - 128, point.y - 96). That code sits in a 2 KB
  region Ghidra never made a function of (between `FUN_02048774` and
  `FUN_02048f68`).
- **The owner is the battle view.** `User::WorldViewScreen::virtual_09`
  (`020DD708`), its per-frame update, hides the widget at +0x1E4 once the
  expiry at +0x1E8 has passed, by setting bit 1 of the widget's flags.
- **How it reaches the screen:** OBJ is off in the battle (DISPCNT
  `00010F08`), so the widget cannot be a hardware sprite. It is drawn
  through the 3D engine, like the in-field HUD that `lb_hud` already finds
  and relocates.

## 2. What that means in a widened view

The test and the clamp are the DS's own 256x192. A point the player can see
in a side band therefore still counts as off-screen, and the marker is
pinned to the edge of the native region, pointing at something already in
view. Widening the test alone would not do: the position travels as 8-bit
values, so band coordinates would wrap. A fix places the marker host-side,
as `lb_hud` places the corner HUD. When the point is inside the widened
surface the marker goes where the point is; otherwise it is clamped to the
surface's edges, not the DS's.

## 3. Looking for it firing

`LB_MARKERLOG=1` (src/lb_patches.cpp, an observer on `020DAAE4`) logs each
call:
- the point and the camera;
- where the point falls against the DS screen.

It then runs the cartridge's function unchanged. The exactness gate is
identical with it installed.

**Zero calls so far:**

- **All 14 recorded play sessions** (`build/lb-session-*.lbdemo`,
  09-14 to 09-22, the longest 22.5 minutes), replayed widened: 12 reached a
  widened battle, and none called the marker. They are skirmish or free
  play, which has no mission markers. The 22.5-minute session also put the
  native-picture skip (docs/361) through a real pause: its one native frame
  was completed exactly, 192 lines at full fade.
- **The harness's story route** (Knights, Act 3) reached a widened battle
  and sat 2 minutes in it with no input: none either.

So the marker needs a mission that points somewhere. The tutorial (Act 1
mission 1, which the story route skips on purpose because it holds the
camera) is the likeliest; failing that, a mission played to its first
objective. Until one of those fires it, a fix could not be seen working, so
none is written.

## 4. A correction made along the way

A scratch ARM9 image disassembled 0x020DAAE4 as
the middle of a function starting at 0x020DA9F4. The image is not the ROM's
(69% of its bytes differ). The real image,
`out/eur/arm9/section_static_02000000.bin`, has the prologue at 0x020DAAE4,
matching Ghidra, the RTTI walk and the recompiler's dispatch table. The
observer was briefly moved and moved back before any build.
