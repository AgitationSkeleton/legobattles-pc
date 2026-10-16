# The submit confirmed, and the game's own "take then kill" path

> **Corrected by docs/392.** Transport Occupants' "Destroy them" does not use
> this command. The kill the game itself uses on a dying entity's occupants is
> `func_0205E78C(entity, 0)`, an immediate call inside the simulation:
> `Sim::DieEntityCommand::v08` makes it on every handle in the entity's list
> at +0x100 (count +0x104). The same slot of `Sim::WaitingToDieEntityCommand`
> (v08, 0x02075380) is an empty function, so waiting to die is not dying.
> Destroy now makes that one call per occupant through the docs/383 hop. The
> reading of the command below stands; the premise that issuing it is how to
> kill does not.

## func_020EB910 is the submit, read rather than inferred

`docs/137` named it from its position in a call sequence and flagged that as
positional evidence. Its body settles it. It is **Thumb**, which is why an ARM
disassembly of it came back empty:

```asm
020EB910  push {r3,r4,r5,lr}
          adds r5, r0            ; the command
          adds r0, #0x10         ; command->owner_10  (docs/88's header field)
          blx  02087244          ; resolve the owner -> a player index
          bl   020A186C          ; the session/context
          ldr  r1, [pc, ...]     ; a base address
          lsls r0, r4, #4        ; index * 16
          adds r0, r1, r0        ; base + player*16 -> that player's queue
          adds r1, r5
          blx  020F14D4          ; push the command into it
          ldr  r0, [pc, ...]     ; a global
          ldr  r1, [r0] ; adds r1, #1 ; str r1, [r0]   ; ++counter
```

It reads the command's owner, indexes a **per-player array of 16-byte queues**,
pushes the command in, and bumps a global counter -- a lockstep sequence number.
That is a submit, and it has **72 callers**, consistent with every command site
in the game using it.

## The recipe is now complete, with no assumed steps

`docs/137` left one gap: whether the handle vector needs constructing. It does,
and the call site shows it:

```asm
add  r0, sp, #0 ; movs r1, #1 ; blx 020EC5A8    ; init/reserve the vector
...build the 2-byte handle from [entity+0xDC]/[+0xDD]...
add  r0, sp, #0 ; ldr r1, [r2] ; blx 020EC8DC   ; push it (handle BY VALUE in r1)
bl   020A186C ; ldr r0, [r0, #0x64]             ; the sink
add  r1, sp, #0 ; blx 02005B0C                  ; construct
bl   020EB910                                   ; submit
add  r0, sp, #0 ; blx 020EC728                  ; destroy the vector
```

`func_020EC5A8` is the same initialiser `WaitingToDieCommand`'s own constructor
calls on `this+0x14`, which cross-checks it. Six steps, every one read from
code.

## The lead worth following before writing any of it

The function this was read from, `func_02046530(x, entity)`, is not a random
kill site. It:

1. calls `func_0205DF94(entity, 1)`;
2. calls **`Entity::SetOwner(entity, owner)`** at `0204654E`;
3. tests a byte at **`[x + 0x21]`**;
4. **only if that byte is set**, issues the `WaitingToDieCommand` for that same
   entity.

Its caller walks a **list of 2-byte handles** (`data` at `[r4+4]`, `count` at
`[r4+8]`, stride 2), resolves each with `func_0208727C`, and calls this for the
one that matches.

That is *"change these entities' owner, and if a flag says so, kill them too"* --
which is the shape of the hotwire-transport setting itself, in the cartridge's
own code. `[x+0x21]` is very likely the game's own "destroy rather than convert"
switch.

**This is a lead, not a finding.** What `x` is has not been established -- it is
`[sp]` in the caller and could be a command object, a context, or a parameter
block -- and neither the class nor the byte's meaning is confirmed.

## Why it matters

If `[x+0x21]` is that switch, hotwire-transport mode 1 is a **one-byte change to
a value the game already reads**, rather than six chained guest calls with a
hand-built vector on borrowed stack. That is strictly safer and strictly more
faithful, and it is worth one more hop to find out before implementing the
harder version.

`docs/137`'s recipe stays valid either way, and is the fallback.
