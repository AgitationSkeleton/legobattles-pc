# The ARM9 ships its C++ RTTI

**This is the single most valuable thing found in the cartridge so far.**

LEGO Battles' ARM9 was built with run-time type information left on, in Itanium
C++ ABI form. The binary therefore carries, for essentially every polymorphic
type in the game:

- the fully qualified class name, mangled — `N7Battles14UnitBlueprintE`;
- the base class or classes, as typeinfo pointers;
- a vtable per class, whose slots are that class's virtual functions in
  declaration order, **with the ARM/Thumb bit already set correctly**.

None of that needs an analysis pass. It is read straight out of the image.

## What it yields

Run `python tools\lb_rtti.py out\eur\arm9\section_static_02000000.bin --code-end 0x02125fc4`:

| measure | value |
| --- | ---: |
| mangled type names | 517 |
| typeinfos parsed | 517 |
| … single inheritance (`__si_class_type_info`) | 446 |
| … no base (`__class_type_info`) | 70 |
| … multiple/virtual (`__vmi_class_type_info`) | 1 |
| classes with a vtable | 515 |
| vtable slots | 3,811 (3,790 with a target, 21 null pure-virtuals) |
| … Thumb | 2,171 (57.0%) |
| distinct virtual function addresses | **2,466** |

2,466 named-by-class function addresses against the third-party project's 45
hand-recovered names, and against 9,158 total ARM9 functions. **27% of the
function corpus arrives attributed, with its instruction set, for free.**

Base pointers now resolve completely. The one that used to fail was
`__cxxabiv1::__class_type_info`'s own base, `std::type_info`, whose name is
stored as `St9type_info` rather than in nested `N…E` form; the scanner now
accepts that shape. The same pass widened the identifier charset to include
`$`, which recovered four internal-linkage
`Battles::Sim::OccupationGrid::…MatchFunc$NNNNN_OccupationGrid_cpp` classes.
Those five are the 512 → 517 difference.

The slot walk also used to stop at the first null. This build encodes a pure
virtual as a literal `0`, not as a pointer to `__cxa_pure_virtual` (that symbol
is nowhere in the image), and the nulls appear *mid*-vtable —
`Battles::Sim::EntityCommandBase` has four consecutive nulls at slots 8–11 and
real slots at 12 and 13. Stopping at the first one truncated the interfaces of
eight abstract bases, exactly the ones with the most subclasses:
`Battles::Effect` read as 2 slots instead of 11 (30 subclasses) and
`Battles::Sim::EntityCommandBase` as 5 instead of 14 (27 subclasses). That is
the 3,767 → 3,811 difference.

See `docs/data/rtti_summary.md` for the full class/vtable map, the validation,
and the remaining gaps.

## How the three ABI shapes are told apart

An Itanium typeinfo is one of:

```
__class_type_info      { vptr, name }
__si_class_type_info   { vptr, name, base }
__vmi_class_type_info  { vptr, name, u32 flags, u32 base_count,
                         { base, offset_flags } * base_count }
```

The three ABI vtable addresses are not known in advance, so `lb_rtti.py`
**measures** them: every pointer to a recovered name string is a typeinfo's
`name` field, so the word before it is a vptr, and the histogram of those vptrs
has exactly three entries. In this ROM:

| vptr | shape | users |
| --- | --- | ---: |
| `0x021507C0` | `__vmi_class_type_info` | 1 |
| `0x021507D0` | `__si_class_type_info` | 442 |
| `0x021507E0` | `__class_type_info` | 69 |

The single multiple-inheritance class in the whole game is `Battles::Texture`,
deriving from `Battles::SListItem`.

## The shape of the engine, in its own words

The largest hierarchies name the game's architecture directly:

| base | direct subclasses | what it is |
| --- | ---: | --- |
| `Battles::CampaignMissionBase` | **90** | one per campaign mission — see `docs/08` |
| `Battles::User::ScreenType` | 63 | every front-end and in-game screen |
| `Battles::Effect` | 30 | visual effects |
| `Battles::CommandBase` | 27 | player commands |
| `Battles::Sim::EntityCommandBase` | 27 | the simulation's own command set |
| `Battles::SListItem` | 25 | intrusive singly-linked list membership |
| `Battles::EventBase` | 22 | |
| `Battles::DListItem` | 21 | intrusive doubly-linked list membership |
| `Battles::ObjectiveBase` | 13 | mission objective types |
| `Battles::SimEventBase` | 12 | |

Some names worth having early:

- **Data:** `EntityBlueprint`, `UnitBlueprint`, `UnitFactionBlueprint`,
  `UpgradeBlueprint`, `ProjectileBlueprint`, `CollectableItemBlueprint`,
  `FactionBlueprint`, `CustomFactionBlueprint`. The `BP/` directory's `.ebp`,
  `.fbp`, `.abp` and `.tbp` files are these, serialised.
- **Commands:** `AttackCommand`, `MoveEntitiesCommand`, `HarvestResourceCommand`,
  `ConstructStructureCommand`, `ProduceEntityCommand`, `ResearchUpgradeCommand`,
  `CastSpellCommand`, `GarrisonCommand`, `PatrolCommand`, `RallyPointCommand`,
  `RepairStructureCommand`, `RetireCommand`, `ChangeAllianceCommand`,
  `CombatHoldPositionCommand`, `CombatStandGroundCommand`,
  `CancelProduceQueueItemCommand`, `CreateUnitFactionCommand`,
  `CreateCollectableItemCommand`, `AssistStructureCommand`,
  `ConstructMultipleCommand`, `StopCommand`, `TeamCommand`.
- **Lockstep networking:** `SyncCheckCommand` and `SyncStatusCommand` are
  commands like any other, which says the multiplayer model is deterministic
  simulation with a periodic sync hash — the usual RTS design, and the thing a
  PC netplay layer has to preserve.
- **Screens relevant to the online scope:** `WiFiScreen`, `JoinWifiGame`,
  `HostLobby`, `ClientLobby`, `LobbySettings`, `OpponentSetup`.
- **Containers:** `BitArray`, `BitArray2D`, `DList`, `SList`, `PtrArray`,
  `Array{U8,U32,CommandRef,EntityRef,TeamRef,SpellRef,MagicInfluenceRef,Rect2u8,Vector2u8}`,
  `Rect2u8`, `Rect2u16`, `Vector2{f,s8,s16,u8,u16}`, `Vector3u8`, `LERP`,
  `Random`, `RealTimeTimer`.

`Battles::Random` being a named class matters for the netplay scope: a
deterministic lockstep simulation needs its PRNG identified, and it is.

## Why this changes the plan

`docs/03-tooling-decision.md` describes seeding Ghidra from the third-party dsd
config. That still holds, but the RTTI is a better seed *and* a check on it:

1. **2,466 function entry points with modes**, independent of any prior work.
   Cross-checking these against the dsd config's 9,158 will validate both.
2. **Class-level structure for the decompilation.** A vtable slot index is a
   virtual function's position in its class; recovering the class layout is
   ordinary work once the hierarchy is known.
3. **A naming scheme that costs nothing.** `Battles::AttackCommand::virtual_03`
   is a worse name than the real one and a far better name than
   `func_020a3964`.

`tools/lb_rtti.py --symbols` writes the flat list.
