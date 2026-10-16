# The save record is a CardPage, and its checksum is four bytes by construction

`docs/151` showed the record's leading word behaves as a checksum. This confirms
it from **code** rather than from the diff, and narrows what it can be.

## The record's class

The container builder `func_0204B0A4` is **Thumb** -- an ARM disassembly of it
returns one nonsense instruction, which is the third time in this project that a
function looked missing for that reason and is worth remembering.

It allocates each record and constructs it through `func_0204BBE0`, whose second
vtable store resolves to `vtable_Battles__CardPage`, with the mangled name
`N7Battles8CardPageE` sitting inline in the table. The save record is a
**`Battles::CardPage`**.

## Four bytes, by construction

```asm
func_0204BC14:  movs r0, #4 ; bx lr        ; a constant: the header size

func_0204BBE0(this, r1, size, r3):
    str  size, [this, #0x10]               ; the record size
    bl   0204BC14                          ; -> 4
    subs r0, size, r0
    str  r0, [this, #0x14]                 ; size - 4, the PAYLOAD size
```

So the leading four bytes are **not part of the payload by construction**: the
object stores the full size at `+0x10` and `size - 4` separately at `+0x14`, and
a sibling accessor `func_0204BC18` returns that same difference.

For a 242-byte record the payload is **238 bytes**, which is exactly offsets
`0x04 .. 0xF1` -- the range `docs/151` tested. That range was a guess there; it
is now read from the code.

## What the algorithm is not

**Not table-driven.** Scanning the whole ARM9 static image for a 256-entry
table whose first entry is zero and whose second is any standard CRC constant
-- `0x77073096`, `0x04C11DB7`, `0x1DB71064`, `0x82F63B78` -- finds **nothing**,
and the same search for CRC-16 tables (`0x1021`, `0x8005`, `0xA001`, `0x8408`)
finds nothing either.

Combined with `docs/151`'s negative on CRC-32, its complement, Adler-32, and
byte, `u16` and `u32` sums, the checksum is a **custom computed loop**.

## Where to look next, precisely

The two `CardPage` vtable slots are a destructor and a state machine, not the
checksum, so it is computed elsewhere -- most likely on the flush path, where
the page is written into the 512-byte image.

The narrow target: a function that **writes four bytes to the start of a
CardPage buffer**. `Battles::CardPage` is now a searchable handle for that, and
`docs/151`'s four known `(record, checksum)` pairs will confirm any candidate
implementation immediately.
