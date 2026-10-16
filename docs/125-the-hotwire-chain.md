# The hotwire chain, and a second near-miss avoided

## The chain, end to end

`docs/106` gives the spell id. This traces it to the effect:

```
UnitBlueprint +0x72         five spell-id slots per hero
  id 0x20                   HotWireSpell         (docs/106's dispatch table)
    Battles::Sim::HotWireSpell   typeinfo 0214A8EC, vtable 0214A920
      virtual_03  02078D5C  the tick, a three-state machine on [this+0x54]
      virtual_04  02078CE4  "is [this+0x14] == -1", a one-line predicate
      virtual_05  02078CFC  target assignment
        -> effect id 0x2A spawned via func_02016AE4
      the effect factory table  020166B0, 74 entries
        entry 0x25            HotWireEffect       (docs/124)
```

**The effect factory table is at `020166B0` with 74 entries**, and effect ids
index it directly: `HotWireEffect` is entry `0x25`, and the `0x1B` / `0x1C` ids
`docs/124` saw the effect spawn are entries 27 and 28 of the same table. That
confirms `func_02016BFC` and `func_02016AE4` as spawn-by-id over this table.

`HotWireSpell::virtual_05` stores a target by copying **`[entity+0xDC]` and
`[entity+0xDD]`** into `[this+0x50]` and `[this+0x51]` -- an index and a
generation, matching the entity generation at `+0xDD` already known to this
project.

## The near-miss

`virtual_05` reads as an acceptance test:

```asm
bl   020873C4
mov  r1, r0
add  r0, r6, #0x50        ; this spell's stored target handle
bl   02087404             ; <- looks exactly like "may I target this?"
cmp  r0, #0
beq  skip                 ; rejected?
```

It is not. `func_02087404` is nine instructions:

```asm
bl 02087350 ; bl 02087350 ; cmp r4, r0 ; moveq r0,#1 ; movne r0,#0
```

It resolves **both** handles and compares the pointers. It is
`HandlesEqual(a, b)`, and it has **248 callers**. `func_020873C4`, the call
before it, has **438**.

Overriding it to "return true when Hotwire Anything is on" would have changed
the meaning of handle comparison across the entire simulation -- the same
mistake as `docs/105`, which cost a shipped defect, but roughly three times
larger in blast radius.

The check that caught it is the cheap one `docs/105` should have had: **count
the callers before wiring an override**. A predicate specific to one spell does
not have 248 of them.

For the record, caller counts in this neighbourhood:

| function | callers | what it is |
| --- | --- | --- |
| `020873C4` | 438 | helper, unidentified |
| `02087404` | 248 | `HandlesEqual` |
| `020872DC` | 96 | `ResolveLiveEntity` (`docs/105`) |
| `02087390` | 50 | a resolver used by the tick |
| `02087350` | 10+ | the resolver `HotWireEffect` uses |

Everything in `020873xx`-`020874xx` is generic handle machinery. **Nothing in
that range should ever be overridden for a per-spell behaviour.**

## Status

`lb_setting_hotwire_anything` still has no consumer. The restriction on what
hotwire may target has **not** been found, and two candidates have now been
examined and rejected on evidence rather than assumed:

* `HotWireEffect`'s `cmp r0, #0xB1` -- a presentation branch (`docs/124`);
* `func_02087404` -- generic handle equality, 248 callers.

What remains unexamined: `HotWireSpell::virtual_02` at `02077868`, which is
likely a shared base-class method, and the **UI-side target selection**, which
decides whether a click on a given entity is offered as a valid cast at all.
The latter is the more likely home for a per-spell restriction, since the
restriction is something the player sees before committing the cast.
