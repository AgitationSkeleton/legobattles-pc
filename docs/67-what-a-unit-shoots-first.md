# What a unit shoots first

**Status: `UnitBlueprint+0x70` is the target-priority score — the quantity the
combat commands maximise when choosing whom to attack. That is the last of
`docs/20`'s five open questions, and all five are now closed.** One new check;
**89 of 89 pass**.

`docs/11` recorded only that `+0x70` is "accumulated into `Entity+0x44` in the
combat path" and `docs/20` listed it as unidentified. Both were right and
neither was enough.

## Who reads it

`lb_field.py` finds eleven readers through the entity→blueprint idiom. Three are
the projectile impact path, where `+0x70` is the **`ProjectileBlueprint`**
second damage minimum (`docs/59`) — a different record type at the same offset,
the trap `docs/60` named.

The other eight are **four classes, two functions each**:

| class | accumulates | tests |
| --- | --- | --- |
| `Sim::CombatHoldPositionEntityCommand` | `0x020638C8` | `0x020631D4` |
| `Sim::CombatMoveEntityCommand` | `0x02065974` | `0x020651D0` |
| `Sim::CombatStandGroundEntityCommand` | `0x0206741C` | `0x02066DD0` |
| `Sim::PatrolEntityCommand` | `0x020709BC` | `0x0206FF04` |

**Every command that fights reads it and nothing else does.** Move, Patrol,
Hold Position and Stand Ground are exactly the four orders under which a unit
engages on its own initiative.

## It is a maximum, and the code says so

Inside the candidate loop:

```asm
ldrb r1, [blueprint, #0x70]     the candidate's score
add  r0, r0, r1
strh r0, [r4, #0x44]            running total of everything considered
cmp  r1, r6                     against the best so far
bhi  keep
...
keep:
mov  r6, r1                     r6 = new best
ldrb r1, [sp, #0x10]
ldrb r0, [sp, #0x11]
strb r1, [sp, #0x12]            and remember THIS candidate's tile
strb r0, [sp, #0x13]
```

`r6` is a running maximum, and the branch that updates it also copies the
candidate's tile into the slot the command later attacks. **`+0x70` is the score
being maximised.** Not a range, not a damage term — the code keeps the largest
and throws the rest away.

## The table

One value per category, **identical in all six factions** — the same shape
`docs/53` found for the buildings:

```
 45-65   Siege                the three tiers differ; everything else is flat
    40   Tower, Tower2, Tower3    all three tiers equal
    30   Hero
    20   Mounted
    18   Ranged
    15   Melee
    11   Builder
    10   Transport, Base
     8   VehicleCenter, Shipyard
     5   Barracks
     3   Mill, Mine
     2   Farm
     1   Wall, Bridge, Gate
     0   two decorative entities
```

Read it as an order of engagement and it is exactly what a designer would write:
**siege first, then towers, then the hero, then cavalry, archers, swordsmen.**
Production buildings rank above resource buildings; walls and bridges rank last
and are never worth attacking while anything else is in range.

Three details worth having:

* **All three tower tiers score 40.** Upgrading a tower makes it deadlier
  (`docs/53`: range 6, 6, 7) but no more attractive as a target.
* **A Builder (11) outscores a Transport or a Base (10).** A unit will break off
  to shoot an enemy builder rather than the building it is standing next to.
* **Siege is the only category whose score varies within it**, because the
  category lumps `SIEGEONE`, `SIEGETWO` and `SIEGETHREE` — 45, 55 and 65. Every
  other category holds one slot and one value.

## The running total, and what is not established

The same loop accumulates **every** candidate's score into `+0x44`, and the
partner function divides that total by a score and compares the quotient
against 3.

Read literally that is "is the threat around me more than three times some unit's
worth?" — a plausible disengage heuristic. **It is not established here.** The
divisor's provenance was not traced (whether it is the deciding unit's own score
or a candidate's), and a reading that hangs on which register holds what is
exactly the kind this project has had to retract before. The maximisation is
proven; the ratio is described and left open.

## `docs/20`'s five questions, all closed

| question | answer | where |
| --- | --- | --- |
| `ProjectileBlueprint+0x6B` | "explodes" — the 11 splash weapons | `docs/59` |
| the second damage pair `+0x70`/`+0x72` | impact damage vs the direct hit | `docs/59` |
| the effect id `+0x5D` | an animation-set id; 0 means no sprite | `docs/66` |
| `UnitBlueprint+0x71` | `interaction_range` | `docs/64` |
| `UnitBlueprint+0x70` | **target priority** | here |

Every one was closed the same way: **find who reads the field, read the branch,
then check the partition against the data.** None needed a new tool.

## Tips and pitfalls

* **The same offset in two record types is two fields** — `docs/60`'s rule,
  and it bit here too: three of the eleven readers of `+0x70` are reading a
  `ProjectileBlueprint`, where it means something else entirely. Separate them
  by which code region the reader lives in before drawing conclusions.
* **A `cmp`/`bhi`/`mov` triple around a running register is a maximum.** The
  giveaway is that the same branch also stores the candidate's other fields:
  the code is choosing, not measuring.
* **The readers being exactly one coherent set is evidence.** Four combat
  commands and nothing else is a much stronger signal than four arbitrary
  functions would be.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 53.7% | 2,501 listed + 2,504 derived, 50 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
