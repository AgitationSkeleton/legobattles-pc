# An honest panel

## A fourth badge

The overlay had three states -- `WIRED`, `READ`, `UNREAD` -- and none of them
describes what this investigation found. A control can be fully researched, correctly
implemented, and still do nothing, because the address it must hook is one the
recompiler never dispatches to (`docs/143`).

`BLOCKED` says that:

```
//   BLOCKED    the ROM IS read and the change is understood, but no mechanism
//              this port has can apply it. docs/143 counted which addresses the
//              recompiler dispatches to: gameplay functions have zero, so an
//              override on them never runs. A control in this state is honest
//              about being inert -- it is not a to-do, it is a dependency on
//              recompiler coverage.
```

It renders in violet with a tooltip saying the hook needs wider recompiler
coverage rather than more research, which is the distinction that matters to
anyone reading the panel.

## Attack Boost re-badged

`Attack Boost Spell Affects Ranged Units` was badged **WIRED**. It is not: its
override sits on `0206E408`, which has zero dispatching call sites. It is now
`BLOCKED`, with the reasoning on the panel itself:

```
entity+0x170 is the runtime melee damage and equals blueprint+0x68 exactly.
Ranged units have melee_damage 0 -- their damage is in the projectile -- so
the boost must be applied where projectile damage is computed. That address
has ZERO dispatching call sites, so an override there never runs.
```

Badge census after the change: **6 wired, 6 read, 7 unread, 1 blocked**.

## What is deliberately not re-badged yet

Two more controls are suspect but not proven inert, and guessing at a badge is
the same error as guessing at code:

* **Hotwire targets: specials only** -- its filter now runs inside
  `set_owner_override`, and `SetOwner` has **one** dispatching call site
  (`docs/143`). One is not zero. Whether hotwire reaches `SetOwner` through
  that particular site is unknown, and a probe would settle it.
* **Hotwire Kills Transport Occupants (convert)** -- same override, same single
  site, same open question.

Both stay as they are until a probe shows whether that one site carries the
hotwire path. Badging them `BLOCKED` on suspicion would be as wrong as leaving
Attack Boost `WIRED` was.

## Why this matters

`docs/123` audited the settings by asking whether each getter had a consumer.
That was the wrong question -- it cannot distinguish a consumer that runs from
one that never will. The panel repeated that error to the player's face, telling
them a control changed the game when it could not.

A badge is a claim. The panel now makes only claims this project can support.
