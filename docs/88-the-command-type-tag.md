# The command type tag

**Status: `docs/87`'s open question is answered. The tag is the first argument to
`CommandBase`'s constructor, stored at `command+0x0C`, and 26 of the 27 classes
carry distinct ids. The command stream's receive path is unblocked.**

## How it was found, after three dead ends

`docs/87` recorded three things the tag is *not*: constructors do not stamp a
distinct constant into the object directly, there is no dispatch table of command
vtables, and slot 2 is a shared thunk. It named the restart as *find the sender*.

That restart worked, through four hops:

1. **The command factory family.** 32 `Heap_New` factories live in
   `0x02003000`–`0x02008000`; **27 resolve to `CommandBase` subclasses**, in one
   contiguous source-order run at `0x020052F0`–`0x02005B0C` with retained
   `__LINE__` values from 108 to 492. The fourth such family in this project
   after objectives (`docs/71`) and jobs (`docs/75`).
2. **Nothing references them.** Scanning the whole image for the factories'
   addresses as data words finds **zero** — no table, so no table-index id.
3. **68 call sites**, all non-virtual, spread across the UI, each passing
   `ctx->[0x64]` as the factory's first argument.
4. **The constructors all call one base.** Every command constructor begins with
   `bl func_02004350`, and `CastSpellCommand` passes it `r1 = 0x10`.

## `CommandBase`'s constructor lays out the header

```c
void CommandBase::CommandBase(Command *this, u32 type, void *ctx) {
    this->vtable_00 = <CommandBase vtable>;
    this->field_04  = 0;
    this->field_08  = 2;
    this->type_0C   = type;          /* <-- the tag */
    this->owner_10  = *(u16 *)func_020871F8(ctx);
}
```

Read from the ROM:

```asm
stm  r4, {r0, r3}      ; +0x00 vtable, +0x04 = 0
mov  r0, #2
str  r0, [r4, #8]      ; +0x08 = 2
str  r1, [r4, #0xc]    ; +0x0C = the type id
bl   func_020871F8     ; resolve the owner from the context argument
ldrh r1, [r0]
strh r1, [r4, #0x10]   ; +0x10 = owner, u16
```

## The ids

| id | command | | id | command |
| ---: | --- | --- | ---: | --- |
| 0 | `ChangeAllianceCommand` | | 21 | `GarrisonCommand` |
| 1 | `CreateCollectableItemCommand` | | 22 | `HarvestResourceCommand` |
| 3 | `CreateUnitFactionCommand` | | 23 | `MoveEntitiesCommand` |
| 4 | `TeleportUnitCommand` | | 24 | `PatrolCommand` |
| **5** | **`SyncCheckCommand`** | | 25 | `ProduceEntityCommand` |
| **6** | **`SyncStatusCommand`** | | 26 | `CancelProduceQueueItemCommand` |
| 14 | `AssistStructureCommand` | | 27 | `RallyPointCommand` |
| 15 | `AttackCommand` | | 28 | `RepairStructureCommand` |
| 16 | `CastSpellCommand` | | 29 | `ResearchUpgradeCommand` |
| 17 | `CombatHoldPositionCommand` | | 30 | `RetireCommand` |
| 18 | `CombatStandGroundCommand` | | 31 | `StopCommand` |
| 19 | `ConstructMultipleCommand` | | 32 | `UngarrisonCommand` |
| 20 | `ConstructStructureCommand` | | 33 | `WaitingToDieCommand` |

**26 classes, 26 distinct ids, no duplicates.** The gaps at 2 and 7–13 are ids
this hierarchy does not use; nothing was found occupying them and none is
invented here.

**`TeamCommand` is the exception and is not resolved.** Its constructor passes
`r1 = r2` — the id arrives as a *parameter* rather than a literal, so the value
depends on the caller. That is recorded as open rather than guessed.

## What this unblocks, and what it does not

The receive path now has everything it needs in principle: read the tag, pick the
class, run its slot 4 (`docs/87`). Combined with the codec — little-endian
byte-at-a-time, per-field validation, fail-on-empty — a port can decode a command
stream.

**What is still unread** is where the tag is *written to the wire*. `command+0x0C`
is the in-memory field; no command's slot 3 was observed writing it, which makes
sense — a framing layer would write the tag once, ahead of the body, rather than
each command serialising its own. Finding that layer is the next step and it is
also where packet framing lives.

So: **the tag exists and is enumerated, but the byte offset it occupies in a
packet is not established.** A port can build the class-id mapping now and must
wait for the framing before it can parse a real capture.

## Tips and pitfalls

* **Three dead ends and a written restart beat guessing.** `docs/87` refused to
  invent a scheme and named where to look; that note is what made this pass
  short. Fourth time this habit has paid.
* **A base constructor's arguments are a schema.** The whole header — vtable,
  type, owner — is laid out in one 60-byte function that every command runs.
* **Source-order factory families keep appearing.** Objectives, jobs, and now
  commands. When a run of small `Heap_New` wrappers shows monotonic `__LINE__`
  values, the family is complete and its order is meaningful.
* **An id space with gaps is still evidence.** 2 and 7–13 being unused is a fact
  about the enumeration, not a hole in the reading.
