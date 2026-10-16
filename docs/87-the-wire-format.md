# The wire format

**Status: `CommandBase`'s five virtuals are read. Slots 3 and 4 are serialise and
deserialise, the byte-level codec is read end to end, and deserialisation is
validated per byte. The command type tag is *not* found and is recorded as open
rather than guessed.** The multiplayer command encoding goes from unknown to
mostly known.

## The vtable

`CommandBase`'s own five slots are stubs — 4, 20, 4, 4 and 8 bytes. **All 27
subclasses override all five.** The sizes are tightly patterned, and one relation
holds in every single row:

```
slot 0   20 / 32 / 40 B      non-deleting destructor
slot 1   28 / 40 / 48 B      deleting destructor   -- always exactly slot 0 + 8
slot 2    4 / 12 / 16 B      a thunk to shared code
slot 3   16 .. 116 B         SERIALISE
slot 4   16 .. 224 B         DESERIALISE
```

`slot 1 == slot 0 + 8` in all 27 is what identifies the destructor pair: the
deleting form is the plain form plus a call to `Heap_Delete`. Read directly from
`TeamCommand`:

```asm
slot 0:  bl func_020043F8 ; mov r0,r4 ; pop        <- ~Command()
slot 1:  bl func_020043F8 ; bl 0x204CA50 ; pop     <- ~Command() then delete
```

`func_020043F8`, `CommandBase`'s destructor body, is a bare `bx lr`.

## Slots 3 and 4 are the codec, and they differ by one thing

`SyncCheckCommand` makes it unambiguous, because it carries two plain fields:

```c
/* slot 3 -- 32 bytes */                 /* slot 4 -- 44 bytes */
Write32(this, this->field_14);           if (!Read32(this, &this->field_14))
Write32(this, this->field_18);               return 0;
                                         return Read32(this, &this->field_18);
```

**Slot 3 passes fields by value; slot 4 passes their addresses and returns a
boolean.** That is the whole distinction, and it is why slot 4 is consistently
the larger of the two — every read carries a failure branch.

**Deserialisation is validated and short-circuits.** A port that reads the four
bytes and trusts them accepts malformed packets the cartridge rejects.

## The byte-level codec, read end to end

```c
Write32(stream, value)          /* func_02004768, 72 B */
    WriteByte(stream, ((u8*)&value)[0]);   /* four unrolled calls */
    WriteByte(stream, ((u8*)&value)[1]);
    WriteByte(stream, ((u8*)&value)[2]);
    WriteByte(stream, ((u8*)&value)[3]);

bool Read32(stream, u8 *out)    /* func_020047B0, 92 B */
    if (!ReadByte(stream, out + 0)) return false;
    if (!ReadByte(stream, out + 1)) return false;
    if (!ReadByte(stream, out + 2)) return false;
    return ReadByte(stream, out + 3);
```

The value is pushed to the stack and addressed as bytes, so on this
little-endian target **the wire order is least-significant byte first**. Both are
fully unrolled — no loop, no length, no alignment, no padding.

### The buffers

```c
void WriteByte(stream, u8 b)    /* func_020045E4, 52 B */
    if (g->primary->count < g->threshold)  PushByte(g->primary,  b);
    else                                   PushByte(g->overflow, b);

bool ReadByte(stream, u8 *out)  /* func_02004618, 56 B */
    if (IsEmpty(g->buffer)) return false;
    *out = PopFront(g->buffer);
    return true;
```

**Two write buffers with a size threshold**, both reached through one global.
Once the primary fills, writes spill to a second buffer. A port that uses a
single growable buffer produces a different split — which matters if the split is
what frames a packet.

**Reads fail on exhaustion**, and that failure is what the `Read32` chain and
then slot 4 propagate. The validation is real, not decorative.

## The primitive family

Eighteen functions at `0x02004588`–`0x02004DBC` call `WriteByte` or `ReadByte`
and nothing else does: **8 writers and 10 readers**. That is the whole codec, and
it is small enough to reproduce exactly.

Most command classes call `WriteByte` and `ReadByte` **directly** rather than
through `Write32`, so **most fields on the wire are single bytes**. Only
`SyncCheckCommand` and `SyncStatusCommand` use `Write32`/`Read32` — fitting, as a
sync hash is a word.

**The widths of the other sixteen primitives are not established.** I classified
them by counting call sites from `lb_scan.walk`, which returns a deduplicated
target list, so a helper calling `ReadByte` four times counts as one. That method
is unsound for this and its output is discarded rather than reported. The correct
approach is to disassemble each helper's walked extent and count encoded call
instructions; that is the next small task.

## Open: the type tag

**How a receiver knows which of the 27 commands it is decoding is not
established.** Three things were checked and none of them is the answer:

* **Constructors do not stamp a tag.** The method that found the job kinds
  (`docs/77` — each job constructor writes a distinct constant to `job+0x24`)
  finds nothing here: of 27 constructors, only two store any small constant, and
  those are field initialisations, not distinct ids.
* **There is no dispatch table of command vtables.** Scanning the whole ARM9
  image for four or more consecutive command vtable pointers finds **zero
  runs**; the 54 references are all isolated.
* **Slot 2 is not the tag.** It is a thunk, and `GarrisonCommand` and
  `ProduceEntityCommand` tail-jump to the *same* shared implementation, so it
  does not distinguish them.

Port code must come from a strict reading, not a guess, so **no tag scheme is
proposed.**

**Stated restart:** find the sender. Something calls slot 3, and whatever writes
a discriminator writes it around that call. The virtual call is hard to find
statically, so the productive angle is the command *queue* — the object commands
are pushed into before transmission — reached from `Sim::` rather than from the
command classes themselves.

## What the port can build from this today

* The destructor pair, both forms.
* `Write32` / `Read32` byte order and unrolling.
* `WriteByte` / `ReadByte` including the two-buffer threshold and the
  fail-on-empty contract.
* Per-field validation and short-circuit in every deserialiser.

**What it cannot build yet:** framing and dispatch. Without the tag there is no
way to route a received buffer to the right command class, so the receive path
stays blocked.
