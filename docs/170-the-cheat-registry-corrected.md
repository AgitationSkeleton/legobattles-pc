# 170 — The cheat registry, with three corrections to docs/97

Status: **partly superseded.** Correction 1 below is WITHDRAWN by docs/171 --
the base was always right and the field order was the error. Corrections 2 and 3
stand, and the 18 cheats and their codes are correct. Read docs/171 for the
resolved layout and for the consequence (the flags are per cheat, and eighteen
settings are wired because of it). Data: `docs/data/cheat_registry.tsv`.

docs/97 did the hard work of finding this block and should be read first. Three
of its conclusions are wrong, and each was wrong for a reason worth keeping.

## Correction 1 — WITHDRAWN by docs/171: the base was right, the FIELD ORDER was wrong

**Everything in this section is retracted.** docs/97's base `0x02125FF4` is
correct: it has six literal-pool references in the image and `func_020c2ec4`
walks from it, while `0x02126000` has none. A lattice argument settled by data
coherence alone is not settled -- a partial-stride offset rotates the fields
rather than destroying them, so both readings looked sensible.

What docs/97 actually had wrong is the field order within the record:

    docs/97 said:  +0x00 kind   +0x04 index    +0x08 code ptr  +0x0C sid
    actually:      +0x00 index  +0x04 code ptr +0x08 sid       +0x0C kind

On the correct reading `index` equals the record number for all 155 records and
exactly 57 carry a code, which is the self-check that settles it. See docs/171,
which also shows the consequence: the flags are per CHEAT, not per category, and
eighteen settings are now wired because of it.

The withdrawn original argument follows, kept because the failure mode is worth
seeing.

### (withdrawn) the table base is `0x02126000`, not `0x02125FF4`

docs/97 gives the base as `0x02125FF4` and then cites example records at
`0x02126150`, `0x02126160`, `0x02126170` and `0x021262B0`. Those cannot both be
right: `0x02126150 - 0x02125FF4 = 0x15C`, which is not a multiple of the 16-byte
stride. The examples are 12 bytes off the stated lattice.

Scoring candidate bases on whether all 155 records decode sensibly settles it:

    0x02126000 : 155/155 coherent
    0x02125FF0 : 155/155 coherent
    0x02126010 : 154/155
    0x0212601C :   0/155

`0x02126000` is the base that puts docs/97's own example addresses on the
lattice, and it is the one where the code-string pointer field actually resolves
to the seven-character codes.

**Why the wrong base still looked right.** Reading at `0x02125FF4` produces
records whose first word is a clean ascending sequence 0,1,2,...,154 and whose
last word is a small value from a tidy set. That is coherent enough to write
down — but it is the *previous* record's tail followed by the *next* record's
head. A 12-byte error inside a 16-byte stride does not produce garbage, it
produces a plausible-looking rotation of the fields, which is exactly the failure
mode that makes struct offsets dangerous to eyeball.

## Correction 2 — kind is a 7-value set, not a 155-case space

docs/97 reads `func_02007F3C` as dispatching on the record's kind through a
"155-entry jump table", and infers kind is a category with many values.

With the base fixed, the kind field takes exactly seven values across all 155
records:

    kind  1 : 49 records
    kind  2 :  2
    kind  3 :  5
    kind  4 :  4
    kind  8 : 58 records   <- the cheats, 56 of them carrying a code
    kind 16 : 18 records   <- the named cheat list
    kind 31 : 19 records

The 155 in the dispatch is the *record count*, not the kind range.

(The record layout that stood here was the rotated one. The correct layout is
`+0x00 index, +0x04 code ptr, +0x08 sid, +0x0C kind` -- see docs/171.)

## Correction 3 — it is a cheat registry, not "everything unlockable"

docs/97 concludes "this is the registry for **everything unlockable** — store
items, concept art, minifigs and cheats alike". Resolving the string ids through
`tools/lb_lang.py` says otherwise: every name in the table is a cheat name or the
placeholder `'Unnamed Cheat Code'`. There are no store items, no concept art and
no minifigs in it.

## New: `flags[k]` generalises, across all 18 cases

docs/97 established the global flags object at `*(u32*)0x02150A70` and its
accessor `func_020092a8`, read `flags[8]` and `flags[9]`, noticed both matched
their case number, and then explicitly refused to generalise from two points —
correctly, given docs/95's history.

`func_020088D4` (the sibling handler, 2180 bytes, thumb) settles it. It calls the
accessor 51 times, and every store in the whole function is to the stack, so it
is a **query**, not an activator. Extracting the byte offset read after each
accessor call gives:

    0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 10, 11, 12, 12, 13, 13,
    14, 14, 15, 15, 7, 7, 8, 8, 9, 9, 17, 17, 16, 16

**Eighteen distinct offsets, 0 through 17, with no gaps** — and the function's
jump table is bounded by `cmp r1, #0x11` (17) immediately before it. So case `k`
reads `flags[k]` for every case that exists. The pattern docs/97 saw in two
points holds across all eighteen.

This section drew the wrong consequence from the right observation, because it
still assumed the rotated field order. The byte the switch reads is indexed by
the record's **index**, not its kind, and records 0..17 are the eighteen distinct
cheats -- so it is one byte **per cheat**. docs/171 has the corrected reading and
the live verification.

## The 18 cheats, and the code that grants each

Kind 16 is the named list (records 0..17); kind 8 carries the codes. Pairing them
by name:

    One Hit Kill (Heroes)   AVMPWHK        Super Magic          DWFTBNS
    Fast Building           QMSLPOE        Show Red Bricks      RTGYPKC
    Fast Magic              JRTPASX        Upgraded Towers      EDRFTGY
    Double LEGO Bricks      BGQOYRT        Super Explosions     THNBGRE
    Show Minikit            LJYQRAC        Fast Harvesting      PQZLJOB
    Show LEGO Studs         CPLYREK        Full Unit Cap        UMSXIRQ
    Regenerating Health     ABABLRX        Invincible Hero      HJCRAWK
    Long Range Magic        ZPWJFUQ        Fast Mining          KVBPQRJ
    Show Enemies            IBGOFWX        Reveal Map           SKQMXPL

Full table including the placeholder records: `docs/data/cheat_registry.tsv`.

Two of these bear directly on the widescreen work:

* **Reveal Map (`SKQMXPL`)** is the cartridge's own fog-of-war control. docs/169
  records that the host-side full-map render draws unscouted ground, which is a
  fidelity problem; this is the game's own switch for it rather than a mask the
  port would have to reimplement.
* **Show Enemies (`IBGOFWX`)** is the matching control for enemy visibility.

## What this does and does not unlock

**Does:** the cheat set is fully enumerated and named, each code is paired with
its effect from the cartridge rather than a walkthrough, the record layout is
right, and `flags[k]` is established across every case that exists.

**Does not:** say where per-ITEM unlock state persists — the four *Unlock*
options (units, extras, Santa, maps) are profile data and remain `[unread]`.

For the cheats themselves this doc's pessimism was misplaced: docs/171 shows the
flags block holds one byte per cheat, and all eighteen are now wired and verified
against the running game.
