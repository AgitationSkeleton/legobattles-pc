# The pivot: from survey to port

> **RETRACTED IN PART (docs/105).** This page calls `func_020872DC`
> `HotWire_ResolveTarget` and `func_02087350` `EntityAtTile`. Both names are
> wrong. `func_02087350` resolves a two-byte **entity handle** against the pool
> at `0x02155028`; `func_020872DC` adds a **liveness** test. They are the
> engine-wide handle resolver (80 and 264 callers), not hotwire code and not a
> tile lookup. The override the port built on them is removed. Read anything
> below that rests on those names with that correction in mind.


**Status: the project's target is now a PC port, specified. The loop of
verification passes is stopped. This document records what changed, corrects an
earlier recommendation, and sets the work order.**

## The port's requirements

1. **A PC port**, scope calibrated to the LSW2DS recomp.
2. **Scalable windows.** Adaptive full-screen and widescreen, showing **more of
   the game field** than the DS did, with **mousewheel zoom**.
3. **The main window is the touch screen.** The top screen becomes an **ImGui
   overlay**, toggled with **Esc / F1**.
4. **Online multiplayer** with **master-server query** and **lobby
   broadcasting**.

## A correction, before anything else

An earlier pass recommended **reimplementation** over recompilation, on the
grounds that matching decompilation is blocked and 0.271% is nowhere.

**That recommendation was wrong for this target.** The LSW2DS port, the scope
benchmark, is an **isolated recompilation of the game's own ARM9 code** rather
than a hand-written reimplementation; the model is Ship of Harkinian.

A recomp does not need matching C. It needs a **correct static lift** — every
function's address, extent and instruction-set mode, every call resolved, every
indirect branch accounted for. That is exactly the asset this project has spent
sixty-odd passes building, and it is why the last four passes of call-graph
cross-checking turn out to have been the most port-relevant work in the project
rather than a detour.

The 0.271% figure is **irrelevant to a recomp**. It measures byte-matching C
against mwccarm, which a recomp never attempts.

## What the port actually needs, and where it stands

| requirement | ROM-side dependency | state |
| --- | --- | --- |
| static lift of ARM9 | function map with modes | **9,314 functions, call graph verified** (`docs/80`, `docs/83`) |
| | interior entry points | **13 found and enumerated** (`docs/81`) — a recomp mislinks without these |
| | indirect branch targets | ⚠️ **partial** — several jump tables decoded ad hoc, no systematic sweep |
| | code/data separation | ⚠️ literal pools inside extents cost a retraction (`docs/83`); a lift must not decode them |
| assets at runtime | container + file formats | **99.4% decoded by file** |
| game logic | simulation behaviour | 96 `Sim::` classes named; economy, combat, fog, objectives, AI documented |
| 60 fps host, 30 Hz sim | the tick | **30 Hz lockstep confirmed** (`docs/25`) |
| **online multiplayer** | **the wire protocol** | **solved this pass — see below** |
| **lobby / master server** | DS wireless discovery | **layer enumerated** — `HostLobby`, `ClientLobby`, `JoinWifiGame`, `LobbySettings`, `MultiplayerMenu`, `WiFiScreen`, `WiFiScanForParentsEvent`, `WiFiClientDroppedEvent`, `WiFiErrorEvent` |
| **two windows** | screen ownership | **`WorldViewScreen` + `WorldViewTopScreen`** — the exact pair the requirement splits |
| **widescreen / zoom** | camera model | ❌ **unknown** — no `Camera` class exists; `User::ScrollWidget` is the only candidate |
| ARM7 (sound, touch) | ARM7 coverage | ⚠️ 71.5%, plus a **main-RAM region not modelled at all** (`docs/83`) |

## The wire protocol is a closed set

The single most useful thing for the multiplayer requirement, found this pass:
the game has **two distinct command layers**, and only one of them is networked.

```
Battles::CommandBase              27 subclasses    7,800 B of vtable code
Battles::Sim::EntityCommandBase   30 subclasses   43,712 B of vtable code
```

`CommandBase` subclasses are **player intent** — `MoveEntitiesCommand`,
`AttackCommand`, `ConstructStructureCommand`, `ProduceEntityCommand`,
`ResearchUpgradeCommand`, `CastSpellCommand`, `PatrolCommand`,
`RallyPointCommand`, `GarrisonCommand`, `HarvestResourceCommand`,
`ChangeAllianceCommand`, `TeamCommand`, `RetireCommand`, `StopCommand` and the
rest. In a lockstep RTS **this is the entire wire format**: peers exchange
commands, not state.

`Sim::EntityCommandBase` subclasses are **what a unit is currently doing** —
derived state, recomputed identically on every peer, never transmitted.

Two of the 27 are the giveaway that the game already does what the port needs:

```
Battles::SyncCheckCommand     128 B
Battles::SyncStatusCommand    128 B
```

**The cartridge ships explicit lockstep desync detection.** The port does not
have to invent a sync-verification scheme; it has to reproduce this one.

`docs/data/commands.tsv` carries all 57 rows with vtable addresses, slot counts
and code sizes.

## What each requirement turns into, as ROM work

**Windows and the screen split.** `WorldViewScreen` (touch, the game field) is
the main window; `WorldViewTopScreen` is what ImGui replaces. Both are in the
64-row `docs/data/ui_screens.tsv` map with their layout files. The work is to
read what `WorldViewTopScreen` actually displays, so the ImGui overlay can
present the same information rather than losing it.

**Widescreen and zoom — the real unknown.** There is **no camera class in the
RTTI**. That suggests the view is a scroll offset plus a fixed tile projection,
which would be *good* news: widening the field and adding zoom become host-side
decisions about how many tiles to draw, with the simulation unaffected. **That is
a hypothesis, not a finding.** It has to be established, because if any
simulation behaviour is keyed to what is on screen — spawn culling, entity
activation, AI vision — then widening the view changes the game and breaks
lockstep between a widescreen host and a 4:3 client. `docs/63`'s fog work says
vision is a simulation property fed by mission scripts, which is encouraging, but
it is not proof.

**Master server and lobbies.** `WiFiScanForParentsEvent` is the DS "scan for a
host" — replacing it with a master-server query plus LAN broadcast is precisely
the LSW2DS pattern, and that project already specifies the server (CLI and
GUI builds, lobby lists, kick/ban, verbose log).
The LEGO Battles side needs `HostLobby`/`ClientLobby`/`LobbySettings` read to
know what a lobby *is* to this game — player slots, factions, map, rules.

## Enhancement and host options

A later requirement, and much of it lands on ground already mapped.

### Raising the caps

**Already solved, and the numbers are known.** `docs/68` read the build-limit
system: a nine-class table at `0x02126D94`, a remaining-count reader at
`func_020860A8`, and pending units held at `player[0xC5 + class]`.

```
class 0  Heroes        1        class 3  Towers        7
class 1  Minifigures  20        class 6  Walls        20
class 2  Specials      4        class 7  Bridges       8
                                class 8  Gates         0
```

`docs/58` has the unit cap separately: **`min(20, 4 + 4 x Farms)`**, with the
cap at `0x02085E5C` and the brick balance hard-capped at 500,000
(`player+0x90`).

Raising caps is therefore a **data override, not a code change** — the table is
nine bytes and the farm formula is three constants. A host option exposing
per-class caps is straightforward. Two cautions worth stating now: the pending
count lives at `player[0xC5 + class]`, so the array bound matters if a class
count ever changes; and **caps are simulation state**, so in lockstep every peer
must use identical values — the lobby has to carry them, not the client.

### The hotwire bug, located

`Battles::Sim::HotWireSpell` is 468 bytes over six vtable slots. Its tick is a
three-state machine, and the target resolution is one call:

```c
Entity *HotWire_ResolveTarget(Point *tile) {   /* func_020872DC, 44 bytes */
    Entity *e = EntityAtTile(tile);            /* func_02087350 */
    if (e && e->[0xE0]->[4]->flag_8 == 0)
        return e;
    return 0;
}
```

And `EntityAtTile` resolves through `func_0205C9B4(world, tile)` — **a tile map
that holds one occupant per tile.** There is no disambiguation between stacked
entities.

That is the mechanism of the reported bug. A special vehicle standing over a unit
or building leaves one of the two in the tile map; hotwire resolves whichever is
recorded, applies its type filter, and if the *underneath* entity passes the
filter it gets hotwired. The bug is not in the spell's logic — **it is that the
lookup cannot see two things on one tile.**

The filter flag `entity->[0xE0]->[4]->+8` is read by **35 functions**, so it is a
general entity-type discriminator rather than something hotwire owns.

For the port both behaviours are **single-function overrides**, which
is exactly the shape a recomp host option wants:

* **Bug fix** — `func_020872DC` validates that the entity it resolved is the one
  the player actually targeted, or `func_0205C9B4` learns to disambiguate.
* **"Anything can hotwire" cheat** — `func_020872DC` returns `e` without the
  flag test. **One branch removed.**

Both are simulation-affecting, so both are host options carried in the lobby,
not client toggles.

### The cheat tab

`docs/28` mapped the game-rule/red-brick table and `docs/64` established the
cheats are uniformly x1.5 multipliers. `docs/63` showed **Reveal Map is
implemented as an OR in the visibility query, not by writing the fog map** —
which is why it is lockstep-safe. That is the model every cheat in the ImGui tab
should follow: **cheats that read are safe; cheats that write desync.**

An "unlock everything" tab is mostly the unlock-slot space `docs/13` and
`lb_blueprints.py` already decode (289 records across five unlockable
categories, `blueprint+0x6C`).

## Work order

Ordered by what unblocks the most, not by size:

1. **The camera/view model.** The one hard unknown, and it gates requirements 2
   and 3. Establish whether the visible area affects the simulation.
2. **`CommandBase`'s five virtuals.** Serialise, deserialise, apply — the wire
   format's actual encoding. The protocol is enumerated; its *bytes* are not.
3. **`SyncCheckCommand` / `SyncStatusCommand`.** What the game hashes, how often,
   and what it does on mismatch.
4. **The lobby classes.** What a lobby holds and how a match starts.
5. **Indirect-branch sweep.** Every jump table in the ARM9, systematically — a
   recomp fails at the first unhandled one.
6. **`WorldViewTopScreen`.** What the ImGui overlay must replace.
7. **Host-option surface.** The cap table, the hotwire override and the cheat
   flags are all located; the work is deciding which are lobby-carried
   (simulation-affecting) and which are client-local (presentation).

## What this pivot retires

**Byte-matching decompilation is no longer a project goal.** `docs/62` measured
its ceiling at ~96 further trivial functions with devkitARM, and a recomp does
not need it. The 789 matched stubs stay in the tree as documentation of the
attempt; the `decompilation 0.271%` line leaves the segment table, because
reporting it alongside port progress would misrepresent both.

The verification checks stay. They are what make the function map trustworthy
enough to lift.
