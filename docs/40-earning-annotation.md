
# Earning annotation: 26.9% to 32.1%

**Status: 489 functions annotated from evidence in the image — 135 named from a
string they reference, 354 attributed to a class by their callers. Annotation
moves from 26.9% to 32.1%, and the two derivations are reported as separate
tiers because they are not equally strong.**

The annotation figure had not moved since `docs/07`. That is not neglect: 2,454
of the 2,499 came free from the shipped RTTI in a single pass, and once that
seam was mined there was no second free source. This pass looks for what the
image supports that no source *lists*.

## First, a check that closed a segment

The ARM7's wram is the only code segment under 100%, so by the ordering it came
first. Last pass's lesson — *when a measurement depends on a tool you wrote, the
error bar is the tool's* — applies to it exactly: the ARM7 corpus comes from
this project's own walk, and the overlays had just been rescued by a
third-party source nobody had thought to load.

So: **does the `ds-decomp` configuration cover the ARM7?**

It does not. `config/eur/` and `config/usa/` each contain **`arm9` and nothing
else**. That project never analysed the ARM7, and there is no second source to
find. `docs/33`'s ceiling stands on its own evidence, and the ARM7's 71.5% is
not an artefact waiting to be corrected.

Worth two minutes to establish, and it is the difference between "believed at
its ceiling" and "checked".

## Tier S — named from a referenced string

The ARM9 carries **1,849 NUL-terminated strings**, of which 93 are paths and 52
are format strings. **244 functions reference at least one; 196 reference
exactly one.**

The restriction to exactly one is the whole method. A function loading five
strings is a dispatcher or a formatter, and its strings describe what it
*prints*. A function loading one string is usually *about* that string:

```
0x02001E70  "BP\entities.ebp"      -> the Entities.ebp loader
0x020025D4  "BP\factions.fbp"      -> the Factions.fbp loader
0x02001370  "UI/GamePlayerCards/"
0x02035028  "KingFaction.NCLR"
```

**135 of the 196 were previously unnamed.** Among them a run at `0x0203EE98`
onward, one function per campaign map — `ma2_1`, `ma3_1`, `ma1_1`, `ma2_2` —
which is `docs/08`'s campaign table seen from the code side, having been built
there from rodata adjacency.

## Tier C — attributed to a class by its callers

Building the ARM9 call graph gives **25,429 edges over 5,559 callees**. For each
unnamed function, ask whether **every** caller is a method of the same RTTI
class. If so, it is that class's private helper.

**354 functions qualify, across 143 classes:**

| helpers | class |
| ---: | --- |
| 15 | `Battles::User::WorldViewScreen` |
| 11 | `Battles::Sim::PatrolEntityCommand` |
| 11 | `Battles::Sim::Simulation` |
| 10 | `Battles::Sim::CombatHoldPositionEntityCommand` |
| 9 | `Battles::Sim::ResearchUpgradeEntityCommand` |
| 8 | `Battles::Sim::HarvestEngineerEntityCommand` |
| 7 | `Battles::Sim::AttackEntityCommand`, `CombatMoveEntityCommand`, `DieEntityCommand` |

The rule is **all** callers, not most. A function called by two classes is
shared infrastructure, and naming it after either would be wrong — that is the
kind of plausible-but-false annotation that is worse than none, because nothing
downstream would ever question it.

The distribution is itself a sanity check: the classes gaining most helpers are
the ones `docs/25` showed carry the real work — the entity commands and the
simulation — not, say, `Vector2s16`.

## Being honest about tier C

**It gives an owner, not a name.** `Sim::PatrolEntityCommand::helper` says where
a function lives, not what it does. That is the same limitation `docs/34`
records for the 347 generated stubs, and it is reported the same way rather than
quietly folded in:

```
2,501 of 9,314 functions  -> 26.9%
plus 489 derived: 135 named from a sole string reference, 354 attributed
to a class because every caller is one of its methods  -> 32.1%
```

`docs/30` exists to separate what was cheap from what was earned. Collapsing
these into one number would destroy the distinction it was built for — and tier
C, while genuinely earned, is closer to "cheap" than tier S is. A reader
deciding where to spend an hour needs to know that 354 of the 489 tell you only
a namespace.

## What did not work, and why it is recorded

Two ideas were measured and dropped before either got as far as a tool:

- **Naming by multi-string reference.** Loosening tier S from one string to
  "any" would add 48 functions and make every one of them a guess. The gain is
  under half a percent of the corpus and the cost is that the tier stops meaning
  anything.
- **Naming callees of named functions transitively.** A helper of a helper is
  not attributable — after one hop the caller set spans classes, and the rule
  that makes tier C safe stops holding. The measurement is that the strict rule
  already captures what the call graph supports.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling — **no second source exists** |
| ARM9 annotated | **32.1%** | 26.9% listed + 489 derived |
| data decoded | 6.8% | |
| decompilation | 0.143% | 351 functions byte-verified |

Annotation is now the segment with the most room that is still tractable.

> **`docs/41` found a third derivation**, so "the two cheap ones are spent" was
> premature by one: searching for *strided families of identical-size adjacent
> functions* exposed 209 C++ static-object initialisers. The lead came from
> measuring the corpus's structure rather than from reading any function.
