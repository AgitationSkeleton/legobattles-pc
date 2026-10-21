# 398 — The rename is saved, and number keys follow the blue menu

Status: **built, gated, deployed with the next release.**

## "Write this name into the save" now writes it

**Defect.** A player pressed the button to rename their profile. The game showed the new name, but the next start loaded the old one again.

**Cause.** The button (docs/396) changed only the profile name in RAM (`0x02154E2D`). The game writes it at its next save, and a player who renames and then quits never reaches one. The log of that session ends `profile renamed to ...`, with no save after it.

**Why the port does not write the file itself.** `SaveGame` (`FUN_0204FA88`) writes the record bit-packed:

- it writes the name first (`FUN_0204B854(name, 11)`);
- then field after field through `FUN_0204BA48` (encode) and `FUN_0204BAB0` (write);
- then it finishes with `FUN_0204BB84`.

The checksum is still unknown (docs/97, docs/176), so a forged file would be refused.

**Change.** The rename asks the game to save, using its own call:

- **The call.** It is `FUN_0204FA6C(0)` (ARM): SaveGame with R0 passed through, then `FUN_0204B538(0)`. `User::MainMenu::virtual_12` makes exactly this call after the 12/25 unlock.
- **Where it runs.** On the main menu, in front of `User::MainMenu::virtual_09` (`0x020BF230`, vtable `0x0214D00C`, reached only through the vtable). It is a tail call that returns there, the docs/383 mechanism.
- **Why the main menu.** A profile is certainly loaded there, and the game itself saves there.
- **Why not the shared menu update.** The shared update (`0x020E1BF4`) would not do: MainMenu's update calls it directly, so an override on it never fires on the main menu.
- **Conditions.** Only with no wireless session (`0x0215B7BC` = 0 or -1). A rename asked for anywhere else waits for the main menu.
- **Test aid.** `LB_TEST_RENAME=<name>` asks for the rename the first time the main menu is up.

**Verified:**

- Run `t398ren` renamed Tester to Tester2 and logged `the game saved the profile (result 0)`.
- The save changed in its record head, bytes `0x10..0x16`.
- Run `t398ren2` booted from that save: the profile name in RAM is the new name.

## Number keys follow the blue menu

**Request.** While the blue brick's menu is the open one, the number keys should pick from it, not from the red, yellow or white brick's menu.

**What the game has:**

- The open list is `screen+0x26C`: 0 blue, 1 white, 2 yellow, 3 and 4 red, 5 none (`FUN_020DA6BC`).
- The blue list's items are widget ids 614..623. They go through the bricks' own callback `FUN_020DC298`, which `FUN_020DA2B8` hands to list 0.

So the docs/385 item mechanism works for them unchanged.

**Change:**

- A new option, Input > "Number keys pick from the open blue menu", is on by default.
- With it on and the blue list open, 1-9 click item N of the blue list.
- The choice is made in the view update and recorded as made: actions 21..29 are blue items, beside 11..19 for the second brick. A recording therefore replays the same whatever the setting.

**Verified** (run `t398blue`, `LB_QUICKLOG=1`):

- With the hero's blue list open, 1 logged `item 1 of list 0: clicked (widget 619)`. The list closed with the command armed, as a tap does.
- With it closed, 1 opened the yellow list and picked its first spell (widget 680), as before.

## Gate

The final build passed:

- rep398a == rep398b, and equal to rep397a;
- gate398 counts equal gate370b;
- 8/8 pictures equal gate381, by pixels;
- 59.95 fps;
- van398 240/240 against van-hudmask.
