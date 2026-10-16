# 385 — Number keys, own-hero spells, drawers in flight, and the moving window

Status: **implemented; each item's verification is stated with it.** Nine
items, following docs/384 (where the randomizer and team builder work):

1. Little patches of terrain could still overlap things (a grass square over
   a unit).
2. In a randomizer match, the text naming the bot's team should be the name of
   its hero (Alien Queen, Gemma, Ghost, King), not the team of its builder
   (Earth, Wizard).
3. Export the caged Space Criminal / monkey sprite so it can be edited without
   the Space Criminal head, and add an Enhancement, on by default, that fixes
   the monkey cage.
4. With Space Criminal Leader selected, 4 on the number row or keypad should
   arm Hotwire; with a builder selected, 2 should follow what the game shows
   as the second HUD icon's colour (red for the builder, yellow for the hero).
5. An ImGui Enhancement, "Prioritize Own Hero Spells", that always defers to
   the hero picked for Free Play.
6. The drawer open/close animations did not always occur at the HUD's
   adaptive position (the red list drawn mid-field).
7. Faction-dependent music bled over from the Free Play enemy team.
8. Zooming in/out while a menu is open could offset what a click hits in it.
9. The game froze while the window was being moved, which would be a problem
   for online matches.

| # | Result |
|---|--------|
| 1 | Cause found in a play-session log; fixed; the carry-forward verified, the trigger not reproducible in the harness |
| 2 | Done, seen on the Victory screen; the four long names shortened |
| 3 | Exported (`out/exports/caged_criminal/`); the fix is an Enhancement, on by default |
| 4 | Done and verified live (red and yellow lists, number row and keypad) |
| 5 | Done and verified live through the game's own SetOwner |
| 6 | Cause traced frame by frame; fixed; the trace verified |
| 7 | Cause found in a play-session log; fixed |
| 8 | Not reproduced; logging added |
| 9 | Fixed and verified (180 frames in a 3 s modal loop against 2) |

## 1. Terrain patches: a row with no split is not vacated (lb_hud.cpp)

> **Correction (docs/388): the cause below is the wrong one, and the carry
> made it worse.** The logged split-0 rows were edge-marked frames. The first
> transport or siege unit on screen turns DISP3DCNT edge marking on until
> restart (Building_Draw3D), and the renderer did not split such a frame.
> That fits "in a big fight". Every row was then split 0: before this change
> each was vacated to nothing (the patches), and after it none was vacated.
> The carry lasted only until a box changed, and the HUD then sat at its
> native spot: the HUD snapped back until restart. docs/388
> splits edge-marked frames. The carry is kept only for a frame the renderer
> found no HUD polygon in (split 3), which is the case described below.

A play-session log has `[lb_hudsplit] first moved-HUD row with split 0
(not split: vacate to nothing)` in a big fight; no harness run has one.
`nds_gpu3d_split_rows` answers 0 for a row whose raster the renderer did not
split, which with the shipped settings (LB_PAIR, LB_SNAPCOPY and LB_RESHIFT
off; play.bat sets no LB_ variable) is a frame drawn with no HUD polygon at
all -- the frames after a camera step (docs/349 s9c) -- while lb_hud's boxes
are still the last scan's. The old path then treated every opaque pixel in
each box's native rect as HUD: vacated it to nothing, and captured it as the
element's picture. A unit standing on Deselect's native 24x24 rect lost that
square to the terrain under it, and the next frame drew the unit's pixels at
the button's corner.

Such a row has no HUD pixels to tell apart, so it is now left as drawn:
nothing vacated, the previous capture carried forward, and the moved HUD
drawn from it (`untrusted`, with LB_HUDSPLIT=0 keeping the old path).

Verified: `LB_HUDSPLIT0=1` (a test aid) forces every row to split 0 for half
of every second. Through those blocks the moved Deselect stayed at its corner
(run t385p2, eight shots). What the aid cannot show is the logged patch
itself: a forced row still holds the HUD the renderer drew, which is left at
the native spot for those frames, where a real split-0 row has none. An
`=old` variant meant to reproduce the patches did not (run t385p1 whole in
all eight shots) and was removed rather than kept as a misleading A/B.

## 2. A Custom or Random opponent is named after its hero (lb_patches.cpp)

The Start handler names the opponent right after building its roster:
`020C0FB2 blx FUN_020029DC` (the base's faction), `ldr r1,[r0,#0x28]; ldrh
r1,[r1,#6]` (its name id), `blx FUN_0203E1D4` (String from id, lr 020C0FC3),
then `FUN_02045858(setup, 1, name)`. The base is the builder's faction
(docs/384), hence "Earth". This is the custom branch (`FUN_0200239C`,
factions 132..134), which only this port's Custom and Random reach. A pre-hook
on FUN_0203E1D4 (ARM; every other call a tail call) swaps the id at that
return address for the hero's: blueprint +0x06 of the hero in the army record
the roster was just built from, the caller's `sp+0x18`, hero at +8.
`lb_teams_unit_name_id` reads the blueprint.

Verified (run t385r, a Custom opponent with hero 133): `[lb_enemy] the
opponent is named after its hero 133: Space Criminal Leader (name id 556, not
the base's 567)`, and the Victory screen lists "2 Space Criminal Leader".

**Four names were too wide for that box, and are shortened.** The 25 hero
names (from the blueprints in RAM) run from "King" (4) to "Space Criminal
Leader" (21). The 21-character one ran about 35 window px past the frame's
right edge at 1473x830. The box fits roughly 17 characters, so "Captain
Brickbeard", "Governor Broadside" (18) and "Space Police Captain" (20) very
likely overflowed too; "Alien Commander" (15) and shorter fit. After the first
build shipped, all four were shortened: Brickbeard, Broadside, Space Police,
Space Criminal.

* **Space Police Captain (129) and Space Criminal Leader (133)** take the
  game's own strings 553 "Space Police" and 557 "Space Criminal", the ids
  after the heroes' own.
* **The Enhancements rename (docs/356)** rewrites 556 in place to Kranxx or
  Plisken. While it is on, 556 is kept: it is short, and it is the player's
  chosen name. The two do not conflict.
* **Captain Brickbeard (40) and Governor Broadside (60)** have no string of
  their own. FUN_0203E1D4 is `FUN_020F1C88(dest, table[id])`, a copy from a
  C string, so the hook calls FUN_020F1C88 directly with a pointer to the
  word after the last space of the game's own name, returning where
  FUN_0203E1D4 would have. That works in every language.

Verified: runs t385n40b, t385n133 and t385nkr log "the last word of its name,
Brickbeard", "name id 557" and "name id 556" (with Kranxx). Guest RAM then holds
standalone heap strings "Brickbeard", "Space Criminal" and "Kranxx" (five
copies each, 0x40-byte blocks). The Victory screen did not come up in those
runs -- the owner-byte write ends a match only sometimes -- so the shortened
names were not photographed. Recordings say `oppname=2`; `oppname=1` (the
first docs/385 build) replays full names, and an older recording keeps the
base's name and its counts (rep385c, identical to rep384c).

## 3. The cage: exported, and the monkey cage fixed

The export, with a README, the frames, the sheet and the palettes, is in
`out/exports/caged_criminal/` (under out/, not committed). The cage is not a
blueprint: `Unit_BuildBillboard` (0203ABC8) draws any unit with a
WaitingToDie command from `Sprites/Projectiles` at a fixed cell, 0x140 (the
minifig head at the bars) or 0x1A0 (a monkey's). The choice reads the wrong
field:

    0203AD58  ldrh  r0,[r0,#0x66]   ; blueprint +0x66: the PROJECTILE index
    0203AD5C  cmp   r0,#0x99        ; 153: the Monkey's own INDEX (+0x04)

The Monkey is melee and its +0x66 is 0xFFFF, so no unit takes the branch and
caught monkeys wear the minifig head. The fix is one word, `ldrh r0,[r0,#4]`
(E1D000B4 for E1D006B6), applied at boot: the Enhancements tab's "Fix the
Monkey Cage", on by default, this machine only; `[lb_cage] caught monkeys use
the monkey cage` in every run's log. Patched, the function runs in Tier 3
(docs/377's pattern), which changes the counts, so recordings carry the
recorder's setting as `cage=N` and a replay takes it from there. Not seen on
screen: Monkey Business (pi3_1) was not reachable from the harness's save.

## 4. Number keys arm the second brick's items (lb_overlay.cpp, lb_patches.cpp)

1-9 on the number row or the keypad ask for item N of the list that Mouse 4's
brick opens: red (606, list 3 and its second row, list 4), yellow (608, list
2) or white (607, list 1), whichever the game shows. A key
bound to a DS button stays that button.

The lists (FUN_020DD414, the layout pass; FUN_020DA6BC, the toggle): list g is
at `screen+0xBC+0x30*g`, its items an array of POINTERS at list+0x10 to
entries `{widget*, payload}`. The layout places shown items left to right in
array order, skipping a widget whose record has bit 1 set: 9 entries for g0,
20 for g1, 31 for g2, 20 each for g3 and g4, and the red rows count only
entries with a payload. Opening list 3 opens drawer 4 as well when the red
element is two rows high, so N counts along g3 and then g4.

An item's callback is the bricks' own, FUN_020DC298. Its jump table
(0x020DC34C, ids from 561) sends 648..656 to the builder's buildings and
677..707 to the heroes' spells (FUN_020E62C4; the spell's byte goes to
021575B8, input action 0xF, the list closes). It acts only while its list is
the open one (FUN_020DA2B8 -> FUN_020DA3E0: `screen+0x26C == g`, 3 and 4 as
one). So a closed list is opened first with its brick's callback, the
player's own first tap, and the item follows when that returns. Both calls are
tail calls into the view update, like docs/383's shortcuts, gated the way the
pen path gates a click.

Verified live (run t385q):

* With Space Criminal Leader selected: 2 armed widget 683, 4 armed 707
  (Hotwire), keypad 1 armed 680. His spells are 680, 683, 706, 707.
* With the builder alone: 2 opened list 3 and took widget 644; the placement
  ghost followed the cursor.
* With both selected, the second brick was yellow and 2 armed SCL's second
  spell.

Recordings carry the request as `q 11..19`.

## 5. Prioritize Own Hero Spells (lb_patches.cpp, hero_hud_override)

FUN_020DE000 picks the unit that drives the HUD from the selection (02158024:
+4 two-byte handles, +8 count). It keeps the FIRST unit of the highest rank
among those that FUN_0208727C resolves, FUN_0205A118(e, 0xB) says are not in
state 0xB, and whose owner byte (e+0xE4) FUN_020875F0 and FUN_02085D58 accept.
The rank is by the blueprint's category (bp+0x5C): builder 1, hero 2, some
buildings 3 and 4, the base 5. Every hero is rank 2, so of two heroes the one
earlier in the selection wins. The winner's handle goes to 02157599 and a hero
goes to FUN_020DC004 (T; one caller, the BL at 020DE1B6, dispatched), which
fills the yellow list with its spells.

The pre-hook on FUN_020DC004 changes only the choice among heroes the game
itself would accept. If the unit is a hero that is not the army record's
(02154E40+8, the Customize Army pick Free Play fields), and the selection also
holds that hero, then that hero drives: its handle goes to 02157599 and r1 to
it. The own hero must pass the same tests, read rather than called:

* the handle resolves (FUN_0205C9B4: the table at [02155028], +0xDD
  generation);
* [[e+0xE0]+4]+8 is 0 and e+0x227 is set;
* e+0x114 resolves to nothing;
* neither [e+0xF4]+4 nor [e+0xF8]+4 is 0xB;
* it has the winner's owner byte. Both owner tests read only that byte
  (FUN_02086CA4, FUN_02085D58).

It is on by default (Enhancements), and recordings carry the recorder's
setting as `ownhero=N`.

Verified live (runs t385v..t385y). An enemy hero was given to the player
through the game's own Entity::SetOwner (020598C4, the function Hotwire
calls), using the new test aid `LB_TEST_ADOPT_FILE`. Writing the owner byte
instead ended the match in a "Victory!" (t385q).

* In 14 drags over SCL and the adopted Wizard, from every direction and
  distance, the selection listed SCL first every time. The game already
  chose SCL, so the hook had nothing to do and did nothing.
* To exercise the branch, the army record's hero was set to 20 (the Wizard).
  The log then read `the game picked blueprint 133; own hero 20 selected too:
  it drives the HUD`; the driver became 0900 and key 4 armed 690, the
  Wizard's fourth spell. His spells are 680, 681, 689, 690.
* Put back to 133, the same drag armed 707.

A selection order that varies wildly with the drag did not occur in these
runs. The selection order held whatever the drag, so the cause of such
variation (a hotwire trick may reorder the selection) is not established. The setting holds either way.

## 6. The drawers' pieces follow the bricks (lb_hud.cpp)

LB_HUDTRACE (new: every composite whose HUD elements differ from the last,
with each element's native box and move) on a list opening and closing,
run t385d:

    [4..20 x 16..64 q4 -244,-127]     the brick column, to its corner
    [68..100 x 40..64 q2 -240,-111]   an item in flight in the left third
    [100..132 x 40..64 q2 +0,-111]    one mid-screen: stays mid-field
    [212..244 x 40..64 q2 +251,-111]  one at the right: to the RIGHT edge

A list's items slide along their row between the column and their slots, and
each one in flight is an element of its own, placed by where it happens to be.
Once in its slot it touches the column and moves with it. The middle frames
are the red list drawn mid-field (item 6). docs/316's
counters could not see it: a piece in flight always moves vertically, so it is
never a "decline", edge or span. docs/316 carries a note to that effect.

The anchor is now found first: the element against the left edge, in the top
half and at least a brick tall. Its move is worked out with the placement
loop's own formulas, and a piece takes it. A piece is one item (at most
48x26), not against the left edge itself, in the anchor's rows or the one
below them (the red list's second row), sharing a texture with the anchor and
holding no command icon.

Verified (run t385e): every piece in flight is `-244,-127`, opening and
closing.

## 7. Faction music followed the enemy (lb_music.cpp)

A play-session log: `local player is owner 1 (its base is nearest the opening
camera)`. The camera heuristic chose the enemy in Free Play, and faction
music played Wizard, then Alien, until a later latch went back to Earth. The
cartridge knows the local player: its session block (02156A24) holds the
local id at +0x64, and the player list (*0215540C: +8 array, +0xC count) each
player's id at +0x10 -- FUN_02034B44's gate reads them. The list index is the
owner, accepted when that owner has a base on the field; otherwise the camera
decides as before. Each latch now logs which source decided.

## 8. Zooming with a menu open: not reproduced

A one-notch wheel step in each direction was followed by clicks on the base's
list at 0, 0.05, 0.15 and 0.4 s (run t385zc, surfaces 736x414 and 842x474).
Every press on a drawn item mapped to that item: the hero item at native 24,
the builder item at 49. The misses in those runs were the harness's own. A
closed list was clicked, and the first runs aimed with window-capture
coordinates that include the frame (about 11,33 px). The play settings
(pixel_scale 2, hud_scale 1) are the harness's. LB_HUDHITLOG now prints the
scan's band and the shown band with every mapped press, and every moved face
on a miss. A drawer piece in flight was hit-tested where it was misplaced
(item 6), which could explain a press landing on the neighbour during an
animation; that is a guess, not a finding.

## 9. The moving window no longer stops the game (lb_winloop.cpp)

Pressing a title bar or border, or opening the system menu, makes
DefWindowProc run a modal loop inside the dispatch that SDL_PollEvent makes,
and nothing returns to the frame loop until release. Emulation, presentation,
audio feeding and the network stop; the network runs on this thread
(lb_net.cpp), so a lockstep peer waits for as long as the button is held.
SDL2 (2.32.10 here) has no answer for an application with its own loop.

Emulation cannot leave the thread that owns the windows, so the pump moves
instead:

* The frontend's `while (SDL_PollEvent(&e))` becomes `lb_winloop_pump()`
  followed by `SDL_PeepEvents` (apply_hooks stanza 0zzzzzzzzzzzzzz).
* `lb_winloop_pump()` runs SDL_PumpEvents on a second fiber of the same
  thread.
* Each SDL window's procedure is subclassed. WM_ENTERSIZEMOVE and
  WM_ENTERMENULOOP start a timer, and each WM_TIMER the modal loop
  dispatches switches back to the main fiber. The frame loop runs a frame,
  paced by its own deadline, and its next pump resumes the modal loop.
  WM_EXIT* stops the timer.

Outside a modal loop the pump fiber pumps once and returns, as SDL_PollEvent's
first call did. `LB_WINLOOP=0` turns it off.

Verified with the harness's stand-in for a drag (a window posted WM_APP+0x4C
runs a nested loop shaped like DefWindowProc's for N ms), run w385on against
w385off:

| | frames presented in the 3 s loop | a debug command sent 0.5 s in |
|---|---|---|
| fixed | 180 (60 fps) | answered at once |
| `LB_WINLOOP=0` | 2 | answered after 2.53 s |

Not exercised: Windows' own modal loop, which cannot be driven without moving
the real cursor. That it dispatches WM_TIMER to the window is the documented
behaviour SDL3's live resize rests on. A real window drag is the final check.

## Recordings

New header keys: `cage=N` and `ownhero=N` (the recorder's settings) and
`oppname=N` (1 full hero names, 2 the four long ones shortened); `q` events
11..19 for the number keys. Absent, each is stock.

## Gate

* The exactness gate (gate385): counts identical to gate370b, and 8 of 8
  layer files identical to gate381.
* Vanilla (van385): 240 of 240 frames identical to van-hudmask.
* The t384f recording (a Custom opponent), replayed twice (rep385a/b):
  counts identical to each other and to rep384c, docs/384's final build.
  An older recording keeps the base's name and the stock cage.
* fps: 51.67 in the instrumented gate run. Three plain replays of the same
  recording gave 58.75, 59.69 and 59.86 (docs/384: 59.92). The docs/384 exe
  run through LB_EXE reports no fps line, so the A/B has one side.
* The cage word costs nothing extra in play: Unit_BuildBillboard already runs
  in Tier 3 for docs/377's depth words (0203B28C..0203B29C).

## Lessons

* fdrive clicks are CLIENT pixels; lb_capture.ps1 shots include the window
  frame (about 11,33), so a point read off a shot has to lose it first.
* Writing a unit's owner byte ends a Free Play match; give a unit away through
  Entity::SetOwner (`LB_TEST_ADOPT_FILE`).
