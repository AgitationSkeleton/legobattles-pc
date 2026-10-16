# 181 — The unlock bits, named by the game itself

Status: **the mechanism is established and verified; the full bit table is being
measured.** This is what docs/176 said the four Unlock settings were waiting for
— "which bit is which" — and it is now answerable by measurement rather than
guess.

## The route to the code screen, which was not written down

docs/97 and docs/99 name `CheatsScreen` / `CheatKeyboardScreen` but nothing
recorded how a player reaches them. Driven and captured:

    title -> profile -> name -> [the Single Player mode picker]
      back (18,172) -> MAIN MENU: five icons
         Single Player | Multiplayer | Extras (gift) | Trophies | Options (gears)
      Extras (48,120), twice -> six icons, whose labels are
         Character Collection | Help and Hints | Concept Art
         Cheat Codes (48,120) | Maps           | Cutscenes

The main menu is **not** the first screen after naming a profile — that is
already the Single Player picker, and the back arrow leads *up* to the real menu.
That is why the route was never found by tapping forward.

The keyboard is the same one the profile-name screen uses, so lb_warp's
`(20,70)` / 24 px pitch for row 1 carries over:

    QWERTYUIOP  x = 20 + 24i, y = 70
    ASDFGHJKL   x = 32 + 24i, y = 94
    ZXCVBNM     x = 44 + 24i, y = 118

An **Ok button appears at (213,175) only once seven characters are entered**, and
nothing happens until it is pressed. The first attempt typed a code, diffed, and
found 5082 changed bytes of nothing — because the code had not been submitted.

## The codes are what docs/170 said they were

Submitting `GJSGHOQ` puts this on the top screen:

    Cheat Code
    Ninja
    Accepted!

**The game names the unlock exactly as `docs/data/cheat_registry.tsv` does.**
That is an independent confirmation of docs/170's corrected field order and of
the name resolution built on it — the registry said record 35 is "Ninja" and the
cartridge agrees, out of its own mouth.

It also settles what kind 8 is. Its 56 coded records are 17 cheats followed by
**39 unlockable characters and vehicles** — Ninja, Dwarf King, Islander, Tiki
Golem, Space Police Cruiser, Conquistador, Sheriff, Forestman, Ghost, Cavalry,
Brickbeard's Bounty, Battleship, Catapult, Alien Strike Ship and the rest.

## What an accepted code writes

Diffing only the profile record docs/176 located, across each accept:

    code      name                    field A       field B
    GJSGHOQ   Ninja                   +0x0BB.b5     +0x0DF.b4
    ITDXPMQ   Dwarf                   +0x0B6.b4     +0x0DA.b3
    UGDRSQP   Islander                +0x0B9.b2     +0x0DD.b1
    AVMPWHK   One Hit Kill (Heroes)   +0x0C1.b5     +0x0E5.b4
    QMSLPOE   Fast Building           +0x0C1.b6     +0x0E5.b5
    JRTPASX   Fast Magic              +0x0C0.b4     +0x0E4.b3
    BGQOYRT   Double LEGO Bricks      +0x0C0.b5     +0x0E4.b4
    LJYQRAC   Show Minikit            +0x0C1.b7     +0x0E5.b6
    CPLYREK   Show LEGO Studs         +0x0C0.b1     +0x0E4.b0

Every accept writes **exactly three things**: four bytes at `+0x000` that change
wholesale each time (a checksum or hash), one bit in a field based at `+0x0B4`,
and one bit in a second field based at `+0x0D8`.

Two invariants appeared to hold across the first nine — field B's byte being
field A's + 0x24 and its bit one lower — and **both are artefacts of the wrong
bit numbering.** Seven of the 56 codes break them:

    Show Red Bricks   +0x0C0.b0 -> +0x0E5.b7     dbyte 0x25, dbit +7
    Full Unit Cap     +0x0BF.b0 -> +0x0E4.b7
    Troll King        +0x0B7.b0 -> +0x0DC.b7
    ... every one of them a case where field A's bit is 0

Numbering bits **MSB-first** — bit 0 is `0x80` — dissolves the exceptions. Treat
each field as a continuous big-endian bit stream based at `+0x0B4` and `+0x0D8`:

    stream index n  ->  byte base + n/8, mask 0x80 >> (n % 8)

Then **field B's index is always exactly field A's index + 1**, for all 52
measured codes with no exception at all. The two fields are one bit stream and a
copy of it shifted one position along.

That is worth stating plainly because the first model fitted 45 of 52 points and
looked convincing. Fitting the majority is not the same as being right, and the
seven exceptions were all the same case — the one a lazy fit rounds away.

Nothing here says what the second field is *for*. "Owned" versus "seen" is the
obvious guess and is labelled as one.

## What the bit index is NOT

The obvious model — bit index = the record's registry index — does **not** hold.
As a linear bit offset from `+0x0B4`:

    registry index  19   20   21   22   23   24
    measured bit   109  110  100  101  111   97

Consecutive registry indices do give consecutive bits in pairs (19,20 -> 109,110
and 21,22 -> 100,101), so the two orderings are related, but they are not the
same ordering and no single offset reconciles them. The record's `string_id` does
not fit either.

Rather than keep fitting models to nine points, the table is being **measured
outright**: every one of the 56 coded records is entered through the game's own
keyboard and the bit it sets recorded. That gives the mapping by observation of
the cartridge's own behaviour, which is how this project derives values, and it does not
require understanding the indexing at all.

## What this unblocks, and what it does not

`Unlock All Units` and `Unlock All Extras` become "set this measured set of
bits", which is exactly what the game does when the codes are typed.

**Not yet established, and not to be assumed:**

* **Whether writing the bits without updating the `+0x000` checksum is
  accepted.** docs/176 argued the game recomputes its own checksum when it next
  saves, so the live record can be written. That argument has not been tested for
  *this* field, and the check is direct — set the bits, then open Character
  Collection and see whether the game shows them unlocked. Until that is done the
  settings should not be marked wired.
* **Unlock Santa.** There is no Santa record anywhere in the registry. Whatever
  grants it is not a cheat code, and nothing here touches it.
* **Unlock All Maps.** No map records in kind 8 either. Kind 1 has 49 records
  whose string ids do not resolve to loaded LOC strings, and 49 is a plausible
  map count — but "plausible count" is not evidence, and the Extras menu has its
  own Maps entry that has not been opened yet.
