# The music set

**Status: every music track in the game is enumerated with its stream id. The
music setting's data model is complete — all four planned modes can be
expressed against real track ids. What is still missing is the one place that
chooses a track for a multiplayer match.**

## Where it is

`Sound/sound_data.sdat`, 62,902,976 bytes, a standard SDAT. Its `SYMB` block at
offset `0x40` carries the names, and the music lives in the **STRM** table
(streamed audio), not `SEQ` — the `SEQ` table is empty and the 217 `SEQARC`
entries are sound effects.

**98 STRM entries, and the naming is completely regular.**

## The four world themes

```
id 0   STRM_COMBINED_FE_THEME
id 1   STRM_CASTLES_FE_THEME
id 2   STRM_PIRATES_FE_THEME
id 3   STRM_MARS_FE_THEME
```

`FE` is the front end. **Castles / Pirates / Mars are exactly the three worlds**
a map-specific mode needs (castle, pirate or space), so map-specific music has a
ready-made key.

## The six faction sets

```
faction    ids       campaign  battle  victory  defeat
KING        4..17       6        6        1       1
WIZARD     18..31       6        6        1       1
EARTH      32..48       6        9        1       1
ALIEN      49..65       6        9        1       1
PIRATE     66..79       6        6        1       1
IMPERIAL   80..97       7        9        1       1
```

Named `STRM_<FACTION>_CAMPAIGN_<n>`, `STRM_<FACTION>_BATTLE_<n>`,
`STRM_<FACTION>_VICTORY`, `STRM_<FACTION>_DEFEAT`.

**Six factions, and `docs/97`'s achievement table has `CompleteSide1Campaign`
through `CompleteSide6Campaign`.** Two tables read from different parts of the
ROM for different reasons agree on six, which is worth more than either alone.

Pairing them onto the three worlds is the obvious grouping — Castle takes KING
and WIZARD, Pirate takes PIRATE and IMPERIAL, Space takes EARTH and ALIEN — and
it is consistent with the world themes, but **the pairing has not been read from
the ROM** and is recorded here as inference, not fact.

## What each mode needs

| mode | what it selects |
| --- | --- |
| Vanilla | the KING set, ids 4-17, regardless of faction or map — the stock multiplayer behaviour |
| Faction-aligned | the set matching the player's faction |
| Map-specific | keyed off the world, with ids 1/2/3 as the theme |
| Random | any faction set |

`CAMPAIGN` is the calm track and `BATTLE` the combat track -- the King calm and
battle music -- and KING is the only set whose calm/battle
split is six and six with no extras.

**Note the asymmetry** — EARTH, ALIEN and IMPERIAL have **nine** battle tracks
and IMPERIAL has **seven** campaign tracks. A mode that assumes six of each will
silently never play the extras. Any implementation must read the count per
faction rather than hard-coding six.

## What is not established

**Where the game picks the track.** Nothing above touches code. The
premise — that multiplayer always plays the King tracks — is consistent with the
data but has not been confirmed by reading the selection site, and until that
site is found there is nowhere to hook a mode switch.

That is now the single remaining question for the music setting, and it is a code search
rather than a data one.

## The selection site is not where the obvious searches look

Four searches for it, all negative. Recorded because each one rules something
out and the next person should not repeat them.

**1. No `STRM_` name strings anywhere.** The SDAT's SYMB block names all 98
streams, but nothing in the ARM9 static image, the ARM7, or any of the 91
overlays contains the substring `STRM`, nor `CAMPAIGN`, `_FE_`, `THEME` or a
`%s_`/`_%d` format that could build one. So the game does **not** look streams up
by name, which is worth knowing because it *does* do exactly that for the
victory and defeat cues (`K_Victory`, `TrollKing_Victory` and the rest live as
plain strings at `0x0214D6F4`).

**2. No table of the six faction base ids.** The sets begin at 4, 18, 32, 49, 66
and 80, and that sequence appears nowhere as `u8`, `u16` or `u32`, in the ARM9 or
in any overlay.

**3. Not even strided.** A search for those six values in order at every constant
stride from 1 to 64 bytes, as `u8` and as `u16` — the shape they would take
inside a per-faction struct — finds nothing.

**4. Not in the map or mission tables.** `docs/data/maps.tsv` has no music
column, and `campaign_missions.tsv` carries `world` and `side` but no track
reference.

## What that leaves

The ids must be reaching the audio layer some other way. Two possibilities are
open and neither is checked:

* **Computed rather than stored.** The set sizes are 14, 14, 17, 17, 14 and 18,
  so the bases are their running sum. Code that walks a count table would never
  contain the bases themselves.
* **Handed to the ARM7.** Audio is the ARM7's job on this hardware, and the ARM9
  side may only post an index through the IPC FIFO, in which case the selection
  is a number in a message rather than a constant in either image.

The audio module is the place to look: `func_02089B64` loads
`Sound/sound_data.sdat` and the module runs from about `0x02089AAC` to
`0x0208A4CC`. `func_02089DE4` has 30 callers and allocates a 0x24-byte object per
sound, which makes it the most likely play entry point, but its arguments have
not been traced back to their call sites.

**The music setting stays at 85% understanding and 0% built.** Every track is enumerated and
all four modes can be expressed against real ids; what is missing is
still the single place that chooses one, and this pass narrowed where it is not
rather than finding it.

---

# Two more negative results, this time from the running game

The four searches above were static. `tools/lb_warp.py` and the debug server's
`run_to_pc` now allow the same questions to be asked of a live match, which is
what the earlier pass lacked. Both attempts failed, and both narrow things.

**5. `func_02089DE4` does not execute during steady-state gameplay.** Warped
into a Free Play match with music playing, `run_to_pc` on it for **3,000,000
rounds** never broke. So whatever it is, it is **not** a per-sound or per-frame
entry point -- it runs at transitions at most. The "30 callers, allocates a
0x24-byte object per sound, most likely play entry point" reading above is
therefore either wrong or true only of scene changes.

**6. It does not execute on input either.** Six tap-and-break trials on the
field, 400,000 rounds each, produced no hit. Taps that select a unit make a
sound in this game, so if `func_02089DE4` were the play entry for UI cues it
should have fired.

**Method note.** `fifo_sample` returned `{"found": false}`, so the IPC hypothesis
was not tested rather than disproved -- that command samples a captured message
and none was captured, which is not the same as no traffic.

## What to try next, given all six

The remaining shape that fits every negative result is that the ARM9 posts a
**number** and the ARM7 owns selection. The way to see that is not a breakpoint
on an ARM9 function but a **watch on the IPC registers or the ARM7 side**:
`LB_WATCH` (`docs/109`) takes any range, and `0x04000180`-`0x04000188` is
IPCSYNC and IPCFIFO. A capture across a track change -- entering a match, which
`lb_warp.py` does reproducibly -- would show the value crossing if one does.

That is a concrete experiment rather than another search, and it is the first
one available now that was not available when the four static searches were run.

## 7. The IPC hypothesis is now observed, not just plausible

`LB_WATCH=4000180:4000190` across a full `lb_warp.py` run into a Free Play match
captured **73 writes, all to `0x04000188`** -- IPCFIFOSEND -- from exactly two
program counters:

```
pc = 0x037FE490   x50    (ARM7 -> ARM9)
pc = 0x02111D48   x23    (ARM9 -> ARM7)
```

with command words such as `0000C007`, `C0000006`, `C0200006`: a tag in the high
bits and a small value in the low ones.

`0x02111D48` sits inside **`func_02111CD4`** (136 bytes), the ARM9-side FIFO
send, which has **10 callers**, all in the `0x0211xxxx` library region -- the
NitroSDK layer rather than game code.

So hypothesis 2 above is no longer speculation: **the ARM9 does hand values to
the ARM7 across the FIFO**, and the senders are library entry points. It does
not yet show that a *track id* is among them, but it establishes the channel and
gives ten concrete functions to attribute.

### Why this matters for the four negative searches

Searches 1 to 4 looked for track ids as **constants in the ARM9 image**, and
search 5 and 6 looked for an ARM9 **play function** executing. If selection is a
value assembled into a FIFO word by library code, none of those six could ever
have found it -- which is consistent with all six failing.

The next step is to attribute the ten callers of `func_02111CD4` and find which
belongs to the sound driver, then read what it packs into the word. That is a
bounded list rather than an open search.

## 8. The FIFO carries a POINTER, not the data

Breaking at `func_02111CD4` (the ARM9 FIFO send) twenty times during a warp gives
an identical picture every time:

```
r0 = 0x00000006      a subsystem tag
r1 = 0x03000000      shared WRAM
lr = 0x02114F08      inside func_02114EEC, one of the ten callers
```

So the ARM9 does **not** post a track id. It writes a command block into **shared
WRAM** and posts a *pointer* to it with a tag. That is why searches 1-4 could not
find an id as a constant and searches 5-6 could not catch a play function: the
value is assembled into a buffer, and the only thing crossing the FIFO is where
to find it.

**Negative result 9:** `LB_WATCH=3000000:3000040` across a full warp captured
**zero** writes. So the block is not written through the watched bus path at that
address -- either the command is built elsewhere and `0x03000000` is only a
mailbox the ARM7 reads, the writes go by DMA, or the region is reached through a
different mapping than the watch sees. Untested rather than disproved, as with
`fifo_sample`.

**Next:** read `func_02114EEC` and find where it gets the buffer it publishes.
That is one function, and it stands between the game's choice of track and the
ARM7 -- the narrowest the search has been.

---

# 10. FOUND. The selection site is a jump table, which is why six searches missed it

Searches 1-4 looked for the six faction base ids (4, 18, 32, 49, 66, 80) as
**data** in the ARM9 and the overlays -- as `u8`, `u16`, `u32`, and at every
stride from 1 to 64. Searches 5 and 6 looked for an ARM9 **play function**
executing. Searches 7-9 followed the IPC FIFO and found it carrying a pointer.

Every one of those is consistent with what is actually there, which is why they
all failed:

```
func_02089c38(r0 = side, r1 = kind)     a pure function; returns the STRM base
    a nested jump table, and every arm is `movs r0, #imm ; bx lr`
```

`movs r0, #0x42` puts 0x42 in the **low byte of a 16-bit instruction**, next to
an opcode. It is not in a table, it is not word-aligned, and it does not appear
at any stride, because the arms are separated by whatever the branch layout
needs. A search for the values as data could not have found them.

Reading the twenty-four `movs r0, #imm ; bx lr` pairs in order, out of
`section_static_02000000.bin`:

| side | calm | battle | victory | defeat | set |
|---|---|---|---|---|---|
| 0 | 4  | 10 | 16 | 17 | KING |
| 1 | 18 | 24 | 30 | 31 | WIZARD |
| 2 | 66 | 72 | 78 | 79 | PIRATE |
| 3 | 80 | 87 | 96 | 97 | IMPERIAL |
| 4 | 32 | 38 | 47 | 48 | EARTH |
| 5 | 49 | 55 | 64 | 65 | ALIEN |

**Every base matches the SDAT table at the top of this document**, including
both of its irregularities: IMPERIAL has seven calm tracks (80..86) and nine
battle (87..95), which is why its victory lands on 96 rather than 92; EARTH and
ALIEN have nine battle tracks each. Two readings taken from different parts of
the ROM for different reasons, agreeing on all twenty-four numbers.

`kind` is 0 = calm (`CAMPAIGN`), 1 = battle, 2 = victory, 3 = defeat. Out of
range returns -1 (`movs r0,#0 ; mvns r0,r0`).

## And the side is one word in RAM

```
0x0214B41C + 0x0C   the SIDE, 0..5     written by func_020898e8(side)
0x0214B41C + 0x08   the KIND, 0..4     written by func_02089908(kind)
0x02155494          the runtime block; +0x0C, +0x10, +0x18 are the re-pick state
```

`func_02089974` reads both words and calls `func_02089c38` with them.
`func_020897f4` calls it once the current stream passes half its length, so a
side change takes effect at the next track boundary rather than cutting.

`func_020898e8` is six instructions:

```c
if (side != state[0x0C]) {
    state[0x0C] = side;
    rt[0x0C] = rt[0x10] = rt[0x18] = 0;   // force a re-pick
}
```

The public setters are `func_02088980` (side) and `func_0208898c` (kind), and
their callers are named: `Battles::User::HostLobby::virtual_08`,
`ClientLobby::virtual_08`, `SkirmishFactionSelect::virtual_08`,
`MainMenu::virtual_12`, `CampaignSetup::virtual_08`.

## The premise, measured

The premise: vanilla multiplayer always uses the King calm and battle tracks
regardless of faction or map.

Confirmed, and not only for multiplayer. In a Free Play match on a **KingTileset**
map with an **EARTH** builder on the field, the game's own side word reads **0
(King)**:

```
LB_MUSIC_MODE=0   0214B41C  00000002 0000007F 00000001 00000000
                                                       ^ side 0 = King
LB_MUSIC_MODE=2   0214B41C  00000002 0000007F 00000001 00000004
                                                       ^ side 4 = Earth
[lb_music] Faction-aligned -> side 4 (Earth), was 6
```

## The music setting is built

`port/runner/src/lb_music.cpp`. All four modes, and the Music tab's badge is
WIRED rather than READ.

* **Vanilla** writes nothing at all -- the "off" position is the game untouched.
* **Faction-aligned** reads the side off the local player's **BUILDER**, as the
  design specifies. There is exactly one builder blueprint per faction
  (category 1) at indices 2, 22, 42, 62, 82, 102 -- twenty apart, in the order
  King, Wizard, Pirate, Imperial, Earth, Alien, which is the SAME order as
  `func_02089c38`'s sides. `side = blueprint_index / 20`. A builder is the right
  key because every side has one and a campaign mission can start without a
  hero.
* **Map-specific** reads the **tileset the level actually loaded**.
  `docs/data/maps.tsv` says all 122 maps use exactly one of KingTileset (41),
  PirateTileset (41) or MarsTileset (40), so the world IS the tileset. The names
  appear three times over in RAM -- a static table near 0x0214BA20, the front
  end's asset lists near 0x02176000, and once in the heap above 0x02300000 for
  the one actually loaded. Only the last moves with the level, so only the last
  is read. Which of the two factions in a world's pair to use is a CHOICE and is
  marked as such in the source; the cartridge has no "castle music", only two
  castle factions.
* **Random** picks once per match, seeded from a match counter rather than a
  clock, so a harness run stays reproducible while the same map does not play
  the same "random" side forever.

## Two harness capabilities this needed, both general

* `LB_WATCH_VAL=n` -- keep only accesses carrying a value. The sound module
  clears a whole flag array every frame from one instruction, which refills the
  512-entry watch ring inside a single frame.
* `LB_WATCH_NOTPC=a,b,c` -- the complement of `LB_WATCH_PC`. "Who ELSE touches
  this?" is the question that comes up, and it fails silently when two or three
  per-frame writers evict the one-off write being looked for. Measured on the
  sound globals: three PCs accounted for all 512 events.

Plus `--peek HEXADDR:HEXLEN`, `--findstr TEXT` and `--audio` on `tools/lb_exp.py`,
so a one-off question about guest memory no longer needs its own flag.

## What is NOT established

Whether the change is **audible**. The side word moves, the game's own re-pick
state is cleared exactly as `func_020898e8` clears it, and the next
`func_02089974` will call `func_02089c38(4, kind)` instead of `(0, kind)`. That
is the whole of the cartridge's selection logic. But no run so far has been long
enough to cross a track boundary and confirm the STRM id that reaches the ARM7
changed, and until one has, "the setting is wired" is the claim -- not "the
music changed".


## 11. It is audible, and the control is what proves it

"The side word moves" is not "the music changed", and the difference cannot be
settled by reading more code. `tools/lb_exp.py --audio-wav SECONDS` dumps the
SPU's own output to a WAV, so three runs of the identical deterministic Free
Play route -- differing ONLY in `LB_MUSIC_MODE` -- can be compared as sound.

The control is the whole design. The route's map uses **KingTileset**, so:

* mode 0 (Vanilla) leaves the game alone and the game picks **King**;
* mode 3 (Map-specific) reads the tileset and writes **King** -- the same side,
  by a completely different code path;
* mode 2 (Faction-aligned) reads the player's builder and writes **Earth**.

If the setting did nothing, all three would sound alike. If it merely perturbed
the run, all three would differ. What actually happens, correlating the RMS
envelopes of 28 seconds of audio:

| pair | 1 s windows | 0.5 s | 0.125 s |
|---|---|---|---|
| Vanilla(King) vs Map-specific(King) -- **same side** | **+0.973** | +0.942 | +0.699 |
| Vanilla(King) vs Faction-aligned(Earth) | +0.725 | +0.646 | +0.391 |
| Map-specific(King) vs Faction-aligned(Earth) | +0.666 | +0.561 | +0.335 |

Two runs that select the same side by different means agree at 0.97. Either of
them against the run that selects a different side drops to 0.67-0.73. The
0.5 s envelope makes it visible without any statistics -- at 8.0 s the two King
runs read 409 and 426 while the Earth run reads 1231; at 10.5 s, 416 and 428
against 1127.

Both King runs also share a silence at 23.5-26.0 s that the Earth run shares
too, which is a scene boundary rather than a track one -- worth noting so the
gap is not mistaken for the effect.

**The music setting is finished.** The setting is wired, the wiring is read out of the
cartridge rather than guessed, and the result is audible.
