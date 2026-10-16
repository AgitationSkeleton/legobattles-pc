# 356 — A cosmetic name for the Space Criminal Leader

Status: **built into the port and seen in game** (2026-09-24, section 8). It
was first implemented and tested off-target, as recorded here: The new code was compiled on its own (g++ 16.2,
`-std=c++20 -Wall -Wextra`, no warnings) against a mock guest bus and all eight
`LOC/*.lng` files out of the ROM. Both edited sources also pass a syntax-only
compile against the assembled runner tree's headers (section 7). The badge is
UNSEEN until section 6's check has been watched.

The feature: an imgui Enhancements dropdown, "Change Space Criminal Leader
Name", with options Off / Kranxx / Plisken. It replaces the text "Space
Criminal Leader" with "Kranxx" or "Plisken" cosmetically, on the local player's
end only; it must not affect gameplay or online games.

The cartridge already names him: string 2697, the briefing of the Alien
mission Tech Thief, opens "Plisken, the leader of the space criminals, …",
and each of the six translations names him Plisken too.

## 1. Where the words are

All of the text is in the eight language tables, `LOC/<language>.lng`. They are
uncompressed and read here straight out of the cartridge's FAT:

| language | index | FAT id | ROM offset | bytes | the name (ids 556 and 2854) |
| --- | ---: | ---: | --- | ---: | --- |
| American_English | 1 | 779 | 0x1B8A00 | 79,308 | Space Criminal Leader |
| UK_English | 2 | 786 | 0x24D000 | 78,892 | Space Criminal Leader |
| French | 3 | 781 | 0x1E0400 | 86,876 | Leader des criminels spatiaux |
| Italian | 4 | 783 | 0x20E000 | 89,164 | Capo dei criminali spaziali |
| German | 5 | 782 | 0x1F5800 | 100,036 | Weltall Kriminellen Oberhaupt |
| Spanish | 6 | 785 | 0x237200 | 89,204 | Líder de los criminales espaciales |
| Danish | 7 | 780 | 0x1CC000 | 82,924 | Rumskurkenes boss |
| Japanese | 8 | 784 | 0x223E00 | 78,400 | 556 "Criminal Leader", 2854 "Space Criminal Leader" |

The index is the game's own language number (section 2).

**Format** (`tools/lb_lang.py`, confirmed): `'LANG'`, u32 `0x3B20`, u32 0, then
u32[3064] byte offsets from the start of the file, then NUL-terminated strings.
The text starts at 0x2FEC in every file, which is 0x0C + 3064 × 4. Many ids share
one string. **The bytes are UTF-8**, for example `C3 A9` for é, `C3 AD` for í and
`E2 80 99` for ’. `lb_lang.py` decodes them as Latin-1, which is why it prints
"arenât". Kranxx and Plisken are plain ASCII, so that difference does not
matter here.

**Which ids carry him.** Id 556 is the display name of blueprint 133,
`SpaceCriminalLeader` (its +0x06; docs/11). Id 2854 (0x0B26) is the name of
cheat record 51, code ZVDNJSU (docs/170). Nine sentences in the Alien campaign
mention him (English shown):

| ids | shipped | → Kranxx |
| --- | --- | --- |
| 556, 2854 | Space Criminal Leader | Kranxx |
| 2676 | Rescue the Space Criminal Leader from the Earthlings. | Rescue Kranxx from the Earthlings. |
| 2678 | Free the captured Space Criminal Leader. | Free the captured Kranxx. |
| 2679–2681 | Bust out the Space Criminal Leader | Bust out Kranxx |
| 2682–2684 | Get the Space Criminal Leader back to Base | Get Kranxx back to Base |
| 2685–2687 | The Space Criminal Leader died | Kranxx died |
| 2697 | Plisken, the leader of the space criminals, has … | Kranxx, the leader of the space criminals, has … |
| 2702–2704 | Use the Space Criminal Leader Hotwire ability on the Claw Tank. | Use Kranxx's Hotwire ability on the Claw Tank. |
| 2767–2769 | Defend the Criminal leader | Defend Kranxx |
| 2770–2772 | The Space Criminal leader is dead. | Kranxx is dead. |

Plisken gives the same strings with "Plisken" in place of "Kranxx", and leaves
2697 as shipped. `docs/data/overlay_inventory.tsv` places the objectives in
three missions of the Alien campaign: Act 2 Mission 1 Criminal Minds (`ov001`,
2679/2682), Act 2 Mission 3 Tech Thief (`ov007`, 2702) and Act 3 Mission 4
Freedom (`ov008`, 2767).

**Occurrences per language.** "Space Criminal Leader" as text exists only in
the English-text tables: UK_English and American_English (9 hits each), and
Japanese.lng, which in this European ROM holds English text for these ids (10
hits, because there 556 and 2854 are separate strings). The five translations
never say it. They carry the name in their own words at 556/2854, and describe
him in their mission sentences ("le leader des criminels spatiaux", "das Weltall Kriminellen
Oberhaupt", …).

**Raw search of the whole ROM**, case-insensitive: 36 hits. 28 are those
language-table strings. The other 8 are file names in the FNT
(`SpaceCriminalLeader.NANR/NCER/NCGR/NCLR` under `UI/FEPlayerCards/` and
`UI/GamePlayerCards/`). There is no copy in the ARM9, the ARM7 or any overlay,
and no UTF-16 copy. "Kranxx" does not occur anywhere.

**Not text.** The two player cards and `UI/Heroes/SpaceCriminal_Victory` and
`_Defeat` were rendered from the cartridge's own files (their decompressed
copies in `out/eur/plain/`). They are pictures of him with no lettering, so
nothing he is called is baked into his art. The cutscenes
(`Movies/*.mods`) were not inspected.

## 2. How the game loads and reads them

The language manager is a static block at **0x0215378C**:

| field | meaning |
| --- | --- |
| +0x00 u16 | handle of a load in flight, 0 when none |
| +0x04 u32 | the language asked for, 1..8 as in the table above |
| +0x08 u32 | the language loaded |
| +0x0C u32 | the loaded `.lng`, one heap block |

- `func_0203DFB0(lang)` switches over 1..8 to a path (literal pool
  0x0203E08C..A8) and starts an asynchronous load, `func_020EFE80`. The load's
  callback, `func_0203E114`, is bound to the language number.
- `func_0203E114`: if the language that finished loading is still the one asked
  for, it frees the old block (`func_0204CA50`), stores the new one at +0x0C and
  its number at +0x08, and relocates it. If not, it discards it. Either way it
  then clears +0x00.
- `func_0203E0B4` relocates in place: file+0x08 := file+0x0C, then
  `table[i] += file` for i < 0xBF8. From then on the table holds absolute
  pointers.
- `func_0203E0FC(id)` is `return table[id]`. Its only caller, `func_0203E1D4`,
  copies the text into a String with `func_020F1C88`: clear, strlen, resize,
  memcpy.
- `func_020BEBFC` loads language 1 while +0x08 is still 0. `func_020AD5D8`,
  `func_020BC540` and `func_020C245C` (the last reads the options record) call
  the setter, so changing the language at run time replaces the block.

So the table lives at `*(u32*)0x02153798`, and string `id` is at
`*(u32*)(that + 0x0C + 4*id)`. Text widgets keep a string **id** and fetch the
text again when they are rebuilt (`func_020E97B4`, and the TextWidget methods
that read the id from +0x18 of the widget's definition). Nothing keeps a
pointer into the table.

## 3. Who else can see them

**Nothing that computes.** 14 literals in the ARM9 point into the manager. The
five at its base are in the manager's own functions (`0203DF90`, `0203DFB0`,
`0203E0B4`, `0203E0FC`, `0203E114`). Eight point at +0x08 and one at +0x04, and
every one of them is loaded and dereferenced once, at offset 0, as the language
NUMBER: compared, stored in the options record (`func_020B706C`), or bit-tested.
None points at +0x0C. No overlay holds any of these literals, or calls the
setter, the getter or the copy. The copy's 22 call sites are in 18 functions,
all of them UI: Options, the cheat keyboard and its top screen, profile
creation, the rewards slideshow, skirmish faction select, the skirmish game
setup, a unit info panel (`func_020C146C`), the in-battle HUD text formatter
(`func_020DCD34`), the cheat applier's display name (`func_02009158`), two
dialog builders, a popup, TextWidget, the word-wrap layout and the manager's
own `^`-format builder.

**Online play.** The port does not speak the game's protocol. `lb_net.cpp` is a
directory client, and the bridge carries the cartridge's own DS-wireless frames
(docs/216). No port code hashes guest RAM on that path. The frames carry lockstep
commands (docs/87–89), whose serialisers write command fields; text is not
among them. The multiplayer game start, `func_020F5168`, names each player from
the lobby record (`func_020F17A4` over the records at 0x0215B814, 0x2C bytes
per player), not from this table. The one player name taken from the table is
the skirmish AI's: `func_020C0EB0`, the single-player setup, gives player 1 a
blueprint's display name.

**The cartridge's own sync check is never sent.** `SyncCheckCommand` and
`SyncStatusCommand` each have exactly one constructor, called only by the
receive factory `func_02004DFC`. The factory's only caller is the receive loop
`func_02082F38`, and `Write32` (their field encoder) has no other caller.
Commands that are sent have a second, parameterised constructor behind a
helper: MoveEntities has `func_020067A4`, called from `func_02005884`. A
hand-written byte emitter for type 5 has not been ruled out exhaustively. Either
way, no checksum reads text.

**Port-side consumers that do see it**, both only while the setting is on:

- `LB_RAMSUM` (lb_shadow.cpp) hashes all of main RAM, so its hashes differ.
- The exactness gate. Copying and drawing a shorter string retires fewer
  instructions.

Run both with the setting Off. Off from boot is untouched (section 4).

## 4. The mechanism, and why this one

**Chosen: rewrite the guest-RAM copy of the table**, from the per-VBlank hook,
like the other `apply_*` patches. `apply_leader_name` in `lb_patches.cpp` is
called from `lb_patches_on_vblank`, before any of its early returns.

- It is one place. Every fetch copies from this block, so a rewrite reaches
  every screen that asks afterwards, in every mode.
- The replacements are shorter than every string they replace. The new text is
  written in the table's own format, NUL-terminated, and the rest of the old
  storage is zeroed so that a reader resumed part-way along still stops. No
  offset moves, and no other id points inside a rewritten string. The latter
  was checked across all eight files.
- Every edit keeps the shipped bytes and what it wrote. Off, or a change of
  name, puts the shipped bytes back **only where our text is still what is
  there**.
- **Off from boot is untouched.** With the setting at 0 and nothing of ours in
  guest RAM, the function returns before its first guest read.

The rules:

- The **name** (556, 2854) is rewritten whole, and only when it is one of the
  seven shipped names in section 1.
- A **sentence** is rewritten when its id is in a listed range and it contains
  the listed needle: "the Space Criminal Leader", "The Space Criminal leader",
  "the Criminal leader", or "Plisken" in 2697. The needle gives way to the name
  and an optional tail (the "'s" of 2702–2704). An article goes with the title,
  so the result reads "Bust out Plisken".

**Guards.** A table is used only when it is settled:

- +0x00 is 0, and +0x04 equals +0x08.
- The block is in main RAM, starts with `LANG`, and has been relocated
  (file+0x08 == file+0x0C).
- An entry that is still a raw offset is skipped and retried next frame, because
  `func_0203E0B4` may be part-way through the table.

When the table changes (freed, reloading, another language), every record of
ours is dropped **without writing**: that memory is no longer the table. When
a reload lands in the same block, our text is gone from it, so the record is
dropped and the name is applied again.

**Not chosen:**

- **Patching the file as the cart reads it** needs a hook in the runner's
  cartridge-read path, which this port can change only through
  `apply_hooks.py`. It would only take effect at the next load, at boot or on
  a language change.
- **Swapping at the draw call** has no single draw call to swap. The text is
  copied into Strings at 22 sites, and an override on `func_0203E0FC` would put
  a host dispatch on every string the game fetches, Off included.

## 5. What it cannot reach

- **Copies already made.** A screen that fetched the text before the change
  keeps it until the screen is rebuilt. Reopen it, or select the unit again.
- **Translated sentences.** The name is replaced in every language. The mission
  sentences are rewritten only where they are English: UK_English,
  American_English and Japanese.lng. The five translations keep their own
  descriptive wording, which remains true of him.
- **Pictures and video.** None of his art carries text (section 1). The
  cutscenes were not inspected, and any words burned into a video are out of
  reach.
- **A one-frame race, theoretical.** `func_020F1C88` does strlen and then
  memcpy. If the scheduler yields between the two at the very frame the rewrite
  lands, that one copy holds the new name followed by NULs. It is cosmetic
  either way.

## 6. How to verify it in game

**The setting.** Esc or F1 opens the panel. At the bottom of the Enhancements
tab is "Change Space Criminal Leader Name (this machine only)", with Off,
Kranxx and Plisken. Headless: `LB_LEADER_NAME=0|1|2` (Off, Kranxx, Plisken),
which overrides settings.json without writing it. Launch from PowerShell; the
Bash tool drops environment variables at the .exe boundary.

**The log.** When the name is applied:

```
[lb_leader] "Plisken": 9 string(s) rewritten in the language table at 0x02XXXXXX
```

The count names the language:

| tables | Plisken | Kranxx |
| --- | ---: | ---: |
| UK / US English | 9 | 10 |
| Japanese.lng | 10 | 11 |
| the five translations | 1 | 2 |

Switching to Off prints `[lb_leader] shipped text restored in N string(s)`. With
Off from boot, **no `[lb_leader]` line at all.**

**A. The cheat keyboard.** This is deterministic, and docs/181 has the route and
the coordinates:

1. title → profile → name → [the Single Player picker] → back (18,172) → main menu.
2. Extras (48,120), twice.
3. Cheat Codes (48,120). Like Extras, it may need a second tap to confirm;
   docs/181 does not say.
4. Type ZVDNJSU: Z (44,118), V (116,118), D (80,94), N (164,118), J (176,94),
   S (56,94), U (164,70).
5. Ok (213,175). It appears only after the seventh letter.

The top screen then reads "Cheat Code / Plisken / Accepted!", or Kranxx.
With Off it reads "Space Criminal Leader". docs/181 saw this format with
"Ninja". Accepting the code also unlocks him in that profile, so use a scratch
save.

**B. In a battle** (read from the code, not yet seen):

1. `demos/route-free.lbdemo` (docs/221) reaches a Free Play battle. The
   harness's Free Play runs log the player's own hero as
   `owner=0 cat=0 bp=133`, which is SpaceCriminalLeader.
2. Select him, as docs/215 did with portrait and 1100/1100 on the top screen.
   The info panel, `InfoTab` (`func_020D33D8`), sets its name widget from the
   selected entity's display name, id 556.
3. After changing the setting, select him again.

**C. The campaign text.** Alien Act 2 Mission 1, Criminal Minds: "Bust out
Plisken", "Get Plisken back to Base". Act 2 Mission 3, Tech Thief: "Use
Plisken's Hotwire ability on the Claw Tank." With Kranxx, the Tech Thief
briefing (2697) opens "Kranxx, the leader of the space criminals". Act 3
Mission 4, Freedom: "Defend Plisken". This route needs campaign progress.

**Exactness gate.** Run it with the setting Off and `LB_LEADER_NAME` unset. The
counts must equal the parent commit's, because the function returns before its
first guest access.

**Two machines** (optional). One instance on Plisken and one Off, with
`tools/lb_mp.py --desync` (docs/227), should stay in sync, with the name
differing on one screen only.

## 7. What was tested here, and what was not

Tested off-target:

- The exact block from `lb_patches.cpp`, copied out byte for byte, was compiled
  standalone against a mock 4 MB bus. It was fed each `.lng` from the ROM,
  placed and relocated the way `func_0203E0B4` does.
- Its output was compared with an independent Python model of the same rules.
  They are identical for 8 languages × 2 names × 28 ids.
- `lb_overlay.cpp` and `lb_patches.cpp` pass `g++ -fsyntax-only` with the
  project's flags against the headers of the assembled runner tree. The only
  diagnostics are warnings at four lines this change does not touch.

The scenarios, all passing:

- Off from boot: 0 guest reads, 0 writes.
- No table, and a load in flight: 0 writes.
- Both names in every language, and then Off restores every table byte for byte.
- A reload into the same block is applied again.
- Kranxx ↔ Plisken switches live, including 2697.
- A language change with the old block freed: nothing is written into it.
- A part-way relocation is finished on the next frame.
- No access outside main RAM.

Not tested:

- the port build and the real bus;
- the game drawing the new name;
- routes B and C, which are read from the code and not seen;
- play between two machines.

## 8. Seen in the port (2026-09-24)

Cherry-picked onto main as `58688d77` and built (runner 01:21), with no errors or
warnings from these lines.

- **The log.** `LB_LEADER_NAME=2` on the harness's own Free Play route printed
  `[lb_leader] "Plisken": 9 string(s) rewritten in the language table at
  0x0229807C`. That is the English count section 6 predicted.
- **The screen.** The same route with `--select-hero`: once the hero is
  selected, the top screen's unit panel reads **"Plisken"** above the Space
  Criminal Leader's portrait. That is route B of section 6, seen, not read
  from the code.
- **Off is the cartridge.** The exactness gate, with the setting at its
  default Off and `LB_LEADER_NAME` unset, is identical at all six frames,
  and prints no `[lb_leader]` line at all.

- **Kranxx, on screen.** `LB_LEADER_NAME=1` logs `"Kranxx": 10 string(s)
  rewritten`, the count section 6 predicted, and the harness's window capture
  with the hero selected (`screenshots/exp-kranxx2/route/select-03.png`)
  shows the Top Screen panel's unit card reading "Kranxx" above the
  portrait. (That capture needed two harness fixes, to lb_capture.ps1 and
  lb_exp.py.)

The badge in the Enhancements tab is WIRED. Still not tried: the cheat keyboard
(route A), and two machines.
