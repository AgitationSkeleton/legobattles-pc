# The save format is a bit stream, and it is 220 bits plus one per unlockable

**Status: the layout is recovered and validated by the loader.**

`Battles::Save::SaveGame` at `0x0204FA88` — named by the third-party `ds-decomp`
config, whose own implementation of it is empty — writes the whole profile into
a bit stream at `0x02148AA8`, field by field, with an explicit width for each.

## The layout

Source is a profile structure at `0x02154E08`; the campaign block is at
`0x02154E40`.

| order | source | width | notes |
| ---: | --- | ---: | --- |
| 1 | `profile+0x24` (u8) | **1** | a flag |
| 2 | `profile+0x4C` (u16) | **16** | |
| 3 | `profile+0x50` (u32) | **32** | the widest field — studs, or a play timer |
| 4 | `profile+0x30` (u8) | **7** | 0–127 |
| 5 | `profile+0x31` (u8) | **7** | 0–127 |
| 6 | `profile+0x32` (u8) | **1** | |
| 7 | `profile+0x34` (u32) | **3** | 0–7 |
| 8–11 | the 20-byte campaign record, below | **153** | |
| 12 | one bit per unlockable | **N** | |

### The 20-byte record at `0x02154E40` — the CUSTOM ARMY, not campaign progress

> **CORRECTED.** The widths below are right; the MEANING was guessed and is
> wrong. See "What this record actually is" at the end of this document.

Written by two counted loops and two scalars:

```arm
mov sb, #0x10                  ; the width for the first loop
loop 4x:  ldrh [r4 + i*2]      -> 16 bits each
loop 5x:  ldrh [r4 + i*2 + 8]  -> 16 bits each
          ldrb [r4 + 0x12]     ->  1 bit
          ldrb [r4 + 0x13]     ->  8 bits
```

4 × `u16` + 5 × `u16` + `u8` + `u8` = **exactly 20 bytes**, matching the `0x14`
stride the outer loop advances by. Nine 16-bit counters and two small fields —
the shape of per-act progress plus a completion flag and a percentage.

The outer loop runs **once** (`cmp r0, #1; blt`), so one record is written.

### The unlock bits

```arm
count = [profile + 0x14]
for i in 0 .. count-1:
    r0 = func_020EE9A0(0x02154E18, i)   ; BitArray::Get
    write(r0, 1 bit)
```

`func_020EE9A0` is three instructions of `BitArray::Get` —
`byte = array[i >> 3]; return (byte >> (i & 7)) & 1` — so `0x02154E18` is a
`Battles::BitArray`, and the RTTI has a `Battles::User::UnlockableManager` to
match.

**One bit per unlockable, in index order.** `docs/22` established from the
blueprint side that there are exactly **289** unlockables occupying a contiguous
`0…288` slot space. This is the other half of that fact: the save writes those
289 slots as 289 consecutive bits.

So a fully-unlocked profile is **220 fixed bits + 289 set bits = 509 bits, about
64 bytes**, and "unlock everything" is a single run of ones.

## The loader validates it

`func_0204F718` is the reader — the only other function that references the
stream at `0x02148AA8`, and 0x354 bytes against the writer's 0x394. Extracting
its stream-call widths and comparing:

```
writer  Battles::Save::SaveGame  via func_0204BA48:
        1, 16, 32, 7, 7, 1, 3, <reg>, <reg>, 1, 8, <reg>
reader  func_0204F718            via func_0204BA64:
        1, 16, 32, 7, 7, 1, 3, <reg>, <reg>, 1, 8, <reg>
```

**Identical — twelve stream operations, same order, same widths.** Two
independently-written functions agreeing field-for-field is the strongest
available check that the layout has been read correctly, and it costs one
comparison.

## Why a bit stream at all

Every width is the smallest that fits its field: 7 bits for a 0–127 counter, 3
for a 0–7 enum, 1 for each flag and each unlock. A byte-aligned version of the
same data would be roughly twice the size. DS save chips are small — the
cartridge's is measured in kilobits — and this is what a game does about that.

It also explains `Battles::Save::SaveHasher` in the class list: a bit stream has
no redundancy, so a corrupt save cannot be detected structurally and needs a
checksum. The `LOC` strings agree — *"The LEGO® Battles data is corrupted and
will be deleted."*

## What is still unnamed

The seven scalar fields are recovered as (offset, width) pairs, not as meanings.
`+0x50` at 32 bits is the only one wide enough to be a stud total or a play
timer; `+0x30` and `+0x31` at 7 bits each are counters of something bounded by
127. Naming them needs the code that *writes the profile structure*, which is
`func_0204F4A0`, `func_0204F578`, `func_0204F680`, `func_0204F690`,
`func_0204F6A0` and `func_0204FF68` — all of which reference it and none of
which has been read.

## What this record actually is — the custom army

The 20 bytes at `0x02154E40` are the player's **custom army**, not per-act
campaign progress. The bit widths this document recovered (4x16, 5x16, 1, 8 =
153 bits) are exactly right; only the interpretation was inferred, and the
phrase "the shape of per-act progress plus a completion flag and a percentage"
described a shape that happened to fit.

Stride `0x14`, accessor `func_0204F654(n) = 0x02154E40 + n*0x14`, written only
by `func_0204FED8`, whose only caller is `CustomizeArmy`'s commit
`func_020B8EB4`.

| off | width | field |
|---|---|---|
| `+0x00`..`+0x04` | 3 x u16 | Siege picks 1-3 |
| `+0x06` | u16 | Transport pick |
| `+0x08` | u16 | Hero pick |
| `+0x0A` | u16 | Builder pick (also selects the effective base faction) |
| `+0x0C` | u16 | Melee pick |
| `+0x0E` | u16 | Ranged pick |
| `+0x10` | u16 | Mounted pick |
| `+0x12` | u8 | "a custom army exists" flag |
| `+0x13` | u8 | base faction id (the FACTION space, not the blueprint space) |

The nine picks are **u16 blueprint indices** (`docs/11`'s `+0x04`), written from
`ldrh [record+4]` off resolved blueprints and consumed into
`FactionBlueprint record+0x0C`. `func_02000E78` proves the count exactly: it
clears all 26 roster slots to `0xFFFF`, copies the base faction's categories
7..19 (17 slots), then appends the 5 picks from `+0x08` and the 4 from `+0x00`
-- 17 + 5 + 4 = 26.

**"Non-default" is one bit, not a comparison.** `func_020B5BF4` and
`func_020BA49C` skip the selector row entirely when `[+0x12] == 0`, and
`func_020B83D4` rebuilds all nine picks from `GetFaction(0x7C)` = **King, 124**
when the flag is clear. So the game already implements "show the save's team in
the dropdown when it is non-default". Do NOT decide this by comparing the picks
against the King roster: a save exists whose flag is set and whose picks are
identical to King's.

> **RETRACTED (docs/384).** The flag does not mean non-default. The army
> screen's commit `FUN_020B8EB4` sets `+0x12 = 1` on **every** Continue, so it
> means "the player has confirmed an army at least once". The save with the flag
> set and King's picks is exactly that case, not an exception. "Save File Team"
> is therefore flag set **and** picks unlike King's.

**Only army record 0 is persisted.** Both the save and load loops bound at
`cmp #1`, so Custom Army 2 and 3 (faction ids 133, 134) are RAM-only and lost on
reload.

> **RETRACTED (docs/384).** There are no records 1 and 2. `func_0204F654(1)` is
> 0x02154E54, which is profile `+0x4C`: the u16 and u32 fields the save writes
> (orders 2 and 3 in the table above). Record 2 lands on profile `+0x60`. The
> factions 133/134 are real custom factions (`GetFaction` 130..134, named
> Custom1..3 and CustomWiFiPlayerA/B), but army records for them would corrupt
> the profile. Never write 0x02154E54..0x02154E7B. The only shipped users are
> the multiplayer start, which builds each player's roster from its own lobby
> table, and a cut Free Play option that only ever read them.

In the save file the block is a **bit stream, MSB-first, starting at record byte
`0x0F`** and is NOT byte-aligned -- a byte-oriented reader returns garbage. The
live RAM array at `0x02154E40` is byte-aligned and is what `CustomizeArmy` and
`OpponentSetup` both read, so it is the easier source for a port.

Verified by decoding four save files, including an original cartridge save
opened read-only: slot 0's nine picks are category-correct 9 of 9.
