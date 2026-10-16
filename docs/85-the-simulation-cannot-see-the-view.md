# The simulation cannot see the view

**Status: work-order item 1 from `docs/84` is answered. The simulation never
calls view code, established from two independent directions. Widescreen, a
wider field of view and mousewheel zoom are host-side decisions and are
lockstep-safe.** This unblocks port requirements 2 and 3.

## Why this had to be answered first

The requirement is a view that shows **more of the game field than the DS did**,
plus zoom. In a lockstep game that is only safe if the simulation is blind to
what is on screen. If spawn culling, entity activation, AI vision or anything
else keys off the visible area, then a widescreen host and a 4:3 client compute
different states and desync — and the failure would appear minutes into a match,
not at connect time.

## Test 1: the virtual call graph

Every function reachable from a vtable slot is attributable to a class, which
gives a namespace. Walking all of them by control flow:

```
User  -> User   137
other -> other  103
User  -> other   91
Sim   -> other   80
Sim   -> Sim     36
Sim   -> User     0      <-- none
```

**Not one `Sim::` virtual calls a `User::` virtual.** The dependency runs one
way: the view drives the simulation and reads from it, never the reverse.

Coverage, stated plainly: 2,454 of 9,314 functions (**26.3%**) are attributable
this way — 555 `Sim::`, 556 `User::`, 1,343 other. Strong, not exhaustive.

## Test 2: all functions, by address region

The ARM9 is laid out with the two subsystems on separate pages:

```
page          Sim virtuals   User virtuals
0x02050000        140              0
0x02060000        230              0
0x02070000        171              0
0x02080000         12             21     <- the mixed boundary
0x02090000          0             40
0x020A0000          0             48
0x020B0000          0            165
0x020C0000          0            156
0x020D0000          0             76
0x020E0000          0             48
```

Taking `0x02050000-0x0207FFFF` as simulation and `0x02090000-0x020EFFFF` as
view covers **3,656 functions — 39.3%**, all functions rather than just
virtuals. That run reported **493 SIM → USER call sites**, apparently
contradicting test 1.

It does not. Resolving the 95 distinct targets:

```
0x020EC000-0x020EF000   65 targets   the container and heap library
0x020A1000-0x020A7000   26 targets   the fog / map / world services (docs/63)
0x020E3000, 0x020EA000   4 targets
```

**Zero of the 95 belong to a `User::` class.** The simulation calls
`List_PushBack`, `Vector_Init`, the heap, `FogMap_Update` and `GetLocalPlayer` —
library and world services that happen to be *linked* above the simulation, not
UI.

Both tests agree once the region is defined correctly: **the simulation does not
call the view.**

## The range error, for the third time

My "view region" was `min` to `max` of the addresses where `User::` virtuals
happen to live. That interval also contains the container library and the fog
services, because the linker put them in the same pages.

`docs/82` did this with the ARM7 and swept a 25 MB span across a gap between two
sections. `docs/83` retracted forty-two findings that came out of a related
mistake. This is the third instance, and the rule earns being stated flatly:

> **A range derived from the extremes of a sample is not a definition of that
> sample.** Classify members, then test membership — never infer membership from
> an interval.

The saving grace both times was checking *what the anomalies actually were*
rather than reporting the count.

## The argument the cartridge itself makes

Independent of both tests: **LEGO Battles shipped working wireless multiplayer,
and two DS players scroll their views independently.** If the visible area fed
the simulation, the retail game would desync whenever two players looked at
different parts of the map — which is every match.

That covers the one thing a call-graph test cannot: shared globals. A global the
view writes and the simulation reads would already have broken the shipped game.

## What this means for the port

**Safe, and host-side:**

* Widening the field of view to fill a widescreen window.
* Mousewheel zoom in and out.
* Scaling the window arbitrarily.

None of these can change simulation state, so a widescreen host and a windowed
client stay in lockstep. **Field of view does not need to be a lobby-carried
option** — unlike caps and rules (`docs/84`), it is genuinely client-local.

**The real constraint is not correctness, it is draw budget.** Showing more of
the map means more entities and more tiles on screen than the DS ever drew at
once. That is a host rendering question — sprite counts, tile streaming, batch
sizes — and it is where the work actually lies. It cannot desync anything; it
can only be slow or run out of resources.

**Still unread:** the view's own structure. `WorldViewScreen` slot 8 is
`func_020DAD5C`, 3,580 bytes — the same function `docs/81` found carrying **four
interior entry points**, one called 35 times. The largest and most-entered
function in the corpus is the world view's update, and a recomp has to get its
entry points right. That is the next piece of view work, but it no longer gates
the widescreen decision.

## Where the port stands

| work-order item (`docs/84`) | state |
| --- | --- |
| 1. camera / view model — does the view feed the sim? | **answered: no** |
| 2. `CommandBase`'s five virtuals — the wire encoding | next |
| 3. `SyncCheckCommand` / `SyncStatusCommand` | pending |
| 4. lobby classes | pending |
| 5. indirect-branch sweep | pending |
| 6. `WorldViewTopScreen` — what ImGui replaces | pending |
| 7. host-option surface | located, not built |
