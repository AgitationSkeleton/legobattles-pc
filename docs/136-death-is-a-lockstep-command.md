# Death is a lockstep command, not a function call

> **Corrected by docs/392.** Transport Occupants' "Destroy them" does not use
> this command. The kill the game itself uses on a dying entity's occupants is
> `func_0205E78C(entity, 0)`, an immediate call inside the simulation:
> `Sim::DieEntityCommand::v08` makes it on every handle in the entity's list
> at +0x100 (count +0x104). The same slot of `Sim::WaitingToDieEntityCommand`
> (v08, 0x02075380) is an empty function, so waiting to die is not dying.
> Destroy now makes that one call per occupant through the docs/383 hop. The
> reading of the command below stands; the premise that issuing it is how to
> kill does not.

## Why "destroy the occupants" was refused

`lb_patches.cpp` handles hotwire-transport mode 2 (convert the occupants) and
explicitly refuses mode 1:

> mode 1 -- destroy the occupants -- is deliberately NOT handled. It needs a
> destroy path, and Die/MarkForDestruction/WaitingToDie have not been read;
> clearing a slot by hand would leave the team lists and handle pool
> inconsistent.

`Battles::WaitingToDieCommand` is now read, and the refusal was right for a
**second reason that had not been stated**.

## What it is

`typeinfo 0213B628`, `vtable 0213B63C`, five virtuals. The two substantive ones
are a matched pair:

```asm
virtual_03  02007A14      ; WRITE
    ldr  r1, [r6, #0x1c]  ; a count
    and  r1, r1, #0xff
    bl   020045E4         ; write it
    loop r4 = 0 .. count:
        ldr  r1, [r6, #0x18]
        add  r1, r1, r5   ; r5 += 2 each iteration
        bl   0200488C     ; write one 2-byte entry
```

```asm
virtual_04  02007A64      ; READ, the mirror
    bl   02004618         ; read the count into [sp]
    ...
    bl   020EC7B0         ; size the destination
    loop ...              ; read the entries back
```

A count at `+0x1C`, a list of **2-byte entries** at `+0x18`, written one at a
time. Two-byte entries are entity **handles** -- index and generation, the same
pair `docs/125` found `HotWireSpell` copying from `[entity+0xDC]`/`[+0xDD]`.

**These are serialisers.** `WaitingToDieCommand` is a **command carrying a list
of entities to kill**, not a function that kills one.

## The consequence

Death travels through the **command system**, which is the lockstep channel
(`docs/87`-`89` read its wire format). So:

* killing an entity is a **simulation** action, not a presentation one;
* it must be **issued as a command**, not performed directly, or two clients
  diverge;
* the port's standing rule -- anything simulation-affecting is lobby-carried,
  not client-local -- applies to it directly.

Hand-clearing an entity slot would not merely leave the team lists inconsistent,
as the original comment said. It would **desync multiplayer**, because the other
client would never see the kill. That is the stronger reason, and it makes the
refusal correct rather than merely cautious.

## What implementing mode 1 now requires

Not a destroy function -- there may not be one to call. It requires:

1. the **command type tag** for `WaitingToDieCommand` (`docs/88` read the tag
   scheme; this specific tag has not been resolved);
2. the API that **constructs and submits** a command into the queue;
3. building the handle list from the occupant sweep already implemented for
   mode 2, which walks `entity+0x114` back-references and has the handles in
   hand.

Step 3 is done and reusable. Steps 1 and 2 are the work, and both are ordinary
reads rather than open questions.

## Status

Mode 1 remains unimplemented, now for a **precise** reason rather than a general
one, with a three-step path to implementing it and one of the three already
written.
