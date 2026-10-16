# The twenty entity categories, confirmed from code

**Status: the category taxonomy is no longer inferred from the blueprint table —
it is confirmed by the function that reacts to unit counts. `blueprint+0x5C`
splits at exactly 7: categories 0–6 are units, 7–19 are structures, and the
simulation treats the two halves completely differently. Category 1 — Builders —
is special-cased ahead of the split.** Annotation 54.1% → 54.1%; **98 of 98
checks pass**.

## The route in

`docs/71` left 30 mission-API entry points unnamed and pointed at the player
sub-objects at `+0x4C` and `+0x50`, which no document had touched. Bulk-classifying
the 30 showed a recurring idiom — `Vector_Init`, `GetPlayerList`, an element
accessor, `Vector_Free` — and a family of five setters reaching the player
through `GetPlayerByIndex`.

Two of those, `0x02048324` (31 missions) and `0x020482D4` (27 missions), write a
`u8` pair at `+0x2C`/`+0x2D` of `player->obj_4C`. The mission scripts say what
the pair is for, the same way they did for the vision radius in `docs/69`:

```
0x02048324   values 0 x13, 2 x8, 3 x3, 4 x10     34 call sites
0x020482D4   values 0 x18, 2 x10                 28 call sites
```

**Small integers, not flags.** So the pair is a count, and the missions are
handing out between zero and four of something.

## The function that names it

`func_0208AA34` runs the *same* `+0x2C`/`+0x2D` maintenance, but in the
simulation rather than the mission API — and it reaches it through a category
test:

```c
void Player_OnUnitCountChanged(Player *p, Entity *e, u8 value) {
    if (value) p->flag_32 = 1;
    u8 cat = e->blueprint_8->category_5C;      /* the [entity+8] idiom */
    if (cat == 1) {
        Ui *sub = p->obj_4C;
        if (!value) { func_020963A0(sub, 0, 0, 0); return; }
        if (sub->cursor_2D < value) {
            sub->cursor_2D = value + 1;
            if (sub->count_2C > sub->cursor_2D) sub->count_2C = sub->cursor_2D;
            func_02096578(sub)->field_6E = sub->cursor_2D;
        }
        sub->count_2C = value;
        return;
    }
    if (cat > 0x13) goto units;
    switch (cat) { case 0 ... 6: units: skip = 0; break;
                   case 7 ... 19:       skip = 1; break; }
    if (!skip) func_0208B30C(p->obj_50, e, value);
}
```

`docs/data/entities.tsv` settles what category 1 is:

```
cat 1   n=6   Builder, Builder, Builder, Builder, Builder, Builder
```

**Category 1 is the Builder — one per faction, six in the game.** So the pair on
`player+0x4C` is the player's builder count, and a mission granting `2`, `3` or
`4` is setting how many Builders that player starts with.

## The split at 7

The twenty-arm jump table is not twenty behaviours. It is a **two-way split**
that the compiler expanded:

| categories | arm | behaviour |
| --- | --- | --- |
| 0, 2, 3, 4, 5, 6 | `0x0208AAD8` | `func_0208B30C(player->obj_50, entity, value)` |
| 7 – 19 | `0x0208AAD4` | **nothing — returns immediately** |
| 1 | *before the table* | maintains the builder pair on `player->obj_4C` |

And the category list confirms exactly what that boundary is:

| cat | contents | | cat | contents |
| ---: | --- | --- | ---: | --- |
| 0 | Heroes — King, Queen, Wizard, Brickbeard | | 10 | Farm, Graveyard, Plantation, Biodome |
| **1** | **Builder** | | 11 | Barracks, Mausoleum, Breeding Pit |
| 2 | Melee — Guardsman, Skeleton, Swordsman | | 12 | Special Factory, Cauldron, Drydock |
| 3 | Ranged — Archer, Gunman, Laser Trooper | | 13 | Tower |
| 4 | Mounted — Knight, Horseman, Cavalry | | 14 | Tower II |
| 5 | Transport Ship | | 15 | Tower III |
| 6 | Siege/Special — Ballista, Catapult, Dragon | | 16 | Shipyard |
| 7 | Base — Castle, Fortress, Pirate's Hold | | 17 | Bridge |
| 8 | Lumber — Mill, Shack, Harvester | | 18 | Gate |
| 9 | Mine, Quarry, Well Cap | | 19 | Wall |

**Categories 0–6 are every unit; 7–19 are every structure.** The boundary is not
a convention read off a spreadsheet — the simulation branches on it, and
gaining or losing a *building* does not run this path at all.

## It cross-checks `docs/68`

`docs/68` derived the nine build-limit classes from a different function and a
different table. They agree, which neither pass arranged:

```
class 1  Minifigures  categories 1-4    Builder, Melee, Ranged, Mounted
class 2  Specials     categories 5-6    Transport, Siege
class 3  Towers       0x0D, 0x0E, 0x0F  Tower, Tower II, Tower III
class 6  Walls        0x13              Wall
class 7  Bridges      0x11              Bridge
class 8  Gates        0x12              Gate
```

Every one lands on the right names. **Two independent readings of two different
tables producing one consistent taxonomy is the strongest evidence this project
has for the category numbering.**

## What `player+0x4C` and `+0x50` are

Both are sub-objects the player owns, and the five mission setters write through
them:

```
player+0x32          a byte the mission API and the simulation both set
player+0x4C -> +0x2C  builder count
            -> +0x2D  a cursor bound, kept at >= count + 1
            -> +0x3A  written by 0x02048380
            -> +0x3E  written by 0x0204835C
player+0x50 -> +0x14  written by 0x0204835C
            -> +0x15  written by 0x02048228 and 0x02048380
```

The `+0x2D`-stays-above-`+0x2C` invariant and the mirror into `+0x6E` of an
object two dereferences away read as a **HUD panel** — a builder carousel needing
one more slot than it has builders. **That reading is not proven** and is
recorded as an interpretation, not a name; what is proven is the arithmetic.

## One call that is dead in practice

`0x02048380` writes `player[0x4C]+0x3A` unconditionally, and its other two
fields **only when the value is nonzero**. Across all 92 overlays it has **31
call sites and 29 pass literal zero** — so the conditional half essentially never
runs in the shipped game. A port that implements only the unconditional write
would be indistinguishable in every mission but two.

## Tips and pitfalls

* **When mission scripts pass small integers, they are telling you the type.**
  0/2/3/4 across 34 sites is a count; the vision pass got its answer the same
  way.
* **A twenty-arm jump table can be a two-way branch.** Reading the table as
  twenty behaviours would have invented eighteen distinctions the code does not
  make.
* **Base-register false positives, again.** A first scan for functions touching
  `+0x2C`/`+0x2D` near a `+0x4C` load returned 156 hits, almost all `[pc, #0x2c]`
  literal loads and stack offsets. Requiring the *same register* cut it to a
  handful. This is the third pass to hit this; the rule is to match the register,
  not the constant.
* **Check before reading, again.** Tier T already covered 69 of the 118 12-byte
  ARM thunks and named functions covered 41 more, leaving 8 — a bulk win that
  was not there. Five minutes of counting saved an hour of implementing it.
* **Two tables agreeing is worth more than either table.** `docs/68` and this
  pass never share a function, a constant or a method.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **54.1%** | 2,501 listed + 2,539 derived, 95 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
