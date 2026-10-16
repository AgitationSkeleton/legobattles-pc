# The save container

**Status: the save file's outer format is read and confirmed against a real
save. The record *contents* are not, so the unlock cheats remain
unimplemented — this is the layer above them.**

## The file

An `eeprom-tiny` chip, 512 bytes (`port/game.toml`). A save written by the port:

```
000  41 46 45 5F 42 41 54 54 4C 45 5F 53 41 56 45 53  |AFE_BATTLE_SAVES|
010  ...record 0 ............................. 242 bytes
102  ...record 1 ............................. 242 bytes
1F4  FF FF FF FF FF FF FF FF FF FF FF FF              padding
```

`16 + 242 + 242 = 500`, and the last 12 bytes are erased `0xFF`. Record 1 is a
byte-for-byte copy of record 0 in a fresh save.

## The magic is "AFE_BATTLE_SAVES", all sixteen bytes

`func_0204B1D4` builds it, and the leading `A` is **part of the magic**, not a
version or slot byte:

```asm
movs r1, #0x3f ; strb r1, [r2]        ; scrub
movs r0, #0x41 ; strb r0, [r2]        ; 'A'
strb r1, [r2, #2] ; movs r0, #0x45 ; strb r0, [r2, #2]   ; 'E'
strb r1, [r2, #1] ; movs r0, #0x46 ; strb r0, [r2, #1]   ; 'F'
adds r1, r2, #3
ldr  r0, [pc, #8]   ; 0x021486BC = "_BATTLE_SAVES"
movs r2, #0xd       ; 13 bytes
bx   r3             ; memcpy
```

Three bytes written out of order and each preceded by a `0x3F` scrub, then a
13-byte copy. `docs/97`'s string sweep found only `_BATTLE_SAVES` because the
`AFE` prefix is built in code and never appears in the image.

## The container is built by `func_0204B0A4`

```
r6 = 0x10                       ; the running offset STARTS at the header size
n  = func_0204F5B8()            ; how many save slots
for i in 0..n:
    obj = Heap_New(0x24, ...)   ; a 0x24-byte handle per slot
    ... func_0204BC14 / func_0204BBE0 build it ...
    r6 += func_0204BC28(obj)    ; each record contributes its own size
func_02009A3C(..., count = 0xE, ..., 0xB8, ...)
```

So the format is **header, then one variable-length record per slot**, with the
size of each coming from `func_0204BC28` rather than being a constant. The 242
bytes observed are what that returns for this game, not a literal in the file.

`0x02154468` is the container global: `+0x08` and `+0x0C` hold the two allocated
tables.

## What this does and does not unblock

**Does:** a port can now find the records, and knows the header is a 16-byte
magic rather than a struct with fields in it.

**Does not:** anything inside a record. The observed bytes are sparse — a fresh
save has non-zero values only at `+0x00`, `+0x0F`, `+0x15`, `+0xA9`, `+0xB5`,
`+0xCD` and `+0xD9` of the record — and nothing yet says which of those carry
the unlock flags that `docs/97`'s 25-entry achievement table feeds.

**The obvious next step is a diff, and it needs a played game.** Two saves, one
with something unlocked and one without, would identify the bits in minutes.
Everything the autoplay driver can reach stops at name entry, so this one is
gated on a human playing rather than on more reading.

`func_0204BBE0`, `func_0204BC14` and `func_0204BC28` are the three functions
that would answer it from the code side instead, and none has been read.

## The record is composed, and one part of it is a bitfield

Three more functions read, which changes what the 242 bytes are.

**There are exactly two save slots.** `func_0204F5B8` is eight bytes:

```asm
mov r0, #2
bx  lr
```

So the two records in the file are two *slots*, not a record and its backup as
the byte-identical copy in a fresh save suggested.

**The handle is 0x24 bytes and carries its own size.** `func_0204BBE0`:

```
+0x00  vtable (0x021486E4)
+0x04  0
+0x08  arg3
+0x0C  arg1
+0x10  arg2          <- the record size, which func_0204BC28 just returns
+0x14  arg2 - 4      <- minus func_0204BC14()'s constant 4
+0x18  0  (byte)
+0x20  0
```

So 242 is not a constant anywhere; it is whatever is passed in as `arg2`.

**`func_0204F578` computes it, and it is a sum of parts:**

```asm
r6 = [...+0x14]                 ; a count
r5 = func_020EB7B8()
r4 = func_02045914()
r0 = func_020A80CC()
r1 = (r6 + 0xE3) >> 3           ; <- a BITFIELD, rounded up to bytes
r1 = r1 + 0xB
r1 = r1 + r4
r0 = r1 + r0
r0 = r5 + r0
```

```
record_size = func_020EB7B8()
            + ((count + 0xE3) >> 3)     bitfield bytes
            + 0x0B
            + func_02045914()
            + func_020A80CC()
```

`(n + 0xE3) >> 3` is the give-away: a bit count converted to bytes. `0xE3` is
227, so the field holds **`count` variable bits plus 220 fixed ones** — and a
bitfield of a couple of hundred flags in a save file is exactly the shape unlock
and achievement state takes. `docs/97`'s registry has 25 named entries, so 220
is comfortably more than that alone.

**Which is still not enough to write the cheat.** The formula says a bitfield
exists and roughly how big it is; it does not say where in the record it starts,
nor which bit is which. `func_020EB7B8`, `func_02045914` and `func_020A80CC`
each contribute an unknown number of bytes ahead of it, and none has been read.

The diff of two saves remains the cheap answer, and it still needs a played
game. Reading those three functions is the expensive one, and it is now a
bounded job rather than an open-ended one: three sizes and a starting offset.

## Correction: the record is runtime-composed, not statically laid out

The previous section called finishing this "a bounded job: three sizes and a
starting offset". **That was too optimistic and is withdrawn.** Reading the three
functions shows two of them are not sizes at all.

```
func_020EB7B8()  ->  movs r0, #0x49 ; bx lr        73, constant
func_020A80CC()  ->  movs r0, #0x73 ; bx lr       115, constant
func_0204B050()  ->  movs r0, #0    ; bx lr         0, constant
```

so far so good, giving a fixed `73 + 0x0B + 115 = 199`. But:

```
func_02045914() = func_02045F4C() + func_020401FC() + func_0204B050()
```

and both of the first two are **loops over a registered list**:

```asm
; func_02045F4C
ldr r0, [r7, #0x28]        ; how many are registered
ldr r0, [r7, #0x24]        ; the array
ldr r0, [r0, r5]           ; entry i
ldr r1, [r0] ; ldr r1, [r1, #0x10] ; blx r1     ; entry->vtable[4]() -- its size
adds r4, r4, r0            ; accumulate
```

`func_020401FC` is the same shape over a second list at `+0x2C` / `+0x30`.

So a save record is **a sequence of blocks, one per registered subsystem, each
declaring its own size through a virtual call**. The total is whatever they add
up to, which is why 242 appears nowhere as a constant.

**This changes the conclusion.** The bitfield's offset is not a static fact
waiting to be read out of a few more functions: it depends on which subsystems
register and in what order, which is runtime state. Chasing it statically means
enumerating every registrant and its size virtual, and then establishing the
registration order — considerably more than "three sizes".

The arithmetic still narrows things usefully. With the observed 242:

```
242 = 199 + bitfield_bytes + func_02045914()
   so bitfield_bytes + func_02045914() = 43
```

and if the variable bit count is zero, `(0 + 0xE3) >> 3` is 28 bytes, leaving 15
for the registered subsystems. That is consistent but **not confirmed** — the
count at `[...+0x14]` has not been read, so 28 and 15 are one solution rather
than the solution.

**The two-save diff is now clearly the right route, not merely the cheaper one.**
A dynamic layout is exactly the case where reading bytes that actually changed
beats deriving where they should be.
