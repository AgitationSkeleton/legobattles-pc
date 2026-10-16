# 176 — The profile record, live in RAM

Status: **located.** This is what docs/97 said the Unlock options were waiting
for. It does not yet say which bit means what, and no setting is wired on it.

## Why the save file was the way in

The cartridge save is **512 bytes** — small enough to read entirely, which the
earlier work never did. Diffing 130 save files from the harness runs shows
only 46 bytes ever vary, and the structure falls out immediately:

    000..00F   "AFE_BATTLE_SAVES"
    010..016   varies every run  (checksum or session id)
    ...
    0B9..0BB   FF FF 0A          record 1 bitfields
    0C5..0C6   BF C0
    0DD..0DF   7F FF 85
    0E9..0EA   5F E0
    1AB..1AD   FF FF 0A          record 2 -- IDENTICAL, and exactly
    1B7..1B8   BF C0             0x1AB - 0x0B9 = 0xF2 = 242 bytes later
    1CF..1D1   7F FF 85
    1DB..1DC   5F E0
    1F4..1FF   FF x12

Two 242-byte `Battles::CardPage` records, as docs/100 recorded, carrying
bitfields — which is what progress and unlock state look like.

## The live copy, which sidesteps the checksum

docs/97 stopped at the save layout because the checksum algorithm was never
found (216 CRC variants and accumulators eliminated), so a forged save would be
rejected. **That does not matter if the file is never touched.** Searching main
RAM for the record's populated 50-byte span finds exactly one verbatim copy:

    save offset 0x0B9  ->  0x021685A9
    so save offset 0x010 ->  0x02168500   the record base

465 of 512 bytes agree with the file; the differing ones are the 16-byte magic,
which is prepended when writing rather than held in RAM. Writing the live record
therefore changes what the game reads, and the game recomputes its own checksum
when it next saves — no need to reproduce it.

## Structure so far

    +0x000  59 F5 24 43 51 77 65      7 bytes, vary every run
    +0x015  7F 65 40 . C0 . E0 01 . 01 20 . . . 40 . 60 . 80 . B7 C0
    +0x0A9  FF FF 0A                  \
    +0x0B5  BF C0                      |  a 12-byte stride: entries at
    +0x0CD  7F FF 85                   |  A9, B5, (C1 zero), CD, D9
    +0x0D9  5F E0                     /

The 12-byte stride is the clearest structural signal: an array of 12-byte
entries, each starting with a short bitfield, some entries empty.

## What is NOT established

Which bit is which. Nothing here says that a given bit means "Santa unlocked" or
"map 12 available", and setting them all would be exactly the kind of guess
the project does not make — the more so because the four Unlock
options must be told apart, not merged.

So the Unlock options stay `[unread]`. What has changed is that they are no
longer blocked on an unbroken checksum: the state is reachable and writable, and
what remains is reading the meaning of the bits.

The next step is to change one unlock in-game and diff the record, which is the
one experiment that names bits without guessing — and it needs a way to unlock
something, i.e. the cheat validator (docs/171 has the 57 codes) or ordinary play.

---

# Record structure, on its own stride

Viewing the 242-byte record 12 bytes at a time makes the shape plain:

    +0x000  59 F5 24 43 51 77 65 . . . . .      7-byte head, varies every run
    +0x015  7F 65 40 . C0 . E0 01 . 01 20 . . . 40 . 60 . 80 . B7 C0
    +0x0A9  FF FF 0A
    +0x0B5  BF C0
    +0x0C1  (empty)
    +0x0CD  7F FF 85
    +0x0D9  5F E0

The tail is an array on a **12-byte stride** with one empty slot, which is the
clearest structural signal in the record. The second record in RAM at BASE+242
does NOT repeat record 1's content -- it carries `5B DD F9 7B` at +0x00E -- so
the two are distinct slots, unlike the on-disk copy where the two blocks matched.
That difference is unexplained and is recorded rather than smoothed over.

Bit meanings remain unknown, and the options stay `[unread]`.

# Sprite learning throttled

Capturing a 40x40 patch is ~1600 terrain lookups per unit, each several bus
reads, so learning every visible unit every frame was real work for no gain -- a
unit's appearance changes over animation frames, not single ones. Each slot now
refreshes at most every 30 frames, staggered by slot so the cost spreads instead
of spiking. Verified unchanged: learned sprites still draw outside the camera box
(`screenshots/.../throttled_units.png`).

---

# Two robustness fixes to the wide field, found by reading rather than by hitting them

**1. `lb_field_locate()` was retried every frame until it succeeded.** It copies
and scans 4 MB of main RAM, and it fails whenever no level is up -- which is
every frame the player spends in a menu. Simply having the wide field enabled
would have turned the front end to treacle, and that is the first thing anyone
switching the option on would have met. Now retried at most every 90 frames.

**2. A level change was not detected.** The metatile tables and the grid move
between levels -- docs/168 saw the tables at different addresses in consecutive
runs of the SAME save -- so a level that had been located and then torn down
would still be drawn from its old addresses. `lb_field_still_valid()` samples
sixteen grid cells for plausible metatile indices and the wide field re-checks
every 60 frames, dropping back to re-locate when the level has gone.

Both were found by reading the code rather than by running into them, which is
worth noting only because the other bugs in this document were all found the
other way round.
