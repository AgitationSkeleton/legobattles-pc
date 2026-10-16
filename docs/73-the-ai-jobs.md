# The AI: six jobs, one empty interface

**Status: the AI subsystem is mapped for the first time. `Battles::User::JobBase`
is a *pure* interface — all six of its virtuals are 4-byte empty functions — and
six concrete jobs override every one of them across 10,912 bytes. One of the six
interface methods is empty in all seven classes.** **101 of 101 checks pass**;
annotation unchanged at 54.1%, because every function this pass identifies was
**already named** from the RTTI symbols.

## Following the thread from `docs/72`

`docs/72` ended on `func_0208B30C` — what gaining a non-Builder unit does. It
upserts into a vector at `player->obj_50 + 0x54`:

```c
if (Find(obj50, entity, &value, &index))       /* func_0208B378 */
    vec->data[index]->count_C = value;         /* update in place */
else {
    n = Heap_New(0x18, file, 163);
    n->vtable = SListItem;                     /* base ctor  */
    n->next   = 0;
    n->vtable = User::RequestItem;             /* then derived */
    ...
    n->entity_8 = entity;
    n->count_C  = value;
    Vector_PushBack(obj50->vec_54, n);
}
```

The two vtable stores resolve through `rtti.json` to `Battles::SListItem` then
**`Battles::User::RequestItem`** — the base-then-derived constructor pattern,
and the class name is the whole answer.

The search keys on **the `u16` two hops from the entity** — `entity->[+4]->+4` —
on both sides of the comparison, so entities of the same type collapse to one
record. `player+0x50` holds **one request per unit type, with a count**.

`docs/71` listed `User::RequestItem` as one of 25 `SListItem` subclasses without
knowing what it was. It is the AI's shopping list.

## Nothing documented the AI

Searching the RTTI for the neighbours of `RequestItem` turns up a subsystem no
document in this project had touched — **20 planner classes**:

| group | classes |
| --- | --- |
| jobs | `JobBase`, `JobAttack`, `JobAttackTaskForce`, `JobContractBuild`, `JobDefend`, `JobRepair`, `JobScout` |
| squads | `Squad`, `ArmySquad`, `SquadManager`, `ArmySquadManager`, `ResourceSquadManager` |
| wants | `RequestItem`, `UnitsRequired`, `TransportRequestItem`, `DesiredSpellCategory`, `TargetItem`, `BuildCommandItem` |
| tabs | `BuildTab`, `UnitsTab` |

The six "wants" classes are all `SListItem` subclasses with **2-slot vtables** —
just the destructor pair. They are **pure data nodes**: the AI's desires are
records in lists, not objects with behaviour.

## `JobBase` implements nothing

```
class                          base       slots   every slot
JobBase                        -            6     4 bytes each
JobAttack                      JobBase      6
JobAttackTaskForce             JobBase      6
JobContractBuild               JobBase      6
JobDefend                      JobBase      6
JobRepair                      JobBase      6
JobScout                       JobBase      6
```

**All six of `JobBase`'s slots are the 4-byte empty function** `docs/61`
characterised — `bx lr` and nothing else. There is no shared behaviour, no
default, no template method. Every job reimplements the entire interface.

For a port that matters: `JobBase` is an abstract base with no code, so it
contributes nothing to link order or to the byte-matching effort.

## What the six slots are

Comparing implementation sizes across the seven classes names the interface:

| slot | JobAttack | AttackTaskForce | ContractBuild | Defend | Repair | Scout | reading |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 0 | 220 | 364 | 84 | 128 | 152 | 292 | **query** — switches on a byte 0–11, returns bool |
| 1 | **1716** | **2904** | 628 | 796 | 956 | 1232 | **tick** |
| 2 | 56 | 52 | *4* | *4* | 104 | 80 | **serialise out** |
| 3 | 56 | 52 | *4* | *4* | 112 | 88 | **serialise in** |
| 4 | *4* | *4* | *4* | *4* | *4* | *4* | **nothing, in all seven** |
| 5 | 112 | 412 | 36 | 72 | 36 | 108 | **start / reset** |

Slots 2 and 3 give themselves away — `JobScout`'s and `JobRepair`'s are nearly
identical chains that early-out on failure:

```c
if (func_0204B814(this + 0x28)) return;   /* a wide field */
tmp = this->state_18;  if (func_0204B75C(&tmp)) return;   /* u8  */
if (func_0204B78C(this + 0x30)) return;                   /* u16 */
if (func_0204B78C(this + 0x32)) return;                   /* u16 */
if (func_0204B75C(this + 0x34)) return;                   /* u8  */
...
```

**Field-at-a-time serialisation with an error early-out** — and the two slots
are the same shape at nearly the same size, which is a read/write pair.
`JobContractBuild` and `JobDefend` have **4-byte empties for both**, so those two
jobs carry no state worth saving.

Slot 5 is the reset:

```c
this->counter_36 = 0;
this->state_18 = (this->subject_14 && func_0208FDF4(...)) ? 1 : 4;
```

So `job+0x14` is the thing the job is about, `job+0x18` is its **state enum**,
and `job+0x36` a `u16` counter. Slot 0's switch on a byte with twelve arms is an
event kind being offered to the job.

**Slot 4 is a 4-byte empty in all seven classes.** It is a virtual method the
interface declares and nobody implements — dead weight in the shipped ROM, and
something a reimplementation can drop entirely.

## What the AI actually is

The complexity ranking falls straight out of the tick sizes:

```
JobAttackTaskForce  2904 B      coordinated multi-squad attack
JobAttack           1716 B      a single attack
JobScout            1232 B
JobRepair            956 B
JobDefend            796 B
JobContractBuild     628 B
```

**The AI's largest single function is the coordinated attack**, and it is nearly
twice the plain attack. The economy side — `JobContractBuild` — is the *smallest*
job in the game, which says the opponent's build order is comparatively simple
and its aggression is where the work went.

Squads are thin by comparison: `Squad` and `ArmySquad` have 2-slot vtables, and
the three managers 1 slot each — **404 bytes total** across all five classes. The
squad layer is bookkeeping; the jobs hold the behaviour.

## Tips and pitfalls

* **A base class with only empty virtuals is a finding, not a gap.** It rules
  out shared behaviour, which is exactly what you need to know before porting.
* **Two vtable stores in one constructor name two classes.** Base then derived;
  the second is the one you want.
* **Sizes read the interface when names do not.** Nothing here needed
  disassembling to rank: a 2904-byte slot next to a 4-byte slot in the same
  column tells you which is the tick and which is a stub.
* **Check whether the win exists before building it.** All 42 job slot functions
  were *already* named from `rtti_symbols.txt`, so this pass moved annotation by
  zero. That was worth knowing in one query rather than after writing a tier.
  The same check killed a 12-byte-thunk tier last pass.
* **Offset matching without object identity is noise.** A scan for functions
  storing to both `+0x4C` and `+0x50` returned 81 hits spanning animations,
  screens and blueprints. Third variation on the same pitfall; the fix is always
  to tie the offsets to the same register.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 54.1% | 2,501 listed + 2541 derived, 97 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
