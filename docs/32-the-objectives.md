# The thirteen objectives, and what every mission is made of

**Status: the objective taxonomy is closed and complete. All 13 concrete
objective classes are named from the ROM, all 13 are used, and every one of the
91 missions is broken down by which it uses.** `docs/data/mission_objectives.tsv`
carries the full matrix.

This pass also **corrects a coverage figure published in `docs/31`**, which is
recorded below rather than quietly fixed.

## Naming all thirteen at once

`docs/31` found thirteen factory functions at `0x02047308`–`0x02047514`, each
allocating 24–44 bytes and calling one constructor, and left them unnamed. They
did not need analysis — they needed the RTTI.

Each constructor stores a vtable pointer, and `docs/07` already recovered the
map from vtable address to class name. Walking each constructor to its first
vtable literal resolves all thirteen in one pass:

| factory | bytes | class |
| --- | ---: | --- |
| `0x0204732C` | 40 | `Battles::DestroyMarkedUnitsObjective` |
| `0x020474EC` | 32 | `Battles::TriggerAreaObjective` |
| `0x02047514` | 24 | `Battles::ManualCompletionObjective` |
| `0x02047384` | 36 | `Battles::ProtectMarkedUnitsObjective` |
| `0x0204746C` | 36 | `Battles::CollectMarkedItemsObjective` |
| `0x02047494` | 36 | `Battles::CollectMarkedUnitsObjective` |
| `0x020474C0` | 36 | `Battles::RetrieveMarkedUnitsObjective` |
| `0x020473B0` | 28 | `Battles::TimerObjective` |
| `0x0204740C` | 44 | `Battles::CreateUnitsObjective` |
| `0x02047358` | 36 | `Battles::RepairMarkedUnitsObjective` |
| `0x02047308` | 32 | `Battles::UserUIPenObjective` |
| `0x020473D8` | 40 | `Battles::DestroyUnitsObjective` |
| `0x02047444` | 36 | `Battles::CollectSomeItemsObjective` |

**The set is closed.** `Battles::ObjectiveBase` has exactly **13 concrete
subclasses** in the RTTI, and every one appears in the factory table — no
objective class without a factory, no factory without a class. `ObjectiveBase`
itself derives from `Battles::DListItem`, so a mission's objectives live in a
doubly-linked list, and `Battles::ScriptingMacros::MissionObjectives` is the
container that holds it.

## What the campaign is made of

Across all 91 overlays, **903 objective construction sites**:

| missions | sites | objective |
| ---: | ---: | --- |
| 80 | 310 | `DestroyMarkedUnitsObjective` |
| 51 | 143 | `TriggerAreaObjective` |
| 47 | 233 | `ManualCompletionObjective` |
| 28 | 52 | `ProtectMarkedUnitsObjective` |
| 23 | 50 | `CollectMarkedItemsObjective` |
| 14 | 39 | `CollectMarkedUnitsObjective` |
| 12 | 12 | `TimerObjective` |
| 11 | 21 | `RetrieveMarkedUnitsObjective` |
| 10 | 28 | `CreateUnitsObjective` |
| 7 | 12 | `RepairMarkedUnitsObjective` |
| 1 | 1 | `DestroyUnitsObjective` |
| 1 | 1 | `CollectSomeItemsObjective` |
| 1 | 1 | `UserUIPenObjective` |

**LEGO Battles' campaign is, structurally, "destroy the marked units".** It
appears in 80 of 91 missions and accounts for a third of all objective sites.
The next two are scaffolding rather than goals: `TriggerAreaObjective` fires
when a unit enters a region, and `ManualCompletionObjective` is completed by
script — together they are how a mission sequences itself.

Three objectives are used **exactly once each** in the entire game.
`UserUIPenObjective` is almost certainly the stylus tutorial.

Objectives per mission: **minimum 1, maximum 48, mean 9.9**. The busiest are
`ov80` (48, mostly Destroy plus Manual), `ov31` *Monkey Business* (32, with ten
`CollectMarkedUnits` and ten `Protect` — an escort mission), and `ov54`
*Stand Fast* (30, of which 22 Destroy and a Timer — a survival mission). A
typical one is `ov19` *Something's Out There*: one Destroy, one Protect, one
Retrieve, one TriggerArea.

**These are static call sites, not runtime instances.** A mission that branches
— difficulty variants, alternate paths — counts every site in its code, so the
numbers describe what a mission *can* create, not what one playthrough sees.

## Correction: `docs/31`'s 93.9% was span, not coverage

`docs/31` reported "93.9% of overlay bytes reached". **That figure measures the
wrong thing**, and the error is worth spelling out because it is easy to repeat.

`lb_scan.walk` returns a `high` water mark — the furthest address it touched.
The overlay tool treated `high - addr` as the function's covered bytes. It is
not: **a single branch near the end of a function raises `high` to the end while
the walk skips everything between**, so a span can be reported as fully covered
when most of it was never visited.

It was caught by a contradiction, not by inspection. The tool reported overlay 8
as having **zero objectives**, yet reading the same overlay linearly showed it
calling four different objective factories. The extent was right and the
coverage claim was wrong.

> **Superseded by `docs/39`.** The 49.1% below is an honest measurement of
> *the walk*, not of the overlays, and is 47 points low. The config's own
> per-overlay function list gives a verified 100%. The objective findings in
> this document came from the linear sweep and are unaffected.

Corrected figures, now reported side by side:

```
overlays  88,294 of 94,048 RAM bytes in a function extent  -> 93.9%
          46,194 bytes actually VISITED by the walk        -> 49.1%
```

**Both are kept**, because they answer different questions, and the ARM9 and
ARM7 rows in `docs/30` use the *extent* definition — so removing it would make
the overlay row incomparable with the others. What was wrong was calling the
extent figure "reached".

The lesson generalises past this tool: **an end address is not a coverage
measurement.** Any walker that reports extents is reporting where it stopped,
not what it saw.

### And a second blind spot, from the same root

Fixing the count meant sweeping the code linearly rather than trusting the
walk's call list. The first attempt swept only the walk's function extents —
and `UserUIPenObjective` vanished from the tally, because its single call site
in the whole game sits outside any extent the walk established. The sweep now
covers the **entire overlay range**, with a filter that only counts a `bl` whose
target is already a known ARM9 function. Data decoding into a branch that lands
exactly on a known function entry is not a realistic accident.

That filter is what makes a linear sweep safe here, and it is the reason the
two techniques are used together: **the walk establishes what is code, the
sweep counts what it calls.**

## What this gives a port

`docs/31` established that missions are compiled C++ over a macro layer, not
data. This narrows what a reimplementation has to provide: **thirteen objective
types, one linked list, and a container.** A mod that wants new missions needs
those thirteen behaviours and the 96-entry API around them — a bounded surface,
and now an enumerated one.
