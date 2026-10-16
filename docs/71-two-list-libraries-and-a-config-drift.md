# Two list libraries, and names that never left the prose

**Status: an audit found 15 function names published in earlier documents that
were never written into `config/`, so the tools could not see them. All 15 are
landed, two of my own names from `docs/70` are retracted, and a new check keeps
the gap shut. The `docs/70` open question is answered: there are two list
libraries, not one.** Annotation 53.9% → 54.1%; **96 of 96 checks pass**. The
mission API goes from 51 named to 66.

## The pass that found its own bug

`docs/70` ended pointing at the 45 still-unnamed mission-API entry points. A
mechanical scan for the factory shape — `Heap_New(size, file, line)` followed by
a constructor call — found **11 of them in one contiguous run**, at line numbers
59 through 201 of a single source file:

```
0x02047308  new(0x20) line  59      0x02047444  new(0x24) line 155
0x02047358  new(0x24) line  79      0x0204746C  new(0x24) line 164
0x02047384  new(0x24) line  89      0x02047494  new(0x24) line 174
0x020473B0  new(0x1C) line  98      0x020474C0  new(0x24) line 184
0x020473D8  new(0x28) line 121      0x02047514  new(0x18) line 201
0x0204740C  new(0x2C) line 139
```

**The factory addresses increase monotonically with the retained `__LINE__`
values**, so the compiler emitted them in source order and the original file can
be laid out from the ROM. With the two named in `docs/70` that is **13
factories** — and `docs/32` says there are exactly **13 objective classes**.

`docs/32` had already named all thirteen. **They still read as unnamed**, because
those names live in a markdown table and were never written into
`config/symbols.tsv` or `config/read_functions.tsv`.

## Verifying rather than copying

The names were re-derived from the ROM rather than lifted from the prose: each
factory's constructor stores a vtable pointer, and `out/eur/rtti.json` maps
vtable addresses to class names.

```
0x02047308 -> Battles::UserUIPenObjective
0x0204732C -> Battles::DestroyMarkedUnitsObjective
0x02047358 -> Battles::RepairMarkedUnitsObjective
0x02047384 -> Battles::ProtectMarkedUnitsObjective
0x020473B0 -> Battles::TimerObjective
0x020473D8 -> Battles::DestroyUnitsObjective
0x0204740C -> Battles::CreateUnitsObjective
0x02047444 -> Battles::CollectSomeItemsObjective
0x0204746C -> Battles::CollectMarkedItemsObjective
0x02047494 -> Battles::CollectMarkedUnitsObjective
0x020474C0 -> Battles::RetrieveMarkedUnitsObjective
0x020474EC -> Battles::TriggerAreaObjective
0x02047514 -> Battles::ManualCompletionObjective
```

**All thirteen match `docs/32` exactly.** Independent re-derivation is what makes
landing them safe.

## Two names of mine retracted

`docs/70` named `0x0204732C` and `0x020474EC` structurally, as
`Mission_AllocObjective28` and `Mission_AllocObjective20`, from their allocation
sizes. They are the **`DestroyMarkedUnitsObjective` and `TriggerAreaObjective`
factories**, and `docs/32` had said so eleven passes earlier.

The tier R rows are replaced and `docs/70` is corrected in place. The failure is
the same one the audit found, turned on myself: **I read the disassembly without
checking whether the project already knew the answer.** `docs/70` opened by
congratulating itself for checking `docs/31` before sweeping the overlays, and
then in the same pass named two functions `docs/32` had already named.

## The drift, measured and closed

Scanning every document for an address paired with exactly one backticked name:

```
addresses named in documents            20
  present in config or annotations        5
  ADRIFT -- documented but not recorded  15
```

Thirteen objective factories, plus `Battles::Save::SaveGame` (`docs/23`) and
`Battles::Map::LoadTerrain` (`docs/15`). Those last two are landed with their
provenance recorded honestly: **named from document analysis, not RTTI-derived.**

That audit is now check 96, `no name published in a document is missing from
config`. It re-runs the scan every time and fails if any document names a corpus
function that `config/` does not carry.

## The answer to `docs/70`'s open question

`docs/70` flagged that mission list nodes and `docs/63`'s fog circles both use
`+0x10`, `+0x11` and `+0x12`, and guessed they might share a base class. The
RTTI settles it in one query, and **the guess was wrong**:

```
Battles::ObjectiveBase     -> Battles::DListItem     doubly linked
Battles::User::FogCircle   -> Battles::SListItem     singly linked
```

**There are two list libraries.** `docs/70`'s `List_PushBack`/`List_Remove` pair
(next `+4`, prev `+8`) is `DListItem`; the fog map's list is `SListItem` with
only a `next`. The shared offsets are coincidence — `docs/60`'s rule holding
again.

## What the split says about the design

**46 of 512 RTTI classes — 9% — are list nodes**, and which library a class picks
is a statement about how it dies:

| | count | membership |
| --- | ---: | --- |
| `SListItem` | 25 | `CommandBase`, `EventBase`, `EventListener`, `Task`, `Texture`, `PalettePtr`, `User::Squad`, `User::RequestItem`, `User::BuildCommandItem`, `User::FogCircle`, `UnitContainer` |
| `DListItem` | 21 | `ObjectiveBase`, `TimerBase`, `Effect`, `ParticleSystem`, `Sim::PathFinding::QueueInfo`, and **ten `*Animation` classes** |

The pattern is clean. **Singly linked for things drained in order** — commands,
events, requests, build queue items, the fog sources walked start to finish every
update. **Doubly linked for things that finish at arbitrary times and must be
unlinked from the middle in O(1)** — animations, effects, particle systems,
timers, objectives.

That is why `List_Remove` returns the node instead of freeing it: it is
`DListItem`'s unlink, and the caller owns the object. And it is why an
explosion animation ending mid-list costs nothing, while the fog source list is
only ever appended to and walked.

For a port: **the container choice is load-bearing.** Reimplementing every one
of these as a vector would be correct but would turn 21 classes' O(1) removals
into O(n).

## The objective record, read end to end

With the factories named, six more entry points fall out and the objective
model closes:

```
ObjectiveBase   +0x00 vtable   +0x04 next   +0x08 prev      (DListItem)
                +0x10 u8  complete
                +0x11 u8  the flag Mission_AddObjective defaults
                +0x12 u8  the mission script's key for this objective
```

`func_020472E4` is `Objective_MarkComplete`, and it is **idempotent by
construction**:

```c
if (!o->complete_10) {
    o->complete_10 = 1;
    Mission_OnObjectiveComplete(g_mission, o);
}
```

**Completing an already-complete objective does nothing at all** — not a
re-notify, not a duplicate event. A mission script that fires the same trigger
twice cannot double-count, and a port that drops the guard would emit two
completion events.

That makes the two keyed accessors legible as a pair, and they work on
*different* lists:

| | list | test | effect |
| --- | --- | --- | --- |
| `Mission_LookupByKey` `0x0204A070` | `+0x30` | `+0x12 == key` | returns `+0x10` |
| `Mission_CompleteObjectiveByKey` `0x0204A08C` | `+0x08` | `+0x12 == key` **and `+0x10 == 0`** | `Objective_MarkComplete` |

So the objectives list is the one at `mission+0x08` that `Mission_AddObjective`
appends to, and `mission+0x30` holds a second keyed collection that
`Mission_RemoveByKey` also clears.

A third family sits at `0x02048xxx`: setters taking `(u8 player_index, u8
value)`, resolving the player through `GetPlayerByIndex` and writing paired
flags into the player and its two sub-objects at `+0x4C` and `+0x50`.
`0x02048380` is the interesting one — it writes `player[0x4C]+0x3A`
unconditionally but the other two fields **only when the value is nonzero**, so
clearing it leaves the second pair set. Named structurally by the fields they
write; what those flags mean is not claimed.

## Tips and pitfalls

* **A name in prose is not a name in the build.** Fifteen functions were known
  and unusable for eleven passes because the knowledge stopped at markdown. Any
  project with a docs tier and a config tier needs a check that they agree.
* **Check the project before checking the ROM.** `docs/70` got this right for
  `docs/31` and wrong for `docs/32` in the same pass.
* **Retained `__LINE__` values reconstruct source order.** The 13 factories sort
  the same way by address and by line, which recovers the original file's layout
  for free.
* **Re-derive rather than copy when landing old conclusions.** All 13 were
  re-resolved through the vtables before being written to `config/`; that is the
  difference between propagating a result and propagating a mistake.
* **Inheritance answers offset questions that disassembly cannot.** One RTTI
  lookup closed a question `docs/70` had budgeted a constructor read for.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **54.1%** | 2,501 listed + 2534 derived, 89 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
