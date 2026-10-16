# Entity::SetOwner, and how an entity says which transport it is in

Evidence for two of the unwired options. Both facts are read from the ROM and
each is confirmed by more than one caller.

## `func_020598C4` is `Entity::SetOwner(Entity*, const u8* owner)`

```c
void Entity_SetOwner(Entity* e, const u8* owner) {
    func_020875D4(owner);
    if (e->owner /* +0xE4 */ == owner[0]) return;      /* already theirs */
    func_02086FE8(*(void**)0x0215540C, packed(e->handle /* +0xDC..+0xDD */));
    ...
    e->owner /* +0xE4 */ = owner[0];                    /* the write */
    func_02086EF4(*(void**)0x0215540C, packed(e->handle));
    ...
}
```

It early-returns when the owner is unchanged, unregisters the entity's handle
from the old team's list, writes the owner byte at `+0xE4`, and registers with
the new one. `HotWireSpell::tick` calls it as
`func_020598C4(target, func_02087390(&self->caster))` (`docs/105`), which is what
identifies it: hotwiring *is* a change of owner.

`+0xE4` is the same owner byte the build sidebar's ownership gate tests
(`docs/104`), so the three readings agree.

## `entity+0x114` is the handle of the transport the entity is inside

Two bytes, the game's usual entity handle.

* `UnloadAction::virtual_02` (`func_02055CDC`) reads `e[0x114]` and `e[0x115]`,
  compares the pair against the null handle from `func_020873C4`, fails the
  action when they match, and otherwise resolves it with `func_0208727C`.
* `func_0205BE04` resolves the same field and, when it resolves, defers to it;
  otherwise it returns `entity + 0x124`, the entity's own position. That is a
  loaded unit reporting its transport's position instead of its own.

So the link runs **occupant -> transport**. No forward transport -> occupant
list has been located, and none is needed: the occupants of a transport are the
entities whose `+0x114` resolves to it.

## What this unblocks, and what it does not

**"Hotwire Kills Transport Occupants" = Hotwires Them** is now fully supported by
evidence: override `Entity::SetOwner`, run the cartridge's own behaviour, and
then apply the same owner to every entity whose `+0x114` resolves to the entity
just taken -- when that entity is a transport. That is one more call to the
function being overridden, on data the game maintains itself.

**= On (destroy the occupants)** is NOT yet supported. It needs a destroy path,
and `DieEntityCommand`, `MarkForDestructionEntityCommand` and
`WaitingToDieEntityCommand` have not been read. Killing an entity by hand -- for
example by clearing its slot -- would leave the team lists and the handle pool
inconsistent, which is exactly the kind of guess this project does not ship:
port code comes from a strict reading. The option stays unwired until that path
is read.

---

# The blocker was the override mechanism, and it is now removed

Implementing "Hotwires Them" ran into something structural rather than
game-specific, worth recording because it gated several options at once.

`nds_register_dispatch` appends banks and lookup returns the **first** match, so
an override registered ahead of the title banks wins. That is how this port
changes behaviour at all. The cost: **an override cannot call the function it
replaced.** Re-dispatching the same address re-enters the override.

That left two options for any feature that wants to add a step to existing
behaviour, and both are bad:

* **Reimplement the displaced function.** For `Entity::SetOwner` that means
  reproducing team-list de-registration and re-registration, handle bookkeeping
  and an early-out, in order to add one line. Reproducing 300 bytes of shipped
  logic by hand to extend it is precisely the guessing this project avoids, and
  `docs/105` is what that costs when the reading is wrong.
* **Poll from the VBlank hook** and react a frame later. That means mutating
  simulation state from outside the simulation, and invoking guest calls from a
  host hook that interrupts guest execution -- unsafe for the CPU context and
  wrong for a lockstep game.

## `lb_dispatch_original`

`runtime_arm.cpp` now exposes:

```c
void (*lb_dispatch_original(int cpu, unsigned int addr))(void);
```

Banks are scanned and the **last** entry carrying the address is returned. The
title banks register after every override, so the last entry is the original.

An override can now be written the honest way: do the extra work, then call the
cartridge's own implementation, with the "off" path being the original
byte-for-byte because it *is* the original. That is what a strict reading
requires and what the hotwire override failed to be.

**Unblocked by this:** Hotwire Kills Transport Occupants, Fix Hotwired Healing
(`HealSpell::tick`, 600 bytes -- far past reimplementing), Attack Boost Affects
Ranged, and the hotwire targeting options once their site is found.

## The constraint that comes with it: recompiled bodies are resumable

`lb_dispatch_original` returns a real function pointer, but a recompiled body is
**not a plain C function that runs to completion**. Every generated body opens
like this:

```c
void bank_afunc_027E0244(void) {
    if (g_cpu.R[15] != 0x027E0244u) {
        switch (g_cpu.R[15]) {           /* resume at a label */
            case 0x027E0248u: goto L_027E0248;
            ...
        }
    }
L_027E0244:
    g_cpu.R[15] = 0x027E0244u;
    if (runtime_should_yield()) return;   /* returns MID-FUNCTION */
```

It returns whenever the scheduler wants to yield and is re-entered later at a
label. So:

> **An override may TAIL-CALL the original. It must not do work afterwards.**

Calling the original and then continuing would run the follow-up code with the
guest function possibly only part-executed, and over a 300-byte function a yield
is entirely likely. That is a silent, timing-dependent corruption -- the worst
kind, and precisely the class of bug `docs/105` was.

### What that means for the transport option

"Hotwires Them" wants: change the transport's owner (the original), then apply
the same owner to each occupant. The second half is N further guest calls, and
they cannot be made after tail-calling the original.

The workable shape is the reverse: do the occupant work **first**, then tail-call
the original last. Each occupant still needs `Entity::SetOwner` for its team-list
bookkeeping, so those are sequenced with the host-to-guest call convention
(`runtime_call_push_return` + `runtime_dispatch` + `runtime_call_should_return`)
one at a time, the same convention the old hotwire override used for a single
call -- not by writing the owner byte directly, which would leave the team lists
inconsistent and is exactly the kind of guess the port avoids.

That is the next piece of work, and it is now a known shape rather than an open
question.
