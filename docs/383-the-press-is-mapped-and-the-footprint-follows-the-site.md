# 383 — The press is mapped, the footprint follows the site, and four shortcuts

Status: **fixed** (apply_hooks stanza 0zzzzzzzzzzz; `lb_hud.cpp`,
`lb_widemap.cpp`, `lb_patches.cpp`, `lb_overlay.cpp`, `lb_demo.cpp`). Also
fixed on the way: a building's label drawn under its list, and replays that
lost a release's out-of-band coordinate. The four items:

1. A click on the HUD (red/blue bricks) sometimes under-clicked it (its
   clickability possibly flickering) and placed a movement cursor beneath it,
   directing a unit or structure.
2. The red/blue building placement and its cursor were still not fully
   consistent.
3. Space and right-click bound to quick deselect by default.
4. Mouse 5 opens the blue brick menu and Mouse 4 the red/yellow/white menu.

## 1. A press on a brick fell through to the field

### What the cartridge has

docs/371 corrects the cartridge's own rectangle test, `FUN_020EA1A4`, one
widget at a time. `widget_rect_observer` reads each widget's rect as the
cartridge does and asks `lb_hud_hit_unmap` for the point to test it at. The
widgets and quads were read on a held battle at 1473x830 (run t383a,
`LB_HUDLOG` and `LB_HUDHITLOG`):

| widget | native rect | what it is |
|---|---|---|
| 605 | x 4..12, y 16..40 | the blue brick |
| 606 | x 4..12, y 40..64 | the red brick |
| 609 | x 12..28, y 16..40 | beside the blue brick |
| 613 | x 12..28, y 40..64 | beside the red brick |
| 628 | x 4..28, y 168..192 | Deselect |

The brick column is drawn as **five quads** over native x 4..20, y 16..64
(`[lb_hud] ELEMENT native x 4..20 y 16..64 quads=5`). `lb_hud` moves the
element as one, but the hit test's boxes are the individual quads.

### Why the right half missed

docs/371's rule finds each widget's element from **the widget's centre**:
the smallest scanned box whose source holds it.

* **605 and 606** (centre x 8) find their quad. For a press on the right half
  of the drawn brick, the unmapped point is past x 12, off that quad's face,
  so both answer "moved away": miss.
* **609 and 613** have their centre at x 20. That is just past every quad
  (the boxes are half-open, and the column ends at 20), so they find no
  element and answer **stock**, at their old native place. The press is not
  there.

No widget took the press, and the manager gave it to the field, which made
it a move order under the button. On a DS the same point is native x 16 and
the cartridge decides.

The same rule gave the "flicker". On a frame the cartridge submits no HUD
quads (the frames after a camera step, docs/349 s9c), docs/371 falls back to
the last box it remembered **for that widget rect**. So whether a press hit
depended on which widgets had been found before.

### The fix: map the press, not the widget

`hit_unmap_v2` (lb_hud.cpp):

1. **On a moved element's drawn face:** the press becomes the native point
   under it, for every widget, through the face drawn on top there. The
   cartridge's own tests then run there exactly as on the DS, so 605 takes a
   press on the brick's left half and 609 its right half, as the cartridge's
   layout says.
2. **On no moved face:** a widget whose rect covers at least a quarter of its
   own area of a moved box's source has left this spot, so it misses (-1).
   This keeps docs/371's "a press on the vacated spot is a field click".
3. **Anything else** is stock.

The faces come from this frame's scan. When the cartridge drew no HUD this
frame, they come from the last scan that had any, at the same band and
surface size (`g_hit_last`), for every widget alike.

### The first version of the fix broke placing a building

Step 1 first took the **smallest** face holding the press. Then the placement
route (`scratchpad/test383f.py`: builder, red list, second building, ghost)
reached a placement in one run of two with the same clicks. With
`LB_HUDHITLOG` (run t383h), the click on the second building was mapped to
native 56,51 at first, which is widget 644, the building. Partway through the
press it was mapped to **native 52,35**, inside widget 14's rect (x 36..132,
y 24..40). The held re-test then failed 644, and the next click on the field
became a move order.

What appeared mid-press (run t383i, the press held 1.3 s, `LB_HUDLOG`): a
**label box**, 24 tiles of 6DD01000 at native x 36..132, y 24..40, directly
above the list (x 4..236, y 16..64 with the bricks). docs/365 keeps them
apart, because they only share an edge and their textures differ. Each was
then placed by its own rule:

* **The bricks and the list** are against the top-left edges and go flush:
  -244,-127, which is the band plus -4,-16.
* **The label** touches no edge and keeps its inset from the corner:
  -240,-111.

So the label landed on the list's top sixteen rows. It was drawn **under**
the list and never seen (`exp-t383j/win/label-ab.png`, top: nothing above the
list), but it was the smaller face, so it won the press. Whether a click
selected the building depended on whether the release came before the label
appeared. The old per-widget rule never looked up widget 644 in the label's
box, so this was a regression of the first version, not an old fault.

Two changes, both applied live and to recordings that say `hudmemo=1`:

* **The label stays on the list** (`hud_attach_live`, lb_hud.cpp). An element
  placed by the inset rule on an axis, which touches an element held against
  the **same side** by its edge, takes that element's delta on that axis.
  They keep the cartridge's layout. An element on no side (the centred
  tutorial box beside Deselect, docs/365) or against an edge itself is
  decided as before. `LB_HUDATTACH=0` turns it off.
* **Step 1 takes the face drawn on top.** `lb_hud_shift_line` draws the
  boxes in index order and a later one overwrites an earlier, so the last box
  whose face holds the press is the element under the cursor.

### Also: a fast click was released at the top-left corner

A mouse click whose button came up within two frames of the press has its
release deferred to the frame loop so the ARM7 sees the press. That deferred
release sent `nds_set_touch(0, 0, false)`: native (0,0), with no out-of-band
coordinate. The cartridge acts on the release edge (set_touch_from_mouse's
own note), so a fast enough click ended at native 0,0, which is off every
button (the bricks start at x 4, y 16). The release now goes through
`set_touch_from_mouse` at the last event's position, like every other release
(apply_hooks, frontend.cpp). This one was found by reading the code, not
reproduced: the harness's clicks are held 150 ms and never take this path.

## 2. The placement footprint and its checkmark

### What was drawn

docs/320's harvest paints the footprint for the widened view. It shifts the
guest's camera by whole metatiles so the cartridge's own painter
(`func_020370B4`, a 32x24-cell buffer through the row table at 0x021520F8)
paints the part of the map the band shows. The post hook (`func_02036FBC`)
reads the cells and restores the camera. That harvest had two limits:

* **X only.** There was no vertical shift, and the compositor cleared every
  BG3 row outside the guest's own 192 (`lb_offmap` in gpu2d). A site dragged
  above or below the DS's rows therefore had **no footprint**, while its
  checkmark was still drawn beside it (docs/372). That is the placement
  inconsistency.
* **Capped at ten metatiles** (240 px). A side band wider than that, such as
  342 px each side on a 940-wide surface, had an outer part no
  harvest window reached.

### The fix: shift from the site, in both axes

`lb_fp_band_shift2` (lb_widemap.cpp) reads the site's origin (record
0x02157958 +0x0C/+0x0D, in map metatiles of 24x16 px) and the camera
(view +0x5E/+0x60, halfwords). The site's size comes from the post hook's
state (+4/+5, `lb_fp_set_site_size`).

* **Site inside the painter's 256x192:** no shift and no cost. This is the
  common case, as before.
* **Otherwise:** the camera is shifted in whole metatiles in **each** axis,
  so the site lands two metatiles in from the window's top-left. That leaves
  room for the widest building and the checkmark on every side.

One harvest now holds the cells and the shift they were painted at. The
native window is rebuilt from it by a shift of whole cells in both axes
(`lb_fp_native_rebuild2`, docs/320's argument). `lb_fp_band_tile` serves a
band cell by row as well as column. The gpu2d stanza asks it for BG3 rows
outside the guest's own and draws nothing where it has no footprint cell (the
inert tile 0x001F is opaque, docs/205).

Every cell value is still the cartridge's own painter's. Only which window it
paints is arranged, and the camera write keeps the halfword width
(`func_0203CBD8` reads both with `ldrsh`).

## 3 and 4. The shortcuts

Input tab, **Battle shortcuts** (on by default; `quick_keys` in the
settings):

| input | runs the button |
|---|---|
| Space, right-click | Deselect (widget 628) |
| Mouse 5 (forward side button) | the blue brick (605, group 0) |
| Mouse 4 (back side button) | the red, yellow or white brick, whichever is shown (606 red, group 3; 608 hero spells, group 2; 607 upgrades, group 1) |

The first version pressed the button on the touchscreen, but the shortcuts
are meant to invoke the functions, not tap the screen. So each shortcut now **runs the
cartridge's own code for that button**, and the touchscreen is not involved.

### What the cartridge runs

This is a static trace. Each call path below was checked against
the ARM9 image with capstone, and the event fields and the widget walk were
re-read here.

* **One callback for every HUD button.** `WorldViewScreen::virtual_08`
  (020DAD5C) stores **FUN_020DC298** (Thumb) in W+8 for every button of the
  battle layout (020DBAD8). The pen handler FUN_020E489C calls it at
  020E49C8 with r0 = the widget, r1 = the PenEvent, r2 = the screen and
  r3 = 1. It switches on `id - 0x231` (table at 020DC34C).
* **It acts on a click.** FUN_020E5628 reads E+0xC (u32; 1 = touch) and
  E+0x18 (u32; 0, 2 or 3 = press, 1 or 4 = release; 0200AA70 and 0200AAC8).
  A release returns 1, and only then does the callback act. A press only
  raises the hint label.
* **Deselect (628)** runs input action 5: the selection count goes to 0
  (FUN_020EC7B0 on 02158024), the command mode is reset (FUN_020A8A24(0)),
  and FUN_020E3078(1) refreshes the HUD, plays the sound and posts a
  SelectionChangedEvent.
* **The bricks** call FUN_020DA6BC(screen, group). It closes an open list
  and opens the group's list if it was not the open one, so pressing the
  open brick closes its list. They also post the GameMenuEvent that the
  tutorials wait for. The red brick's call is the same for a builder and a
  base; only the list differs.
* **Everything is client-local.** No simulation command is submitted.
* **FUN_020d7e18 is not this.** It tests id 0x274 too, but it is the score
  screen's callback, and there 0x274 opens the parent screen.

### How the port calls it

`view_update_override` (lb_patches.cpp) sits on `WorldViewScreen::virtual_09`
(020DD708, Thumb, the battle view's per-frame update, reached only through
its vtable). When a shortcut is pending, the override does the following:

1. **Checks what the pen path checks.**
   * The screen is registry slot 0x5C (`*0x02157FAC`, vtable 0214F0EC) and
     its layout is live.
   * The UI manager at 021581A4 has pen handling on (+1 == 0), and no
     button holds the press in progress (+8 != 1). Otherwise the held
     button's own release would act a second time. A press the **field**
     owns (+8 == 2) does not stop a shortcut: that value only routes pen
     events, and a shortcut is not one.
   * The game is in play (0214BBA8 == 5), and the mission has not suspended
     the HUD (screen+0x22C).
   * The widget, found by id in the layout's own array (layout+0xC, count at
     layout+2, as FUN_020E14BC walks it), has the HUD callback, and is
     neither hidden (FUN_020EA4B0's test) nor disabled.

   The callback plays its click before its own hidden test, so these checks
   must come first.
2. **Builds the click.** It builds the PenEvent the widget manager would pass
   for a click, below the stack (vtable 0213B998, touch, release, the
   widget's centre).
3. **Calls it as a tail call.** It calls FUN_020DC298 with the manager's
   arguments, and the return goes to 020DD708 itself.
4. **Resumes the update.** When the callback returns, the override sees the
   stack pointer it gave it, restores the update's registers and runs the
   update as normal.

No nested guest call is made (lb-lifted-body-never-returns: a nested call can
be preempted and re-enters its caller). A slice that ends inside the callback
simply resumes there.

So a shortcut:
* works while the player is holding a press or dragging a box;
* cannot fall through to the field;
* does nothing when its button is not shown, for example with nothing
  selected.

The frontend only posts the request (`lb_quick_request`). The rest happens in
the guest, in the first view update after it.

### Limits

* Enhanced mode only, and only in a battle.
* Space only when it is not bound to a DS button, and not while an ImGui
  field has the keyboard.
* The four inputs are consumed while the setting is on. Outside a battle they
  did nothing before either.

A recording notes each shortcut the frame it ran (`q` events, `quick=1` in
the header), and a replay runs it in that frame.

## Recordings

New recordings' headers add `exty=1 hudmemo=1 fp2d=1 quick=1`.

* **`exty=1`:** touch lines carry a seventh field, the out-of-band Y.
  Before, only X travelled, so a press above or below the DS's rows replayed
  at a clamped row 0 or 191. A touch is also written when only its
  out-of-band coordinate changed.
* **`hudmemo=1`, `fp2d=1`:** the §1 and §2 rules. Both change what the
  cartridge does with the same inputs.
* **`quick=1`:** the recording may hold `FRAME q ACTION` lines, the
  shortcuts of §3/4, each noted in the guest frame it ran and run in that
  frame on replay. An older build would read one as a key mask.

A recording without them replays the old rules exactly: `hit_unmap_legacy`
is the committed body verbatim, and `lb_fp_band_shift2`'s legacy branch is
docs/320's X-only rule. `LB_HUDHITMEMO=0` and `LB_FP2D=0` select the old
rules live, for A/Bs.

### A replayed release lost its out-of-band coordinate

The first replays of a new recording (the t383j session, rep383a/b) were
identical to each other but did not reproduce it: no building was placed.
With `LB_HUDHITLOG` (rep383c), the red brick's press mapped as live (8,52),
but its release arrived at native **4,0**. The list never opened, and every
later step went its own way.

The replay injector cleared the extended coordinate right after delivering a
release, while the cartridge reads it at its own poll, later. So a button
pressed in a band was released at the clamped panel value, off the button,
and the held re-test dropped it. Live, a release keeps the press's
coordinate (set_touch_from_mouse's note), and the next delivery without one
clears it. The injector now does the same for `exty=1` recordings. Older
ones keep the clear, so they replay as before.

After the fix (rep383d), the replay makes the same presses as the live run:
red brick 8,52, train 32,51, red brick 8,52, building 56,51. Then it places
the building, and the harvest at `+48,-64` is the live one. The band cells
were served (`served=96227`). Note: a recording made before this with
`hudhit=1` and a press out in a band has the same fault on replay, and still
has it.

## Verification

Every live run was a held Free Play battle at 1473x830 (a 736x414 surface,
band 240,111), driven through the frontend (`frontend_input`: real SDL
events, the player's path), with window shots.

* **The bricks** (run t383b, `exp-t383b/win/bricks-halves.png`: selected,
  then each click): a click on the right half of
  the drawn blue brick opened the blue list, and one on the left half of the
  red brick opened the red list.
* **The shortcuts, calling the button's callback** (run t383m, final build,
  `exp-t383m/win/shortcuts.png`; `LB_QUICKLOG` gives one line per shortcut
  and one per return).
  * Base selected: Mouse 4 opened its red list (widget 606), and Mouse 4
    again closed it.
  * Space deselected (628).
  * Builder selected: Mouse 5 opened the command list (605), and Mouse 4
    switched to the building list.
  * Right-click deselected.
  * Mouse 4 **with the mouse held down on the field** opened the list
    mid-press. The first build refused that as "pen input blocked" (run
    t383l), because it copied the pen path's check that the field owns the
    press (021581AC == 2).
  * Every call logged its return into the view update, and the building
    placement after it (real clicks) went through.
  * The route's own drag had selected the hero and a builder before the
    first shortcut, and Mouse 5 opened their command list.
* **A recording of that session** (`quick=1`, nine `q` events) replayed
  twice (rep383qa/b). Both clicked the same nine widgets in the same order
  as the live run, and the building placement between them reproduced. The
  counts were identical at 3000, 4500, 5500 and 6500.
* **Holding a building** (runs t383i and t383j, the press held 1.3 s on the
  second building, `exp-t383j/win/label-ab.png`). Before: nothing above the
  list, the press remapped to widget 14 mid-hold, and the next field click
  was a move order. After: the label reads **"Biodome (75)"** above the held
  building, with the label at -244,-127 like the list. The release selected
  the building and the ghost was placed.
* **The footprint in the top band** (runs t383j and t383k, the same route,
  the camera panned until the site is 40 rows above the DS's top,
  `exp-t383j/win/fp-band-ab.png`). With `LB_FP2D=0`, docs/320's rule: the
  checkmark sits in the band and only the footprint's rows inside the DS's
  192 are drawn. With docs/383: `shift=+48,-64`, 24 cells harvested, and the
  whole 2x2 footprint is drawn with the checkmark at its corner.
* **Gate** (the reference replay, which predates every new key): counts
  identical to gate370b at all six checkpoints, and the eight pictures under
  5972 byte-identical to gate381. This held for both builds: exp-gate383
  (before the shortcuts called the callback, 59.41 fps) and exp-gate383b
  (final, 59.82 fps). The view-update override is a pure tail call when no
  shortcut is pending.
* **Vanilla** (van383, van383b): 240/240 identical to van-hudmask.
