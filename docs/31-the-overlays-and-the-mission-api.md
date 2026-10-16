# The overlays, and the mission scripting API

**Status: all 91 overlays scanned, 372 functions recovered, 49.1% of their
bytes proven visited (93.9% lies inside a discovered function extent — the two
are different measurements, see `docs/32`). The interface the campaign is
written against is enumerated: 96 ARM9 entry points, of which 13 are a family
of objective factories, all named in `docs/32`.**

`docs/06` established that the overlays are the missions and then set them
aside, because **all 91 load at the same address, `0x0216A2E0`**. That fact kept
them out of the corpus (`docs/09`) and out of the coverage figures (`docs/30`)
for the whole project so far. This closes that gap.

## The discipline that makes it safe

There is no clever trick. `lb_code.AddressSpace` deliberately refuses to map
overlays unless told which one, because an address in overlay space is
ambiguous — mapping two at once would silently answer a question about mission
A with bytes from mission B, and nothing about the result would look wrong.

`tools/lb_overlays.py` therefore builds **91 separate address spaces**, each
holding the ARM9 static sections plus exactly one overlay, and keeps the results
in their own corpus file (`docs/data/overlay_corpus.tsv`) keyed by overlay id
rather than merging them into the ARM9's. **An address alone does not identify
an overlay function**, so the corpus stores the pair.

### The seeds are given, not guessed

A recursive-descent scan is only as good as its seeds, and a wrong seed produces
confident nonsense. These seeds are not inferred: every overlay's table entry
carries a `static_init` range — a pointer array of static constructors run when
the overlay loads — and **all 91 have a non-empty one**. The low bit of each
pointer gives ARM/Thumb exactly as it does in a vtable slot.

Two rules keep the walk honest: a call that leaves the overlay's range is
**recorded but not reported** as an overlay function, because it belongs to the
ARM9 and is already in that corpus; and a tier-2 literal is only followed if it
lands inside *this* overlay.

## Coverage

```
TOTAL 88,294 of 94,048 RAM bytes in a function extent -> 93.9%   (372 functions)
             46,194 bytes actually VISITED by the walk -> 49.1%
```

> **Superseded by `docs/39`.** Both this figure and `docs/32`'s 52.9%
> correction measured this project's own control-flow walk rather than the
> overlays. Against the ds-decomp config's per-overlay function list the
> overlays are a verified **100%** — 91,540 of 91,540 code bytes.
>
> **Corrected by `docs/32`.** This document originally reported the 93.9%
> figure as "bytes reached". It is not: it is the span of the discovered
> function extents, and a function's extent is a high-water mark, not proof
> every byte inside it was visited. The honest coverage figure is **49.1%**.
> Both are now reported, because the ARM9 and ARM7 rows in `docs/30` use the
> extent definition and dropping it would make the rows incomparable.

Missions are small: `ram_size` runs 224 to 6,432 bytes, typically about 1 KB,
and almost every overlay resolves to **four or five functions**. The largest,
overlay 80, is a single 3,668-byte function plus four small helpers.

## The mission API

96 distinct ARM9 addresses are called from overlay code. Ranking them by how
many *different* missions call each separates the interface from the one-offs.

### First, subtract the compiler

The three functions called by **all 91** overlays are not API at all, and this
is worth stating because raw frequency invites exactly that mistake:

- **`func_02121CBC`** is an intrusive linked-list push whose head lives at
  `0x0216A2DC` — **four bytes below the overlay load address**. That is static
  destructor registration, the C++ runtime's `atexit`. Every overlay calls it
  because every overlay has static objects.
- `func_020F2BA0` and `Battles::Vector2s16::Vector2s16` are constructors of the
  same kind.

**Frequency identifies what is common, not what is meaningful.** The filter that
matters is whether a function does something a mission designer would ask for.

### The real interface

| missions | address | what it is |
| ---: | --- | --- |
| 90 | `SetTeamStartingBricks` | starting resources per team |
| 87 | `SetTeamFriendly` | |
| 89 | `ScriptingMacros::Timer::AddNewTimeNode` | a scheduled callback |
| 80 | `OpenMissionDialog` | the briefing/dialogue box |
| 91 | `func_02049F14` | tags an object with a team id (`+0x11`, if unset) and appends it to a list at `team+8` |
| 67 | `SetTeamNeutral` | |
| 80 | `func_0204732C` | **the `DestroyMarkedUnitsObjective` factory** (`docs/32`) |
| 74 | `DoSomethingWithMarker` | (name from the third-party config, `docs/02`) |
| 47 | `SetTeamEnemy` | |
| 91 | `func_02045898` | fires a map event by id — calls `Battles::Map::GetEventWithId` on the global map and walks the event's action array |

`func_02045898` is in every mission and *is* genuine API: it is the bridge
between the `EVNT` chunk `docs/15` found in the map file and the overlay's code.
**Missions are half data and half code** — the map file carries events, triggers
and markers, and the overlay carries the logic that arms and answers them.

Four team-relation calls (`Friendly`, `Neutral`, `Enemy`, `StartingBricks`)
appearing in 47–90 missions each is the diplomacy setup every scenario needs.

## Thirteen scripting-object factories

At `0x02047308`–`0x02047514` sit **thirteen functions with one shape**: allocate
N bytes from the pool allocator with tag `0x45`, then call one specific
constructor.

| factory | bytes | missions using it |
| --- | ---: | ---: |
| `0x0204732C` | 40 | **80** |
| `0x020474EC` | 32 | 51 |
| `0x02047514` | 24 | 47 |
| `0x02047384` | 36 | 28 |
| `0x0204746C` | 36 | 23 |
| `0x02047494` | 36 | 14 |
| `0x020473B0` | 28 | 12 |
| `0x020474C0` | 36 | 11 |
| `0x0204740C` | 44 | 10 |
| `0x02047358` | 36 | 7 |
| `0x02047308` | 32 | 1 |
| `0x020473D8` | 40 | 1 |
| `0x02047444` | 36 | 1 |

(Counts revised in `docs/32`: the original figures came from the control-flow
walk, which under-reads a compiled mission. A linear sweep filtered to known
call targets finds considerably more.)

**All thirteen are used by at least one mission** — none is dead. The
distribution is a long tail: one object type in two thirds of all missions,
three more in a third, and three used exactly once. That is the signature of a
scripting vocabulary that grew as the campaign was written, with a few
general-purpose constructs and a handful built for a single scenario.

The objects are small (24–44 bytes) and are the things a mission is *made of*.
**`docs/32` names all thirteen** — they are the concrete subclasses of
`Battles::ObjectiveBase`, resolved through their constructors' vtable literals,
and the set is closed: 13 classes, 13 factories, no gaps either way.

## What a mission looks like

Overlay 80's script is 3,668 bytes and makes only **36 calls, 8 distinct**:

```
13x  func_02049F14      register an object to a team
10x  func_0204732C      construct the 40-byte scripting object
 4x  SetTeamNeutral
 3x  <a local helper in the overlay>
 3x  func_02047494      construct a 36-byte scripting object
 1x  SetTeamFriendly
 1x  func_02049BA0
 1x  func_02047514      construct a 24-byte scripting object
```

So a mission is: **set up the diplomacy, construct about a dozen scripting
objects, register them to teams.** The rest of those 3,668 bytes is inline
constant data — coordinates, ids, parameters — being loaded into the arguments.
There is no interpreter and no bytecode: **the campaign is compiled C++**, one
translation unit per mission, calling a macro layer that the RTTI names
`Battles::ScriptingMacros`.

That is a significant fact for the port and mod scopes in the founding
directive. Missions cannot be edited as data, because they are not data. A
mod that changes a mission must either patch the overlay's compiled code or
reimplement the macro layer — and the second is now a bounded problem, because
this document enumerates the layer: 96 entry points, 13 object types.

## Pitfall: an overlapping first function

Overlay 80 reports functions at both `0x0216A2E0` (size `0xE5C`) and
`0x0216A2E8` (size `0xE54`) — the same code, entered eight bytes apart, because
a `static_init` seed pointed at the earlier address and the walk flowed into the
later one. The coverage figure is computed by **merging intervals**, so this
costs nothing in the percentage, and the function count is one too high rather
than the byte count being double-counted. `docs/30` records why that
distinction matters: summing sizes instead of merging is what produced a
188.3% coverage figure in `docs/17`.
