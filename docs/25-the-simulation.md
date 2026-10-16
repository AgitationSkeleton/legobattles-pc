# The simulation: how a unit actually behaves

The RTTI names **96 classes under `Battles::Sim::`**, and together they describe
the whole runtime — what an entity is, how it takes an order, how it moves, and
how it builds. This is the layer `docs/12`'s numbers feed into.

## What an entity is

```
Sim::Entity
  ├── Sim::CollectableItem
  ├── Sim::Projectile
  └── Sim::Unit
        ├── Sim::Bridge
        └── Sim::Gate
```

`Unit` covers buildings too — consistent with `docs/11`, where one
`UnitBlueprint` class serves units and structures alike. Bridges and gates get
their own subclasses because they are placeable *and* passable.

Entity field offsets recovered while reading the actions:

| offset | field |
| --- | --- |
| `Entity+0x08` | its blueprint record — every stat read goes through this |
| `Entity+0xA0` | **current health** (`u16`) |
| `Entity+0x22E` | **construction progress**, a percentage compared against 100 |

## Orders are objects, and idling is one of them

`Sim::EntityCommandBase` has **28 subclasses**, and they are not "commands the
player issues" — they are **the states an entity can be in**:

| group | commands |
| --- | --- |
| combat | `AttackEntityCommand`, `CombatMoveEntityCommand`, `CombatHoldPositionEntityCommand`, `CombatStandGroundEntityCommand`, `PatrolEntityCommand` |
| building | `ConstructStructureEntityCommand`, `ConstructMultipleEntityCommand`, `RepairStructureEntityCommand`, `AssistStructureEntityCommand` |
| economy | `HarvestEngineerEntityCommand`, `ProduceUnitEntityCommand`, `ResearchUpgradeEntityCommand` |
| transport | `GarrisonEntityCommand`, `UngarrisonEntityCommand`, `TeleportEntityCommand` |
| **idle** | `IdleEngineerEntityCommand`, `IdleMineEntityCommand`, `IdleProductionStructureEntityCommand`, `IdleNoActionEntityCommand` |
| death | `MarkForDestructionEntityCommand`, `WaitingToDieEntityCommand`, `DieEntityCommand` |
| other | `MoveEngineerEntityCommand`, `RetireEntityCommand`, `StopEntityCommand`, `FreezeEntityCommand`, and three `MoveProjectile…` variants |

**There are four different ways to be idle**, one per role. A Mine idles by
running `IdleMineEntityCommand`, which is how it produces without an order. That
is the design decision that makes buildings and units the same kind of object:
a building is a unit whose idle state does work.

Every command has a **14-slot vtable** — five inherited from
`EntityCommandBase`, nine of its own — and in every one examined, **slot 9 is
the update**: the largest, and the one containing the state machine.

Below commands sit `Sim::ActionBase`'s 12 subclasses — `AttackAction`,
`MoveUnitAction`, `HarvestAction`, `LoadAction`, `UnloadAction`,
`ConstructProgressAction`, `RepairStructureAction`, `DelayAction`,
`BounceAction` and three projectile movers. A command is *what* the unit is
doing; an action is the step it is currently taking.

## The pathfinder is an escalation chain, expressed as inheritance

```
Sim::WaypointPlotterBase
  ├── Sim::EndpointPlotter
  └── Sim::SegmentedVectorPlotter
        └── Sim::WaitForObstaclePlotter
              └── Sim::SidestepAvoidancePlotter
                    └── Sim::AstarPlotter
```

**A* *is a* sidestep-avoidance plotter *is a* wait-for-obstacle plotter.** Each
level adds one more expensive recovery, and the cheapest that works is the one
that runs. On a 67 MHz ARM9 with up to 9,216 map tiles, running A* only when
waiting and sidestepping have both failed is the difference between a playable
frame budget and not.

`Sim::Continent` and `Sim::ContinentManager` sit alongside it: connectivity
regions, which is how the game answers "can this land unit reach that tile at
all" without pathing, and why transports exist as a category.

Projectiles get their own movers — `Sim::SeekerBase` → `ArcSeeker` and
`DirectVectorSeeker` → `AlignToTileSeeker`, matching the `MoveProjectileArc…`
and `MoveProjectileDirectVector…` commands. An arcing catapult boulder and a
direct-fire arrow are different code paths, not one path with a flag.

## Production is a seven-state machine

`ProduceUnitEntityCommand::virtual_09` dispatches on `this+0x18` through a
PC-relative jump table with seven arms. Reading `func_02071EB4`, the transition
into producing:

```arm
ldrb r1, [r0, #0x22e]     ; the structure's construction progress
cmp  r1, #0x64            ; 100
blo  <abort, state = 5>   ; a half-built structure cannot produce
...
ldr  r1, [r5, #0x20]      ; the queued entity
ldr  r1, [r1, #8]         ; its blueprint record
ldrb r1, [r1, #0x5c]      ; its category
cmp  r1, #6               ; Siege
cmpne r1, #5              ; Transport
```

Two behaviours in six instructions. **A structure must be 100% complete before
it produces anything**, and **Transport and Siege units take a different path
out of the factory** — which is what naval and vehicle units need, since they
cannot spawn on the same tiles as infantry.

## Build time is `+0x60`, and it confirms the 30 Hz tick

`docs/12` carried `+0x60` as unnamed for six working passes. Both
`Sim::ConstructProgressAction` and `Sim::RepairStructureAction` read it, and
they read it identically:

```arm
ldr  r0, [r4, #0x1c]      ; the structure
ldr  r0, [r0, #8]         ; its blueprint
ldrh r6, [r0, #0x60]      ; <- the field
cmp  r6, #1
movlo r6, #1              ; clamp to at least 1
bl   func_020092A8        ; the game settings
ldrb r0, [r0, #0x14]
cmp  r0, #0
beq  <skip>
ldrh r0, [r0, #0x60]
lsr  r6, r0, #1           ; HALVED when the flag is set
...
lsl  r3, r6, #0xc         ; used as a 1/4096 fixed-point divisor
```

So `+0x60` is **build time**: clamped to at least one tick, **halved under a
game-rule flag**, and divided into the health target at `+0x62` to give a
per-tick construction rate. (`docs/28` identifies that flag as **rule 101**,
which is *not* the "Fast Building" red brick — the red brick is a separate
rule, `+0x02`, read by the same function and gated on an owner check.) Repair uses the same field with the same clamp,
against `Entity+0xA0` current health versus `+0x62` maximum, with the cost at
`+0x5E` scaling to the damage repaired.

And then the arithmetic check. **180 of 180 units have a build time that is an
exact multiple of 30 — zero exceptions**, spanning 30 to 1260 ticks:

| | `K_Engineer` | `K_Farm` | `K_Swordsman` | `K_Castle` | `K_Gryphon` | `K_Tower3` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ticks | 150 | 360 | 270 | 900 | 1140 | 1260 |
| seconds | 5 | 12 | 9 | 30 | 38 | 42 |

The designers entered whole seconds and the pipeline multiplied by the tick
rate. `docs/20` derived 30 Hz from the DPS identity; this is the same constant
falling out of a completely unrelated field, in a form — 180 exact divisions
with no exceptions — that is very hard to get by coincidence.

## Two more fields, characterised

The same `[entity+8] -> blueprint -> +disp` idiom, searched across the whole
corpus with the idiom *required* rather than matching any load at that
displacement, finds only 38 functions — a much cleaner set than matching the
offset alone, which returned 706 (`docs/11` warns about exactly that).

**`UnitBlueprint+0x71` is an interaction range.**
`Sim::HarvestEngineerEntityCommand` reads it and aborts immediately when it is
smaller than a computed distance:

```arm
ldr  r1, [r6, #0x160]     ; the entity this command is working on
ldr  r1, [r1, #8]         ; its blueprint
ldrb r1, [r1, #0x71]
cmp  r1, r0, lsr #16      ; against a distance
blt  <return>
```

Its values fit: buildings 6–11 with the Castle and Tower III at 11 and a Farm at
6, units 5–8. A radius that scales with how big the thing is.

**`UnitBlueprint+0x70` is accumulated, not compared.** In the combat path
(`func_020638C8`, which also reads `+0x6E`, `+0x6F` and `+0x71`):

```arm
ldrb r1, [r1, #0x70]
add  r0, r0, r1           ; r0 = entity[0x44]
strh r0, [r4, #0x44]      ; write it back
```

Added to a `u16` accumulator on the entity each time round. Its values run
Gryphon 65 > Catapult 55 > Ballista 45 > Tower 40 > King 30 > Swordsman 15 >
Farm 2, so bigger is "more" of something per step. **Not named** — an
accumulator's meaning depends on what its threshold is, and that has not been
found.

## The rest of the runtime, named

`Sim::Simulation`, `Sim::GameRuleManager`, `Sim::Team` and `Sim::TeamPool`,
`Sim::EntityPool`, `Sim::FiringManager`, `Sim::Scan` and `Sim::ScanManager`
(target acquisition), `Sim::AreaOfInfluenceManager`, `Sim::OccupationGrid` with
its `MatchFunc`, and `Sim::PathFinding::QueueInfo`.

Three pools — entity, spell and magic-influence — all derive from
`Sim::IndexAndInstancePool`, so every runtime object in this game is
index-addressed rather than pointer-addressed. That is what a deterministic
lockstep simulation needs (`docs/07` found `SyncCheckCommand` and
`SyncStatusCommand` in the command set), and it is the single most important
property for a port to preserve: **object identity is an index, and it must be
allocated in the same order on every machine.**
