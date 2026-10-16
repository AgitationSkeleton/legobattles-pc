# Reading a field correctly: the idiom, the width, and the scale

**Status: `+0x0C` is re-read as a `u32` and proved to be tiles-per-tick in 20.12
fixed point. `+0x10` is characterised and deliberately left unnamed. A new tool,
`tools/lb_field.py`, and two bugs it exposed, are recorded here.**

This pass produced one named field, one refused name, and a method. The method
is the part worth keeping.

## The problem: displacement is not a search key

Asking "what reads `UnitBlueprint+0x70`?" by grepping the disassembly for
`ldr rX, [rY, #0x70]` returned **706 functions**. That is not a lead. `0x70` is
a common displacement into a hundred unrelated structures, and no amount of
reading will separate them.

But `docs/25` established that a blueprint read has a *shape*. Every entity
carries its blueprint at `Entity+0x08`, and the simulation never caches it — so
the read is always a pair:

```arm
ldr  rB, [rE, #8]        ; entity -> blueprint
...                      ; a few instructions may intervene
ldrX rV, [rB, #disp]     ; blueprint -> the field
```

Requiring the pair — second load's base register is the first's destination,
and not clobbered in between — is what makes the question answerable.
`tools/lb_field.py` does this. On `+0x70`/`+0x71` it cut 706 down to **11 and
9**.

### The control that makes it trustworthy

A filter that returns a small answer is not thereby a correct filter. The check
is to run it on a field whose readers are already known. `+0x60` is build time,
and `docs/25` named its two readers by hand:

```
$ python tools/lb_field.py 0x60 --cpu arm9 --names-only
blueprint +0x60: 9 function(s) ...
  Battles::Sim::ConstructProgressAction::virtual_02  @0x020523A8
  Battles::Sim::RepairStructureAction::virtual_02    @0x02054B84
  ... plus seven in the 0x02000C.. region
```

Both known readers, found independently. That is what licenses using the tool
on fields whose answers are not known.

> **Corrected by `docs/27`.** The seven others were described here as "the
> blueprint-parser region". They are not the parser: they are
> `Sim::CollectableItem` accessors, and they read
> `CollectableItemBlueprint+0x60` — a `u32` reference field — not
> `UnitBlueprint+0x60`. `Entity+0x08` points at a *different record type* for
> each of the three entity subclasses, so a displacement search over the idiom
> silently mixes three layouts. The access width is the tell: build time is
> read with `ldrh`, the reference field with `ldr`.

### Pitfall: `[pc, #8]` is not `[entity, #8]`

The first version reported three confident hits on `+0x10`. All three were
wrong, and all three failed the same way:

```arm
ldr  r1, [pc, #8]        ; a literal-pool fetch -- displacement 8
ldr  r1, [r1, #0x10]     ; ...so the next load looks like a blueprint read
```

A PC-relative load is byte-for-byte the shape of the entity-to-blueprint hop.
The tool now rejects `pc`/`r15` as a base outright. **A structural filter needs
to exclude the addressing modes that mimic it**, not just match the one it
wants — and the way this was caught was that all three hits sat in the SDK
region at `0x0210xxxx`, nowhere near the simulation.

## The width trap, hit again

`docs/11` and `docs/12` carried `+0x0C` as a `u16` movement speed and `+0x0E`,
`+0x10`, `+0x12` as three separate `u16` fields. Dumping the raw bytes across
the roster says otherwise:

```
K_Swordsman   9a 01 00 00   00 c0 00 00
K_Catapult    9a 01 00 00   55 05 00 00
K_Castle      ff ff ff ff   ff ff ff ff
```

`0x0C..0x13` is **two `u32` fields**, not four `u16`s. The evidence is the
sentinel: all **92 structures** read `0xFFFFFFFF` at both, and all **90 mobile
units** have a zero high half at `+0x0C`. A pair of independent `u16`s would
not both be saturated on exactly the structures and both be clean on exactly
the units.

This is the same trap recorded in the LSW2 notes — *match the width the code
uses, not just the offset*. Here it cost nothing in published numbers, and that
is worth being precise about rather than glossing: **every speed value in
`docs/data/entities.tsv` was and remains correct**, because the high half is
zero for every mobile unit. Only the structure sentinel changed, from `0xFFFF`
to `0xFFFFFFFF`. The reading was wrong; the output was not. Both halves of that
belong in the record.

## `+0x0C` is tiles per tick, and it derives 30 Hz a third time

Divide the twelve distinct speed values by 4096 and multiply by 30:

| stored | ÷4096 (tiles/tick) | ×30 (tiles/s) |
| ---: | ---: | ---: |
| 273 | 0.0667 | **2** |
| 341 | 0.0833 | **2.5** |
| 375 | 0.0916 | **2.75** |
| 410 | 0.1001 | **3** |
| 444 | 0.1084 | **3.25** |
| 478 | 0.1167 | **3.5** |
| 546 | 0.1333 | **4** |
| 614 | 0.1499 | **4.5** |
| 683 | 0.1667 | **5** |
| 819 | 0.2000 | **6** |
| 853 | 0.2083 | **6.25** |
| 956 | 0.2334 | **7** |

Twelve values, every one a clean quarter-tile-per-second. And the check runs
the other way too: `round(tiles_per_sec / 30 * 4096)` reproduces the stored
integer for **all twelve** — 3.0 → 409.6 → 410, 4.5 → 614.4 → 614,
7.0 → 955.73 → 956. Rounding *to* the stored value and recovering *from* it are
different arithmetic, and both landing exactly is not something a wrong scale
factor survives.

So the designers entered whole and quarter tiles per second, and the export
pipeline divided by the tick rate and scaled to 20.12 fixed point.

**This is the third independent derivation of the 30 Hz tick**, and the three
share no inputs: `docs/20` got it from the damage-per-second identity,
`docs/25` from 180 of 180 build times being exact multiples of 30, and this
from twelve speeds landing on quarter-tile boundaries. Three unrelated fields
agreeing on one constant is as close to settled as this project gets.

## `+0x10` is characterised, and is not being named

The second `u32`. In the same 20.12 fixed point it takes exactly two values:

| value | fixed-point | units |
| --- | ---: | ---: |
| `0x0000C000` | **12.0** | 81 mobile units |
| `0x00000555` | **0.3333** | 9 mobile units |
| `0xFFFFFFFF` | — | all 92 structures |

The nine are `K_Ballista`, `K_Catapult`, `W_Ballista`, `A_SiegeOne`,
`E_SiegeOne`, `E_SiegeTwo`, `I_Clipper`, `P_Barracuda`, `P_Battleship` — one or
two per faction, the standard buildable siege and naval line.

The tempting reading is a turn rate: 12.0 means "no limit", 0.3333 is a real
constraint, and a wheeled catapult should turn slowly. **Three things stop that
from being a conclusion:**

1. **Nothing reads it.** `lb_field.py` finds no simulation function that loads
   `+0x10` through the blueprint idiom. Neither does it find one for `+0x0C`,
   the field just proved to be speed — so this is not evidence the field is
   dead (see below), but it is the absence of the only evidence that would
   settle the meaning.
2. **The split is not the category.** It is *not* "Siege units turn slowly":
   16 of the 25 Siege units read `0xC000`, including `K_Gryphon`,
   `I_Battleship` and `WarJunk`. A rule that puts `P_Battleship` at 0.3333 and
   `I_Battleship` at 12.0 is not a rule about ships. **`docs/27` improves this
   considerably**: those 16 are the six bonus races' units, awarded for minikit
   sets and buildable by nobody, so the split is really "the standard buildable
   siege and naval line" against everything else. That is a coherent boundary,
   and it still does not say what the number measures.
3. **The unit is unknown.** 0.3333 per tick is 10 per second, but 10 *of what*
   is exactly the question, and radians, revolutions and eighth-turns all give
   defensible answers.

Recorded as: **a rate limit in 20.12 fixed point, disabled for all but nine
units, meaning unknown.** `docs/data/entities.tsv` carries it as `field_10`.

## Why the idiom search finds nothing for speed — and what that means

`+0x0C` is certainly read; the game moves units. That the idiom search misses
it is the useful result, and `docs/24` already contains the explanation:

> a buff does **not** modify a unit's stats in place. An influence object is
> allocated from a pool […] and the unit's effective numbers are computed into
> a `MagicInfluencedUnitStats`.

The simulation reads **effective** stats, not blueprint stats. The blueprint is
consulted where that derived block is *built*, and nowhere else in the movement
path. So a negative result from `lb_field.py` on a movement or combat scalar
means "this field is copied, not sampled" — it does **not** mean the field is
unused. Fields that *do* show up — build time, harvest range — are the ones
read directly because they are not buffable.

This retroactively explains several earlier dead ends and is the single most
useful thing learned this pass. It also names the next target precisely: the
function that populates `MagicInfluencedUnitStats` reads a long run of
blueprint displacements in one place, and naming it would name a block of
fields at once. **`docs/29` found it**: `func_0205CC14`,
`Sim::MagicInfluencedUnitStats::ResetToBase`, reached not by profiling but by
listing the corpus around the class's own vtable slots. It copies only **four**
blueprint fields, which is why the profile ranking never surfaced it — the
copy site is small, not large, and the assumption below that it would read "a
long run of displacements" was wrong.

`lb_field.py --profile` was written to find exactly that
function by ranking every function by how many distinct blueprint
displacements it touches. It did not find it: the top of the ranking is
two SDK functions matching on unrelated structures, and below them
`func_02053F90` and `func_02054438`, which read `+0x5C`–`+0x6B` **byte-wise** —
the signature of `ProjectileBlueprint`'s 14-flag block from `docs/11`, not of
`UnitBlueprint`. The copy site is reached some other way.

## A latent crash, from last pass's rename

Renaming `field_60` to `build_time` updated the parser and the TSV column list
but missed the `--type unit` summary printer, which still said
`r["field_60"]`. Regenerating the data files does not exercise that path, so it
stayed green. Found by reading, not by running.

**A rename in a dynamically-typed parser is not done when the tests pass; it is
done when every reference is found.** Fixed, and the summary printer is now
part of the regeneration check.
