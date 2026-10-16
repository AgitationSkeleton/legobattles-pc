# 207 — The save checksum is verified, and a wrong edit deletes the save

Status: **measured.** It blocks editing the save FILE, and would have shipped a
save-destroying feature had the unlock buttons been built that way.

**It does not block the unlock buttons themselves** -- see the correction at the
end. The port already implements them by a route this finding does not touch.

## The test

docs/181 noted that every accepted cheat code writes three things: one bit in
the field based at `+0x0B4`, one in the field at `+0x0D8`, and **four bytes at
`+0x000` that change wholesale each time** — described there as "a checksum or
hash", with no statement about whether anything reads it back.

That question is the whole feasibility of an "unlock everything" button, so it
was tested directly. Both unlock streams were cleared in both profile records of
a known-good save, leaving every other byte — including those four — untouched:

    for rec in (0x10, 0x10 + 242):
        zero rec+0xB4 .. rec+0xD7
        zero rec+0xD8 .. rec+0xFB

The game's own answer, on the title screen:

> **The LEGO® Battles data is corrupted and will be deleted.**

## What that settles

* The four bytes at `+0x000` are a **checksum and it is verified on load**.
* The failure is not a soft one. The game does not ignore the record or show an
  empty slot — it **deletes the save**.
* So **no feature may write unlock bits without recomputing that checksum.** A
  button that flipped bits directly would have destroyed the player's save the
  next time they started the game, and a real cartridge save is the
  reference this project keeps pointing at.

This is a much sharper result than "the tag is not covered by the checksum"
(tools/lb_save.py), which remains true and is why retagging is safe: the tag
lives outside the record the checksum protects.

## What is safe today

The save manager's **import is safe** because it replaces a whole file, so the
checksum arrives with the data it belongs to. Nothing in the port edits save
bytes in place.

## CORRECTION: the unlock buttons are not blocked, and already exist

The first version of this document concluded the unlock buttons were blocked on
the checksum. That was wrong, and it was wrong because it assumed the only way
to unlock something is to edit the save file.

`apply_unlocks()` in `lb_patches.cpp` already does it the other way: it sets the
bits in the **live profile record in RAM**, never in the file, so **the game
computes its own checksum the next time it saves**. It only ever sets bits, and
it refuses to write at all until fields A and B agree about a bit they must
share -- which is how it knows a profile record is actually resident.

So the checksum finding above governs one thing only: **the save manager must
not offer to edit save bytes in place.** It does not, and now says why.

## Ground truth from the reference save

A fully-unlocked cartridge profile, read directly:

    slot 0  field A  ff x24                     -- all 192 bits set
            field B  ff x11, fc, 53, 3f, 00...
    slot 1  field A  00 bf c0 00 ...
            field B  00 5f e0 00 ...

Slot 1 is an independent confirmation of docs/181's model: field B is field A
shifted one bit along (`bf c0` -> `5f e0`). It was derived there from 52
measured codes; here it falls out of a save nobody constructed for the purpose.

**Slot 0 breaks that model, and that is new.** With field A entirely set, a
shifted copy would leave field B set for all 24 bytes too. Instead B stops being
0xFF at byte 10 (around stream index 94) and continues `fc 53 3f`, which does
not look like mask bits at all. docs/181 said plainly that nothing established
what field B is *for*; this says the "shifted copy" description cannot be the
whole story, because it does not survive a fully-unlocked profile.

Unresolved, and worth stating rather than guessing at: whether field B is
simply shorter than field A, whether `53 3f` is a different quantity sharing the
region, or whether the port's table -- whose highest index is 110, past where
this save's field B stops -- is setting bits the cartridge never would.

## Checking the port's table against the reference

For every entry in the port's unlock tables, is the corresponding field-B bit
set in the fully-unlocked reference save?

    port entries whose field-B bit is NOT set: 93 94 95 97 99 100 103 104
    reference field B: 104 bits set, highest index 111
    reference field A: 192 bits set (all of them)

All eight are in the **extras** range, which docs/181 identifies as cheats —
One Hit Kill, Fast Building, Double LEGO Bricks, Show Minikit and so on. The
straightforward reading is that these are simply cheat codes the player never
entered, in which case the port setting them is the feature working, not a bug.

That reading is not proven, and the alternative matters: if the port's table is
wrong for those eight, `unlock_extras` sets bits the cartridge never would. What
the data does establish is that the port's highest index is legitimate — field B
reaches index 111 in the reference, and docs/181 measured a real code writing
`+0x0E5.b4`, which is index 108.

The awkward fact remains slot 0's field A being **entirely** set while its field
B is not. Slot 1 shows the two fields agreeing exactly, one bit apart. Whatever
makes slot 0 different is unexplained, and until it is, "field B is field A
shifted along" should be read as a description of how a code ACCEPT writes, not
as an invariant of the fields.
