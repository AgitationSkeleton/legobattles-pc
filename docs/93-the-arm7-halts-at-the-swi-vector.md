# The ARM7 halts at the SWI vector

> **RETRACTED IN PART (docs/105).** This page calls `func_020872DC`
> `HotWire_ResolveTarget` and `func_02087350` `EntityAtTile`. Both names are
> wrong. `func_02087350` resolves a two-byte **entity handle** against the pool
> at `0x02155028`; `func_020872DC` adds a **liveness** test. They are the
> engine-wide handle resolver (80 and 264 callers), not hotwire code and not a
> tile lookup. The override the port built on them is removed. Read anything
> below that rests on those names with that correction in mind.


**Status: the boot blocker is characterised precisely. The ARM7 is not slow and
not starved — it stops at exactly 3,426,692 cycles and never resumes, parked at
`PC=0x00000008` with interrupts master-disabled.** Raising the ARM9 budget
thirteen-fold changes nothing about it.

## The measurement that settles it

```
ARM9 budget      ARM7 cycles     ARM7 dispatches
      60,000,000     3,426,692           129,218
     800,000,000     3,426,692           129,218
```

**Bit-identical.** The ARM9 meanwhile went from 13.7M to 48.8M instructions, all
of them spinning in `Boot_WaitLoop`. Coverage stayed at **6 code pages** in both.

That kills the previous hypothesis. `docs/92` observed the ARM7 mid-autoload-copy
at a 5M budget and inferred "slow, not stuck". It *was* slow there — but it is
stuck here, and a bigger budget was the right experiment to tell those apart.

## Where it stops

```
ARM7 PC=0x00000008  CPSR=0x20000093  SP=0x0380FFC0
     LR=0x038037BA  R0=0x000000FA    R12=0x038037B9
     IME=0  IE=0x00040000  IF=0x00000000  IPCSYNC.out=0x0800
     exceptions=1
```

`0x00000008` is the **SWI vector** on the ARM7. `CPSR` decodes as mode `0x13`
(Supervisor) with `I=1`, which is the state the core enters on `SWI`. `LR` and
`R12` are odd (`0x038037BA`, `0x038037B9`) — Thumb addresses in WRAM — so a Thumb
caller issued the software interrupt.

So the ARM7 got through its autoload copy, ran its init, **enabled the IPCSYNC
interrupt (IE bit 18), posted `0x0800` to IPCSYNC**, then executed an `SWI` and
never came back.

**`IME=0` is the detail that matters.** With the interrupt master disabled, no
interrupt can be delivered, so anything waiting for one waits forever. `IF=0`
confirms none is pending.

## What it does not tell us

Which SWI. `R0=0xFA` (250) is a plausible argument but not an opcode, and the
BIOS SWI number lives in the instruction at `LR-2`, which has not been read.
Nothing here identifies it, and the project does not assume it is `IntrWait`
or `Halt` merely because those are the common ones.

**That read is the obvious next step**: `LR = 0x038037BA` is Thumb, so the SWI
instruction is the halfword at `0x038037B8`, and its low byte is the comfort
number. The ARM7 WRAM section is lifted, so the bytes are available.

## Both halves are alive, which is the good news

The handshake is no longer a missing-code problem:

* the ARM9 reaches `Boot_WaitLoop` and polls `REG_IPCSYNC` correctly;
* the ARM7 reaches the point of **posting to `REG_IPCSYNC`** and enabling its
  interrupt.

What fails is the rendezvous, and the ARM7 side of it is disabled at the master
switch. That is a runtime/BIOS-emulation question rather than a lift question,
which is a different class of work from everything before it.

## A related find, from the other end of the project

While the run was going, `Sim::HealSpell`'s tick (`func_02078770`, 600 bytes)
was surveyed for the *Fix Hotwired Healing* option. It calls
**`HotWire_ResolveTarget` (`func_020872DC`) and `EntityAtTile`** — the *same*
tile-to-entity resolution that `docs/84` located the hotwire glitch in.

That is a genuine lead: if healing and hotwiring share a target resolver whose
tile map holds one occupant per tile, then "the healer keeps healing a unit that
changed team" and "hotwire steals the wrong occupant" may be **two symptoms of
one defect**. Not established — the heal tick has not been read line by line —
but it changes where to look.
