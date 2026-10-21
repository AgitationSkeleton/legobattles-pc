# 396 — Fixes from the first online playtest

Status: **built, gated, deployed with the next release.**

This round comes from the first playtest of an online match: two copies on
one PC, one deployed build and one unzipped v0.1.0-beta.1. Each section gives
the defect, what was measured, and the change.

## The window opens on the primary monitor

**Defect.** The window opened on the last display, not the main one.

**Cause.** The placement code (`apply_hooks.py`, frontend window creation)
was written for a test display. It always chose the last display, or one whose
mode matched a default of 1920x1080, and it always suppressed activation.

**Change.**

- A player's window goes on the primary monitor, the display whose desktop
  origin is 0,0, and takes focus.
- Only `LB_DISPLAY` or `LB_DISPLAY_W/H` choose another display, and only then
  is the window shown without focus.
- `tools/lb_exp.py` and `tools/lb_mp.py` now set `LB_DISPLAY_W/H` themselves.
- The log names each display and marks the primary one.

## The 12/25 Santa unlock

The cartridge reads the DS clock in exactly one place:

- `User::MainMenu` (`0x020BF0A0`) calls RTC_GetDate (`FUN_02115F90`).
- If the profile lacks record `0x1B8` and the date is 12/25, it unlocks
  `0x1B8`, Santa.
- No other function in the corpus calls the date reader.

The runner's clock starts at a fixed 2024-01-01 unless `--rtc-host` is
given. Players never had it, so Santa could not unlock.

**Changes:**

- **Player starts use the PC's local clock.** Both start paths now pass
  `--rtc-host`: the double-click start (the `main.cpp` stanza) and
  `play.bat`. Harness runs pass their own arguments and keep the fixed
  clock.
- **Recordings store the clock.** A recording made on the PC's clock writes
  the guest clock's seven BCD registers into its header (`rtc=`). A replay
  sets them on the same frame (the first VBlank), so it runs on the
  recording's date.
- **Clock accessors.** The registers are private to `io.cpp`, so a stanza
  adds `lb_rtc_get`, `lb_rtc_set` and `lb_rtc_is_host` there.

## WASD moves the camera; Space deselects

The DS X, Y and R buttons sat on S, A and W.

**Change.**

- X, Y and R move to C, V and E.
- W, A, S and D become D-pad aliases in `lb_binding_key_bit`, unless the
  player binds one of them to a button.
- A saved bindings file that still holds the old defaults for X, Y and R
  (S, A, W) is moved to the new ones.

Space was already a quick deselect (docs/383, "Battle shortcuts").

## The player name

**Defect.** The typed multiplayer name only reached the lobby server. The
other players saw the cartridge profile's name.

**What the game uses.** `FUN_020F5EC8` copies the profile name (`0x02154E2D`)
into two 11-byte lobby copies, `0x0215B7D4` and `0x0215B7E4`. It does this
when the Multiplayer menu opens and on a network reset. The host's lobby
record is built from the second copy (`FUN_020F5FC0`).

**Change.**

- With "Use the save's profile name" off, the typed name (10 characters,
  printable ASCII) is written into both copies on every VBlank where they
  differ. That is after any copy the game makes.
- The simulation never reads these copies, so the write cannot desync a
  match.
- Verified: the host lobby shows the typed name.

**Writing it to the save.** `SaveGame` (`FUN_0204FA88`) writes the 11 bytes at
`0x02154E2D` into the save; its literal pool names that address. The new
"Write this name into the save" button renames the loaded profile once. The
game shows the new name at once and keeps it at its next save.

## HUD bricks fully zoomed out

**Defect.** At full zoom-out on a wide window, a click on the blue or red
brick became a move order.

**Measured** (run `t396hud3`, `LB_HUDHITLOG`):

- On a 1536x642 surface the DS's own 256 columns sat 740 columns from the
  left edge.
- The press reached the hit test as `-512,-214`. The extended touch x is
  clamped to -512..1023, a range from when the surface stopped at 1024
  columns.
- The anchored bricks are near x = -732, so no moved face held the point.
- Zooming in once brings the band under 512 columns, which is why that
  helped.

**Change.** The clamp is now the halfword's range, ±4096, on both axes.

**Verified** (run `t396hud4`): the same click maps as `press -732,-214: on a
moved face -> 11,27` and opens the blue brick's list.

## The multiplayer stall

**Measured** (`tools/lb_mp.py`, which now samples `frontend_stats` over its
linger). Two copies in a match ran at 31 fps each. Emulation took 29.3 ms a
frame, against about 10 ms for one copy alone. Present time was 2.7 ms and
drain was 0.

**Cause.** The wireless bridge's two blocking receives poll their queue and
sleep 1 ms between polls:

- the host waiting for its children's replies (`RecvReplies`);
- a child waiting for the host's packet (`RecvPacketGeneric`).

A sleep lasts at least one timer period, whenever the datagram arrived. The
game exchanges several datagrams a frame.

**Change.** The waits now block on the socket itself (`select`, with a 1 ms
ceiling so the deadline is still checked). They return as soon as a datagram
arrives.

**Result.** 54.3 fps on both copies, emulation 15.1 ms a frame.

**Left.** The remaining cost is the two copies waiting on each other's
progress. The DS protocol needs a reply in every exchange, so across the
internet the round-trip time, not the bridge, will limit the frame rate.
Real online play needs a latency-tolerant layer above the radio, as the
LSW2 port built. That is the next piece of multiplayer work.

## The menu, laid out like Ship of Harkinian's

The single floating panel of ten tabs is now SoH's modern menu
(`soh/SohGui/Menu.cpp`, `Menu::DrawElement`):

- the window dimmed behind it;
- a block centred in it, 90% of a large window and never wider than 16:9;
- a header of categories: Settings, Enhancements, Cheats, Network, Tools;
- Quit, Reset and Close at the header's right;
- a white rule, then a sidebar of the category's sections, a second rule,
  and the section's controls, a column wide.

The sections are the old tabs' own drawing code. In menu mode `tab_begin`
answers "is this the section on show" and `tab_end` ends nothing.
`LB_PANEL_TAB` still names one.

**Reset** (also Ctrl+R, as in SoH) restarts the game from its title screen in
the same window:

- It runs the runner's own power-on `boot()`, the one the debug server's
  `reset` command uses, at the frontend's between-frames point after
  `debug_pump()`.
- `boot()` reloads the ARM9 image, so `lb_port_on_guest_reset` puts the
  port's once-only state back: the one-shot patch sites, re-armed through a
  boot generation counter; the blueprint search, with the balance pack and
  builder state it feeds; the per-level tables; and the recording, which
  starts a new file.

**Verified:**

- After a Reset from a battle the title screen comes up.
- The touch patches re-apply, the blueprints are found again, and a new
  recording begins.

## Typing on the game's keyboards

The profile-name keyboard (`User::CreateNewProfile`) and the cheat-code one
(`User::CheatKeyboardScreen`) are two copies of one design, read from their
`virtual_08` (`0x020B6D4C`, `0x020B4B14`) and their key callbacks
(`0x020B71A4`, `0x020B4FDC`):

- 29 key widgets, ids `0x23D..0x259`, at `screen + id*4 - 0x89C`;
- the text being typed at `screen+0x44`, OK at `+0x54`;
- every label is a LOC string id from an 8-byte-per-key table;
- one callback per screen appends the label (`0x258` erases, `0x250` changes
  case, `0x259` adds a space), up to 10 characters for a name and 7 for a
  code.

**Change.**

- A typed character becomes the key whose label is that character.
- The key is pressed by entering its own callback with a click's PenEvent,
  in front of the shared screen update (`ScreenType::virtual_09`,
  `0x020E1BF4`). This is a tail call that returns into the update: the
  docs/383 mechanism.
- Backspace erases and Enter is OK.
- While a keyboard screen is up, the letter, digit, Space, Backspace and
  Enter keys are not DS buttons.
- Typed keys are recorded as `q` events (`0x10000 | char`) and replay.
- `LB_TEST_TYPE` is a test aid that types a string the first time a keyboard
  comes up.

**Verified:** "Tester2" typed into the profile-name keyboard. The game
capitalised the first letter, as it does for taps.

## Make All Builders The Same: a choice of builder

The checkbox copied the Imperial builder's 14 stat fields to the others. It is
now a dropdown: Off, King, Wizard, Pirate, Imperial, Earth or Alien.

- The snapshot and revert now cover all six builders, since any of them can
  be overwritten.
- The copy is taken from the chosen builder as the game has it, after any
  balance pack, as before.
- A settings file with the old `same_builders: true` reads as Imperial.

**Verified.** With Wizard chosen, all six builders carry the Wizard's
attack interval of 24; the Imperial builder's own is 30.

## Not reproduced

**The host lobby's back arrow** answered in every harness run:

- default size;
- a 1728-wide surface;
- the playtest's own 1146x460 surface;
- with local wireless on and advertising;
- zoomed out while the lobby was open.

Each time it showed "This will terminate the current game", and Yes
withdrew the lobby. The playtest's own recording shows back-arrow presses
that reached the game without effect, so the condition is real but not yet
known.

**Not yet looked at closely:** the build-placement preview splitting, and the
move cursor sinking into the shroud at some sizes. Both need a picture of the
moment.

## Gate

The build before the keyboard and builder changes passed:

- rep396a == rep396b, and equal to rep395a;
- gate396 counts equal gate370b;
- 8/8 pictures equal gate381, by pixels;
- 60.17 fps;
- van396 240/240.

The final build passed the same gate:

- rep396ma == rep396mb, and equal to rep396a;
- gate396m counts equal gate370b;
- 8/8 pictures equal gate381;
- 59.74 fps;
- van396m 240/240.

**Read the log before blaming the build.** One run in between (gate396k)
failed: its counts differed and its pictures were missing. Its log showed the
window being resized mid-run (the surface stepping from 736 to 864 columns)
and the Team Builder section being drawn. Someone had used the test window;
the re-run passed unchanged.
