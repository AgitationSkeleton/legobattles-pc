# The unlockable table

**Status: the game's achievement/unlockable registry is located and its shape is
read. 25 entries, four parallel arrays, one builder. This is the ROM ground for
the port's cheat tab, which until now was entirely unread.**

## How it was found

Not by chasing `UnlockableManager`. That class turned out to be a shell — two
vtable slots (the destructor pair) and a 60-byte constructor that initialises
four containers and calls `func_020EB554`. The registry is plain data.

A string sweep of the ARM9 for `unlock|santa|extra|bonus|cheat|secret` found
fourteen hits, of which the useful ones were:

```
0213BD18  UnlockAllCodes
0213BDD0  UnlockHiddenHeroes
0214496C  /LS_CheatCodes
0214D9F0  Santa_Victory
0214DA00  Santa_Defeat
          Battles::User::CheatsScreen, CheatKeyboardScreen, CheatKeyboardTopScreen
```

**The retail game already has a cheat-code system**, with its own on-screen
keyboard. That matters for the cheat tab: its "unlock everything" may have a
legitimate in-game path to imitate rather than a save file to forge.

## The table

The two `Unlock*` strings are referenced only from a pointer array, and that
array is one of four parallel arrays in a single block based at **`0x0213BE8C`**:

```
base + 0x000   u16[25]   thresholds        (0xC350=50000, 0x03E8=1000, 0xFA=250, 0x64=100, 0x14=20, 5 ...)
base + 0x032   u8 [25]   a per-entry byte
base + 0x04C   ptr[25]   name strings
base + 0x0B0   u32[25]   string id A
base + 0x114   u32[25]   string id B
base + 0x178   u32[25]   string id C
```

The 25 names, in table order:

```
UnlockHiddenHeroes      CollectStuds            KillKingWithKing
DefeatEnemyUnits        DefeatSiegeUnits        DestroyBuildings
WinSkirmishGames        WinResourceRace         WinKillTheKing
WinMultiplayer          Play3PlayerMP           CompleteSide1Campaign
CompleteSide2Campaign   CompleteSide3Campaign   CompleteSide4Campaign
CompleteSide5Campaign   CompleteSide6Campaign   PurchaseAllConceptArt
UnlockAllCodes          UseRoarSpell            HealPoints
BuildMountedUnits       BuildSiegeUnits         BuildFarms
BuildTowers
```

**`Play3PlayerMP` independently confirms the three-player maximum**, which had
been carried as unverified until now.

## The builder

`func_020A68D0` (thumb, 284 bytes) is the only code in the image that points at
`0x0213BE8C`. It loops **`cmp r6, #0x19` — exactly 25 iterations** — and emits a
**40-byte record** per entry (`adds r4, #0x28`):

```asm
ldr  r0, [r5, #0x4c]        ; name pointer        -> record +0x08
adds r0, r5, #0xb0 ; ldr    ; string id A         -> record +0x0C
movs r0, #0x45 ; lsls #2    ; +0x114, string id B -> record +0x10
movs r0, #0x5e ; lsls #2    ; +0x178, string id C -> record +0x14
ldrh r0, [r7] ; adds r7, #2 ; the u16 threshold   -> record +0x24
ldr  r0,[sp,#8] ; adds #0x32; the per-entry byte  -> record +0x26
adds r4, #0x28              ; next record
cmp  r6, #0x19 ; blo        ; 25 entries
```

After the loop it makes four more objects through `func_02009A3C` with counts
**15, 13, 21 and 19**. All four share one source table (`0x0214BAF0`), one
callback (`func_020A6A68`) and one destination global (`0x02156D00`), differing
only in the count and a string id.

**Those four groups are not unlock categories.** The first draft of this note
guessed they matched the cheat tab's *Unlock All Units / All Extras / All Maps /
Santa* split, because four groups of unequal size is the right shape. Resolving
the string ids through `tools/lb_lang.py` killed it:

```
0x1A9 = 425  "Shipyard"        count 15
0x1BD = 445  "Plantation"      count 13
0x1C6 = 454  "Builder"         count 21
0x1CF = 463  "Lumber Mill"     count 19
```

Building and unit names. These are **game-content lists**, and what exactly they
enumerate is unresolved — the id runs would overlap if each were simply `count`
consecutive ids, so the id is a base used some other way. They sit in this
function next to the achievement loop, nothing more.

## What this gives the cheat tab, and what it does not

**Gives:** the achievement set is fully enumerated with its thresholds, the
record layout is known, the builder is identified, and the game's own cheat-code
screen exists as a possible legitimate path.

**Does not give:** where progress is *stored*. Nothing above touches the save
file. `SaveGameEvent::SaveableGameParametersType` and `User::ProfileCard` are
the obvious next reads, and neither has been opened. Until the persisted
representation is read, an "Unlock Everything" button can only be honest as
`[unread]` in the overlay, which is how it is currently drawn.

The next read is the save path, not the four groups — those turned out to be
content lists. `SaveGameEvent::SaveableGameParametersType` and
`User::ProfileCard` are the two named classes standing between this table and a
working *Unlock Everything*.

---

# Correction: the `Unlock*` strings are achievement names, and `/LS_CheatCodes` is artwork

Two inferences above need narrowing.

**1. `UnlockAllCodes` and `UnlockHiddenHeroes` are entries in the 25-name
table**, sitting alongside `CollectStuds`, `WinSkirmishGames` and
`BuildFarms`. The section above lists all 25 and both appear there. Neither
string is referenced from a literal pool anywhere in the image -- they are
reached only through the `base + 0x4C` pointer array -- so they are
**achievement identifiers**, not cheat codes. "The retail game already has a
cheat-code system" is still true (the `CheatsScreen`, `CheatKeyboardScreen` and
`CheatKeyboardTopScreen` classes exist), but these two strings are not its
codes, and reading them as such would have sent the cheat work looking for a
comparison that does not exist.

**2. `/LS_CheatCodes` is a graphics asset, not a code list.**
`docs/data/nitro_files.tsv`:

```
UI/AllInOne/LS_CheatCodes.NCBR   NCGR  9280 bytes  288 tiles, 48x6, 4bpp
UI/AllInOne/LS_CheatCodes.NCGR   NCGR  9280 bytes
UI/AllInOne/LS_CheatCodes.NCLR   NCLR   552 bytes  256 colours
```

Tiles and a palette -- the cheat screen's own artwork. There is no cheat-code
data file in the cartridge.

## Where the cheat path actually is, so far

`CheatKeyboardScreen::virtual_08` (`func_020B4B14`) is **not** the validator: it
is the same widget-walk every screen uses (`docs/104`), dispatching on element
ids `0x250`, `0x259` and neighbours -- keyboard buttons, not a comparison.

So the entered string is checked somewhere else, and that site is unread. Until
it is, the four Unlock options stay `[unread]`. The honest options are to find
the validator and drive the game's own path, or to find where unlock state is
stored and set it; the second needs the save layout, which `docs/100` records as
runtime-composed rather than static.

---

# The cheat codes themselves: 57 entries at `0x0213C06C`

Found by sweeping the ARM9 for NUL-terminated uppercase alphanumeric tokens of
5-8 characters and reporting clusters. One cluster dominates: **57 seven-character
codes on an 8-byte stride, starting at `0x0213C06C`**.

```
BXQIEWD GSIAVCN HGQKIRY NZXWDJR PMWQFNG AMWJHDR OJKWDTY BRTKDQA
FKWPTES RJASKRS SIOAMDG ZHWLKIE OISJUEF TRGEQAZ WEVFKLE PWOMFQN
IEUBNFR LAOEBFG KQOCVME UKELZOW XGABVPL ZVDNJSU BLQPWUA VIWHSAZ
RWPOKJL MSOFRJS QETUOLJ XRCTVYB ADGJLPI KLWDMVC YRTPSHI UGDRSQP
BNHSIOZ OPKLSWN ITDXPMQ IQOTMDA SHWSDGU GJSGHOQ IBGOFWX KVBPQRJ
HJCRAWK UMSXIRQ THNBGRE EDRFTGY PQZLJOB RTGYPKC DWFTBNS ZPWJFUQ
ABABLRX CPLYREK LJYQRAC BGQOYRT JRTPASX QMSLPOE AVMPWHK SKQMXPL
JGREOKN
```

Seven-character uppercase codes are exactly the format LEGO Battles uses, and
this is the only such table in the image.

**It is adjacent to the unlockable table.** `0x0213C06C - 0x0213BE8C = 0x1E0`,
and the six arrays documented above end at `base + 0x1DC`. So the codes sit
immediately after them in the same block -- but there are **57 codes against 25
unlockables**, so this is *not* a seventh parallel array and must not be read as
one. What the extra 32 correspond to is unknown.

**Nothing references `0x0213C06C` from a literal pool**, so the reader computes
the address from a nearby base -- consistent with the whole block being reached
through one pointer. The validator is therefore still unlocated, and until it is
found neither "which code does what" nor "how to trigger one" is established.

What this does give: if the unlock options are to drive the game's own path
rather than forge save state, the codes they would enter are now in hand and
extractable from the cartridge rather than from a walkthrough.

## The registry that owns the codes: 76 records, and its builder

The cheat strings are pointed at from a 16-byte record table. `func_020C2EC4`
(56 bytes, thumb, one caller `func_020C2890`) walks it in **two runs**:

```c
r5 = 0x02125FF4;  for (i = 0;    i <= 0x11; ++i, r5 += 0x10) { func_02007BD0(); func_02007F3C(r0, r5); }
r5 = 0x02126114;  for (i = 0x12; i <= 0x4B; ++i, r5 += 0x10) { func_02007BD0(); func_02007F3C(r0, r5); }
```

**18 + 58 = 76 records**, indexed 0..0x4B. The 57 cheat codes are a subset of
them, so this is the registry for **everything unlockable** -- store items,
concept art, minifigs and cheats alike -- not a cheat-only table.

Record layout, read from the dump:

```
+0x00  u32  kind          (8 for every cheat-carrying record seen)
+0x04  u32  unlock index  (0x17, 0x18, 0x19 ... ascending)
+0x08  ptr  the seven-character code string, or 0
+0x0C  u32  a LOC string id (0xB09, 0xB0A, ...)
```

so a record ties **an unlock index to the code that grants it and the name to
show**. Example rows:

```
02126160: kind 8  index 0x17  LJYQRAC  string 0xB09
02126170: kind 8  index 0x18  CPLYREK  string 0xB0A
021262B0: kind 8  index 0x2C  GSIAVCN  string 0xB1E
```

## What remains for the Unlock options

The registry says *what* can be unlocked and *which code* unlocks it. It does
not say where "unlocked" is recorded -- that is the profile/save state, and
`docs/100` establishes the save layout is runtime-composed from registered
subsystems rather than statically laid out.

Two honest routes, unchanged but now much better supplied:

* drive the game's own path -- the codes are in hand and the registry names the
  index each grants; what is missing is the validator that consumes a typed code;
* set the unlock state directly -- which needs the save-state location, not the
  registry.

`func_02007F3C(item, record)` is the constructor for one entry and is the next
thing to read, since whatever field it initialises from the record is where an
"unlocked" flag would live.

## `func_02007F3C` dispatches on the record's kind

```c
func_02007F3C(item /*r0*/, record /*r1*/) {
    kind = record[0];             /* the +0x00 byte */
    if (kind > 0x9A) return;
    switch (kind) { ... }         /* a 155-entry jump table */
}
```

So the byte this document called "kind (8 for every cheat-carrying record)" is a
**type selector with 155 cases**, and the registry is a general
record-driven initialiser rather than an unlockables-specific one. Cheat records
take case 8; the other 154 cases belong to whatever else is built from records
of this shape.

That widens the picture usefully: `func_02007F3C` also has callers
`func_02007F10` and `func_020D2500` (the latter in the `CheatsScreen` region),
so the same machinery serves more than the 76-record walk.

**Case 8's body is the next read** -- it is what a cheat record actually does,
and therefore what an Unlock option would have to reproduce or trigger.

## The flags block: `*(u32*)0x02150A70`

Case 8 of `func_02007F3C`'s dispatch, in full:

```c
case 8:
    p = func_020092A8();
    if (p[8] == 0) goto not_available;
    func_020093F4(func_020092A8());
    return;
case 9:
    p = func_020092A8();
    if (p[9] == 0) goto not_available;
    func_02009418(func_020092A8());
    return;
```

and

```c
void* func_020092A8(void) { return *(void**)0x02150A70; }   /* 50 callers */
```

So there is a **global options/flags object** at `*(u32*)0x02150A70`, and the
record dispatch gates each kind on a byte inside it: **kind k tests `flags[k]`**
for the cases read (8 and 9). Other code reads the same block at other offsets --
`func_0205F124` tests `flags[7]`, `func_0205E7F4` tests `flags[0xD]` -- so it is
the game's own switch panel, consulted throughout the simulation.

**This is the mechanism the Unlock options should use.** Setting a byte in a
block the cartridge already consults is the game's own path, not forged save
state, and it needs no knowledge of the runtime-composed save layout
(`docs/100`).

### What is still needed

Every cheat-carrying record dumped above has kind **8**, so they all gate on the
same `flags[8]`. That byte therefore reads as "cheats are enabled" rather than
"this particular thing is unlocked", and per-item granularity must live
elsewhere -- most likely in `func_020093F4`, which case 8 calls once the gate
passes, and which has not been read.

**Do not assume `flags[k]` generalises beyond the cases actually read.** Two
data points (8 and 9) matching their case number is suggestive and is exactly
the sort of pattern that has been wrong three times in `docs/95`. The next read
is `func_020093F4`, and after it, which byte each of the four Unlock options
corresponds to.

## The actions are toggles, and that does not yet add up

```c
uint32_t func_020093F4(void* f) { f[8] = f[8] ? 0 : 1; return f[8] ? 0 : 1; }
uint32_t func_02009418(void* f) { f[9] = f[9] ? 0 : 1; return f[9] ? 0 : 1; }
```

Both are 36-byte toggles on a single byte of the flags block, returning the new
state inverted.

So case 8 reads, in full: **if `flags[8]` is set, toggle it** -- i.e. turn it
off. For an "activate this unlock" path that is backwards, and it means one of
these is true and none is established:

* the 155-case table is a **query** used by several callers for different
  purposes, and this particular caller is a toggle-on-click handler rather than
  an activator;
* the gate byte and the toggled byte are not both `flags[8]` and the two
  `func_020092A8()` calls return different objects on the second invocation;
* `flags[8]` means something other than "unlocked" -- for instance a
  *pending/dirty* marker consumed elsewhere.

`func_02007F3C` has three callers (`func_02007F10`, `func_020C2EC4`,
`func_020D2500`) and only the middle one is the 76-record registry walk, which
supports the first reading -- but supports is not shows.

**Stopping here rather than guessing.** The flags block, its accessor, the
toggles and the registry are all solid and independently useful. Which byte
corresponds to each of the cheat tab's four Unlock options is not, and inventing a
mapping would produce a switch that silently toggles the wrong feature -- the
exact failure mode `docs/105` records. The options stay unwired.

## Correction: the table is 155 records, and there are two per-entry handlers

`func_020D2500` (700 bytes, in the `CheatsScreen` region) walks the same table
from the same base, but further:

```c
r6 = 0x02125FF4;
for (i = 0; i < 0x9B; ++i, r6 += 0x10) {
    copy 16 bytes of the record to the stack;
    if (screen[0xDF + i] != 0) func_020088D4(func_02007BD0(), record);
    else                        func_02007F3C(func_02007BD0(), record);
}
```

**155 records**, not 76. `func_020C2EC4`'s two runs are contiguous
(`0x02125FF4 + 0x12*0x10 == 0x02126114`) and cover only entries 0..0x4B, so that
builder uses a prefix of the same table.

Two things follow:

* **`func_02007F3C` has a sibling, `func_020088D4`** (2180 bytes), chosen by a
  **per-entry byte in the screen object at `+0xDF + i`**. So the screen carries
  its own per-item state and picks a handler from it. That is the "query versus
  activate" question answered: there are two handlers, not one dual-purpose one.
* The 155-case dispatch inside `func_02007F3C` is on the record's **kind**, and
  kind 8 recurs across many records (the dump shows `0x17`..`0x2E` all kind 8).
  So kind is a **category**, not an index, and `flags[8]` is one flag for the
  whole category rather than per item.

## Where this thread stands

Established: the 155-record table at `0x02125FF4` with its 16-byte layout; the
57 cheat codes and which record carries each; the global flags block at
`*(u32*)0x02150A70` with per-category bytes and their toggles; two per-entry
handlers selected by `screen[0xDF + i]`.

Not established: **where per-item unlock state persists.** It is not the flags
block (one byte per category) and not the screen array (`screen[0xDF + i]` is UI
state on a screen object). It is profile data, and `docs/100` has that layout as
runtime-composed from registered subsystems rather than statically addressable.

The four Unlock options therefore remain `[unread]`, and the honest next step is
the save/profile state -- a different and larger piece of work than the registry,
which is now thoroughly mapped.

---

# Superseded in three places by docs/170

Read `docs/170` alongside this file. With the record table's base corrected from
`0x02125FF4` to **`0x02126000`** (this doc's own example addresses were 12 bytes
off its stated lattice, which is how the error showed):

* **the base above is wrong.** A 12-byte error inside a 16-byte stride does not
  produce garbage, it rotates the fields into another plausible-looking layout,
  which is why it survived.
* **"kind" is not a 155-value category.** It takes seven values across all 155
  records: 1, 2, 3, 4, 8, 16, 31. The 155 is the record count.
* **this is not "the registry for everything unlockable".** Every name in it
  resolves to a cheat name or to the placeholder `'Unnamed Cheat Code'` — no
  store items, no concept art, no minifigs.

What docs/170 adds: `flags[k]` is confirmed for **all eighteen** cases 0..17 of
`func_020088D4`, not just the two read here, so this doc's refusal to generalise
is now resolved in favour of the pattern. And all 18 cheats are named and paired
with their codes.

This doc's central conclusion is UNCHANGED and was right: the flags block holds
one byte per category, not per item, so per-item unlock state is still unlocated
and the Unlock options stay `[unread]`.

---

# The profile state this doc was waiting for: docs/176

This doc ends on "where per-item unlock state persists ... is profile data, and
docs/100 has that layout as runtime-composed rather than statically addressable",
and treats the unbroken save checksum as the blocker.

The checksum turned out not to matter. The 512-byte save is small enough to read
whole, its two 242-byte CardPage records carry the bitfields, and the LIVE copy
of the record sits at **0x02168500** in main RAM -- writable, with the game
recomputing its own checksum when it next saves. See docs/176.

What is still missing is only the MEANING of the bits, which is a smaller
question than the one this doc left open.

---

# The validator this doc twice called unlocated: docs/177

`func_02009158` is it. This doc searched the `CheatKeyboardScreen` classes and
correctly ruled out `virtual_08`; the validator is not in the screen classes at
all, but among the six functions that reference the record table from a literal
pool. It walks all 155 records, compares the typed string against
`record[0x04]`, checks `record[0x0C]` against the screen's accepted kind, and
calls `func_020088D4`.

It also independently confirms the record field order that docs/170 corrected
and this doc originally had wrong.

