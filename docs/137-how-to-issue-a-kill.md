# How to issue a kill

> **Corrected by docs/392.** Transport Occupants' "Destroy them" does not use
> this command. The kill the game itself uses on a dying entity's occupants is
> `func_0205E78C(entity, 0)`, an immediate call inside the simulation:
> `Sim::DieEntityCommand::v08` makes it on every handle in the entity's list
> at +0x100 (count +0x104). The same slot of `Sim::WaitingToDieEntityCommand`
> (v08, 0x02075380) is an empty function, so waiting to die is not dying.
> Destroy now makes that one call per occupant through the docs/383 hop. The
> reading of the command below stands; the premise that issuing it is how to
> kill does not.

`docs/136` established that death travels as a `WaitingToDieCommand` and listed
three things needed to issue one. All three are now in hand, and two of them
came from a single real call site rather than from inference.

## 1. The type tag: 33

`docs/88`'s table already listed `WaitingToDieCommand` as **33**, which this work
should have checked before calling it unresolved. It is confirmed independently
from the ROM: **both** of the command's constructors pass `mov r1, #0x21` to
`CommandBase`'s constructor at `func_02004350`, and `0x21` is 33.

## 2. The object and its constructors

The command is **0x2C bytes**. Two constructors and two factories:

| factory | constructor | shape |
| --- | --- | --- |
| `func_02005298(ctx)` | `func_020079E0` | reserves one entry (`func_020EC5A8(this+0x14, 1)`) |
| `func_02005B0C(ctx, vec)` | `func_020079A8` | copies a vector (`func_020EC6B4(this+0x14, vec)`) |

`this+0x14` is the container; `+0x18` is its data and `+0x1C` its count, which is
exactly what the serialiser in `docs/136` reads.

## 3. The submit path, from a live call site

`0x02046586` issues one. The whole sequence, in order:

```asm
; build a two-byte handle from the entity
ldrb r1, [entity + 0xDC]   ; index
strb r1, [scratch]
ldrb r1, [entity + 0xDD]   ; generation
strb r1, [scratch + 1]

add  r0, sp, #0
blx  020EC8DC              ; push that handle into a local vector
bl   020A186C              ; fetch the session/context object
ldr  r0, [r0, #0x64]       ; ctx->[0x64] -- the argument docs/88 named
add  r1, sp, #0            ; the vector of handles
blx  02005B0C              ; construct the WaitingToDieCommand
bl   020EB910              ; <-- SUBMIT it
add  r0, sp, #0
blx  020EC728              ; destroy the local vector
```

So the recipe is:

1. `func_020A186C()` -> context; read `[+0x64]` for the command sink;
2. build a vector of 2-byte handles with `func_020EC8DC` (push);
3. `func_02005B0C(sink, vector)` -> a constructed command;
4. **`func_020EB910(command)`** -- the submit;
5. `func_020EC728(vector)` to release the local vector.

`func_020EB910` is the piece `docs/136` was missing. It is named here from its
position in the sequence -- immediately after construction, taking the command --
which is strong but is positional evidence, not a read of its body.

## What is now unblocked

Hotwire-transport mode 1, *"destroy the occupants"*. The occupant sweep already
implemented for mode 2 walks `entity+0x114` back-references and has each
occupant's handle bytes at `+0xDC`/`+0xDD` in hand -- the same bytes step 1
above needs. Mode 1 becomes: collect those handles into a vector instead of
calling `SetOwner` per occupant, then run steps 3 to 5.

Because it goes through the command system, it is **lockstep-correct by
construction**: the kill is a command both clients will replay, which is the
property `docs/136` showed hand-clearing would have violated.

## Not done

Building a guest-side vector from an override means giving the guest scratch
space and calling three guest functions in sequence. That is mechanical but it
is not written, and `func_020EB910` should be read before it is relied on rather
than trusted from position alone.

## Addendum — `func_020EB910` is now read, not inferred

The section above named the submit "from its position in the sequence -- which
is strong but is positional evidence, not a read of its body", and said it
"should be read before it is relied on". It has been:

```asm
func_020EB910(cmd):            ; thumb, 0x30 bytes
  r0 = cmd + 0x10
  blx func_02087244            ; -> r4
  bl  func_020A186C            ; the session/context object
  r1 = 0x021585CC              ; a table base
  r0 = r1 + (r4 << 4)          ; element r4 of a 16-byte-stride array
  r1 = cmd
  blx func_020F14D4            ; append cmd to that element
  r0 = 0x021585AC
  [r0] = [r0] + 1              ; bump a global command counter
```

and the two callees settle what the shape means:

* `func_02087244(p)` is three instructions -- `ldrh r0,[r0]; and r0,#0xF` -- so
  r4 is a **0..15 slot index** taken from the halfword at `cmd+0x10`. That is
  the player the command belongs to.
* `func_020F14D4(list, item)` branches on `[list+8]`: non-zero appends via
  `func_020F14F4`, zero initialises via `func_020F14AC`. An intrusive list
  insert, empty case separated.

So `func_020EB910` **appends the command to that player's command list and
increments the global count**. It is the submit, and the positional reading was
right. `docs/136`'s caveat is discharged.

**Still not done, and still the honest status of hotwire-transport mode 1:** the
recipe is verified end to end but not written. The panel says the mode is
unhandled, which remains accurate -- unlike the hotwire *targets* combo, whose
third entry claimed nothing and did nothing (docs/236). The difference matters:
an option that says it is not implemented is incomplete work; an option that
says nothing and does nothing is a defect.
