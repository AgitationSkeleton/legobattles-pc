# `BP/Entities.ebp` — the blueprint format

**Status: the container and the record layout are solved; most stat fields are
named, and the ones that are not are listed as unnamed rather than guessed.**

`python tools\lb_blueprints.py out\eur\plain\BP\Entities.ebp --lang out\eur\plain\LOC\UK_English.lng --type unit --tsv docs\data\entities.tsv`

```
records 551  pool at 0x0fa08  entries 1102  referenced 1102  unreferenced 0
   Battles::UnitBlueprint                   182
   Battles::ProjectileBlueprint             27
   Battles::CollectableItemBlueprint        342
```

## How it was found, which is the point

`docs/10` said this format would not be guessed and that the ARM9 loader was the
right source. It took four steps and about ten minutes once the tooling existed.

**1. Find the filename.** `tools/lb_code.py strings --pattern "ebp|fbp"` prints
every string with the functions that reference it from a literal pool:

```
0x0213abb4  static 'BP\entities.ebp'   0x02001e70(func_02001e70)
0x0213abe0  static 'BP\factions.fbp'   0x020025d4(func_020025d4)
```

**2. Follow it two hops.** `func_02001E70` issues the file request and registers
`func_02001EE0` as the completion callback. That callback stores the buffer and
calls `func_02001F3C`.

**3. Read the parser.** `func_02001F3C` is 0x17C bytes and says everything:

```arm
ldr  r0, [0x0215091C]      ; the loaded file
add  sl, r0, #4            ; skip the 'BPNZ' magic
ldr  r4, =0x227            ; 551 records
loop:
  ldrb r0, [sl, #8]        ; the type tag
  cmp  r0, #0  → new(0xC); func_02002B68(obj, sl); sl += 0x7C
  cmp  r0, #1  → new(0xC); func_02002A4C(obj, sl); sl += 0x74
  cmp  r0, #2  → new(0xC); func_02000C14(obj, sl); sl += 0x70
  add  sb, sb, #1
  cmp  sb, r4
  blt  loop
```

Each constructor writes a vptr, and the RTTI (`docs/07`) turns those three
pointers into names immediately:

| tag | size | vptr | class |
| ---: | ---: | --- | --- |
| 0 | `0x7C` | `0x0213AC40` | `Battles::UnitBlueprint` |
| 1 | `0x74` | `0x0213AC04` | `Battles::ProjectileBlueprint` |
| 2 | `0x70` | `0x0213AA1C` | `Battles::CollectableItemBlueprint` |

All three derive from `Battles::EntityBlueprint`, whose constructor
`func_020010B0` stores the record pointer at object `+4`.

**4. Read the second pass.** A second loop calls `func_02001684(bp, pool)`,
which calls `func_02001674` twice — on `record+0x00` and on `record+0x24`:

```arm
func_02001674:
  ldr r2, [r0]        ; the stored value
  add r1, r1, r2      ; + pool base
  str r1, [r0]        ; write the pointer back
  bx  lr
```

So those two fields hold **offsets from the string pool**, relocated in place at
load. On disk they are still offsets, which is what the parser here reads.

**Note the type dispatch is not a jump table** — it is three compares and a
fallthrough that increments the counter *without advancing the record pointer*.
That means a fourth type value would desynchronise the whole file rather than
being skipped. The loop is therefore only correct because every record really is
type 0, 1 or 2, and the walk landing exactly on the pool is what proves it.

## The self-test

The format validates itself, and every tool here checks it:

- The walk starts at `0x04` and must land **exactly** on the string pool. It
  does: `0x0FA08`.
- 551 records × 2 string fields = 1,102 references, and the pool holds exactly
  **1,102 entries with none unreferenced**.

Either check failing means the record sizes or the type tag are wrong. Both
passing at once is hard to achieve by accident.

## `Battles::UnitBlueprint` — the 0x7C record

Fields marked **verified** have direct evidence, named beside them. The rest are
measured but unnamed.

| offset | width | field | evidence |
| --- | --- | --- | --- |
| `+0x00` | u32 | **name offset** into the pool | `func_02001674` relocates it |
| `+0x04` | **u16** | **own index** | equals the record's position, **0..550** (CORRECTED, see below) |
| `+0x06` | u16 | **display-name string id** | `LANG[385]` is `"[missing BP name]"` and is the value used by everything never shown; 182 units map to 386.. in file order |
| `+0x08` | u8 | **type tag** | the loader's dispatch |
| `+0x0C` | **u32** | **movement speed**, tiles per tick in 20.12 fixed point | the rebalance sheet's `Speed` column matches on 18 of 22 named units; `docs/26` proves the scale and corrects the width |
| `+0x15` | u8 | unnamed — a five-value enum, zero for every structure | **not** the speed; see the retraction below |
| `+0x24` | u32 | **asset path offset** | `func_02001674` relocates it |
| `+0x10` | **u32** | unnamed — a rate limit, `12.0` for 81 mobile units, `0.3333` for nine, `0xFFFFFFFF` for all 92 structures | characterised in `docs/26`; deliberately not named |
| `+0x14`…`+0x2D` | — | unnamed; a few small enums, the patch edits `+0x18`, `+0x1A`, `+0x1B`, `+0x1D` |
| `+0x2E`…`+0x59` | — | runtime pointer slots, filled `0xA1000000` on disk | byte-identical in all 182 records |
| `+0x5C` | u8 | **category** | 21 values; see `docs/12` |
| `+0x5E` | u16 | **cost** | round economy values (50/75/100/150/250/…); identical across factions for every building |
| `+0x60` | u16 | unnamed — build or train time | monotone in cost within a faction; 150 for a builder, 1140 for the most expensive unit |
| `+0x62` | u16 | **health** | Castle 1500 > Mine 1250 > hero 1000 > melee 350 > builder 150; the rebalance sheet's Health column matches this field for both units checked |
| `+0x64` | u16 | **mana** | exactly `1000` for all 25 Hero-category units in the cartridge and `0` for every other unit and building — heroes are the only things that cast — and the rebalance patch varies it per hero (Wizard 800, Sorceress 2000, Lady Pirate 1200). The sheet's `mana` column matches it |
| `+0x66` | u16 | **projectile blueprint index**, `0xFFFF` = melee | Archer→`Arrow`, Ballista→`BallistaBolt`, Catapult→`Boulder`, Gryphon→`Fireball`, Tower/II/III→`TBolt`/`TBoulder`/`TFireball` |
| `+0x68` | u8 | **melee damage** | zero for every ranged unit; see the Wizard case below |
| `+0x6A` | u8 | unnamed — 0..20, non-zero only on melee units | |
| `+0x6D` | u8 | **attack interval** | matches the rebalance sheet's "Atk Interval" column exactly |
| `+0x6F` | u8 | **attack range** | 1 for melee, 5/6/7 for the three tower tiers, 8 for a catapult; matches the sheet's "Range" |
| `+0x70`, `+0x71` | u8 | unnamed | |
| `+0x72`…`+0x76` | u8 | unnamed — non-zero only on heroes | |

A note on what *not* to do with the unnamed fields. Small integers in these
records land inside the 0..550 blueprint-index range constantly, so annotating
every one as "this points at `K_Catapult`" produces a page of confident
nonsense — a `u16` of 7 is only a reference to record 7 if something says the
field is a reference. Nothing does yet. They are listed as unnamed for that
reason.

### Correction: `+0x0C` is a `u32`, and `+0x0E`/`+0x12` do not exist

This table read `0x0C..0x13` as four `u16` fields. It is **two `u32` fields**:
all 92 structures read `0xFFFFFFFF` at both, and all 90 mobile units have a
zero high half at `+0x0C`. `docs/26` has the evidence and the scale.

Because the high half is zero for every mobile unit, **no published speed value
changes** — only the structure sentinel does. The reading was wrong; the output
was not.

### Retraction: `+0x15` is not the movement speed

An earlier version of this table named `+0x15` "movement speed" on the strength
of one property: it is non-zero for all 90 mobile units and zero for all 90
structures, and **no other single byte offset has that property**. That was a
real measurement and it was not enough.

`+0x0C` has the same property (saturated for structures), and it is
the one the rebalance sheet's `Speed` column actually matches. The sheet's
column order was pinned by two anchors this project had already confirmed
independently — `Health` = `+0x62` and `Atk Interval` = `+0x6D`, `Range` =
`+0x6F` — and counting from those puts `Speed` exactly on `+0x0C`, which then
matches on 18 of the 22 rows the sheet names (the four misses are rows whose
numbers the PDF extraction split wrongly).

Two further checks settle it. `+0x15` takes only five values and they **overlap
completely** in `+0x0C`: `K_Engineer` and `K_King` are both `+0x0C` 410 but
`+0x15` 1 and 3. And `+0x15` groups by *role*, not by rate — all 24 Siege units
are 4, all 7 Transports are 5, 43 Hero/Melee/Mounted/Ranged units are 3 — while
`+0x0C` varies within every one of those groups.

So `+0x15` is an enum correlated with what a unit *is*, and it is left unnamed.
It is not a locomotion type either: its class 2 holds five Ranged units and one
Siege, and the Wizard's Crossbowman sits in class 1 with the builders.

**The lesson, which is now in the tips document:** a field that satisfies a
discriminator is not thereby the field you were looking for. "Non-zero exactly
when a unit can move" is satisfied by every per-unit attribute that structures
do not have.

### And two matches that are pure coincidence

The same correlation run scored the sheet's `Size` column at **20 of 22** against
`u8 +0x0D`, and its `range val` column at 17 of 22 against the same byte. Both
are nonsense: `+0x0D` is **the second byte of the speed field at `+0x0C`**. Speeds
run 273–956, so that byte is 1 for most units, and the sheet's `Size` is 1.0 for
most units. Two unrelated columns of mostly-ones agree at a high rate and mean
nothing.

A correlation is only evidence when the values are *varied*. `mana` scored lower
(16 of 22) and is certain, because its values are 405, 605, 700, 800, 900, 1000,
1200, 1750, 2000 and they line up with a category that makes sense. **Score the
variety, not just the hit rate.**

### The Wizard case, which named four fields at once

The community rebalance patch turns the Wizard from a melee hero into a caster.
Diffing its `Entities.ebp` against the cartridge's, record 20 changes in exactly
four places:

| field | vanilla | modded |
| --- | ---: | ---: |
| `+0x66` projectile | `0xFFFF` (none) | `186` = `TFireball` |
| `+0x68` melee damage | 30 | **0** |
| `+0x6D` attack interval | 20 | 40 |
| `+0x6F` attack range | 1 | 5 |

Gains a projectile, loses its melee damage, attacks half as often, reaches five
times as far. Four fields moving together in the one pattern the proposed names
predict is much stronger evidence than any of them alone, and it is the reason
those four are listed as verified while `+0x60` and `+0x70` are not.

## A caution about the rebalance sheet

The patch ships a PDF stat sheet. Its text is recoverable — the font subset
shifts every character by `+0x1D`, digits encode as `0x13 + d` and `.` as
`0x11` — and its column headers are useful: *Speed, Health, mana, Size, DPS, Atk
Interval, Range*. Its **numbers are the modded ones**, so they only ever confirm
a field by matching the patched file, never the cartridge. Two columns did match
byte-for-byte and are cited above; the row's column order could not be aligned
with the header row confidently enough to use the rest, and no field is named
here on the strength of the sheet alone.

## Repacking

The record region is fixed-size per type and the string pool is offset-addressed
from a base the loader supplies, so **a replacement name may be any length** as
long as the offsets are recomputed. Combined with the `COMP` container
rebuilding its own size table (`docs/05`), an `Entities.ebp` of a different
total size loads — which the community patch demonstrates at 18,148 packed bytes
against the cartridge's 17,760.

## Still open

- `BP/Factions.fbp` (`FANZ`, 10,016 bytes) — same treatment, from
  `func_020025D4`. Not yet done.
- `BP/Animations.abp` and the 136 `BP/*.tbp` tile blueprints.
- The `ProjectileBlueprint` (0x74) and `CollectableItemBlueprint` (0x70) stat
  fields. The rebalance patch touches projectile offsets `0x61`–`0x72`, so those
  are the tunable ones.

## CORRECTION — `+0x04` is a u16 over 0..550, not a u8 over 0..181

This document originally declared `+0x04` a `u8` holding the record's own index
in the range 0..181. Both halves were wrong, and the error is the kind
`docs/26` and `docs/60` warn about: the offset was right and the ACCESS WIDTH
was not.

The game reads it with `ldrh`, never `ldrb`:

    func_02002438  0x02002464   ldr r1,[r0,#4] ; ldrh r1,[r1,#4]
    func_02000E78  0x02000F54   ldrh
    func_020B83D4  0x020B8436   ldrh

And the range is the whole file, not just the units. Decoding all 551 records
of `BP/Entities.ebp`, `u16 @ +0x04 == global file index` for **551 of 551** --
`UnitBlueprint` 0..181, `ProjectileBlueprint` 182..208,
`CollectableItemBlueprint` 209..550.

A `u8` read is therefore wrong for the **369 records at index >= 256**, where
it returns `index & 0xFF` and `+0x05` is non-zero. Nothing derived from the
VALUE in this document changes; only the declared width and range do.

`+0x04` is also the canonical way to get a blueprint's index from its record.
Port code that instead divides by the record stride -- `(rec - base - 4)/0x7C`
-- is correct only for units, because the stride is `0x74` and `0x70` for the
other two types.
