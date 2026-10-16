# Locating the record buffer, and two eliminated candidates

Hunting the checksum algorithm. It is not found yet; what follows is the ground
that was gained and the two leads that were spent, so neither is walked again.

## The record buffer is at 02168500

Found without any disassembly, by searching main RAM for two things at once: the
**known checksum value** `59 F5 24 43` from the "Qwe" save, and the **name**
`Qwe\0`. Exactly one place has both, four bytes apart:

```
checksum 59F52443 at: 02168500
name 'Qwe'        at: 02168504   (and seven unrelated places)
-> record buffer at 02168500, checksum then name at +4
```

The layout matches the file exactly, so `02168500` is the live record. That makes
every future watch on this structure precise instead of speculative, which is
worth more than the checksum hunt itself.

## Eliminated: 02110884 is memcpy

Watching **writes** to the checksum word catches `pc=02110884` storing
`4324F559` -- the exact value in the save file. It looks conclusive and is not:

```asm
02110870  bics r3, r2, #3
02110880  ldr  r3, [r0], #4
02110884  str  r3, [r1], #4      <<<
02110888  cmp  r1, ip
0211088C  blo  02110880
```

Post-indexed load and store with a word count and halfword fixups above it --
this is **memcpy**. It carries the checksum into the buffer from somewhere else;
it does not compute it. The other writes seen at that address, `820018F8` and
`D6474F9D`, are the same routine copying other records.

## Eliminated: 0380299C is the save-chip verify

Watching **reads** of the payload catches `pc=0380299C` reading 239 bytes, one
per payload byte plus one -- an irresistible match for a checksum loop over 238
bytes. It is not that either:

```asm
03802990  ldr  r2, [pc, #0x3c]   ; a hardware register address
03802994  ldr  r1, [r0, #4]      ; the running source pointer
03802998  ldrh r2, [r2]          ; read a byte from the chip
0380299C  ldrb r1, [r1]          ; read the payload byte      <<<
038029A4  cmp  r2, r1            ; compare them
038029A8  beq  ...               ; equal, continue
038029AC  mov  r1, #0 ; str r1, [r0, #0xc]    ; else clear a flag
038029C8  add  r1, r1, #1 ; str r1, [r0, #4]  ; advance
```

It compares each payload byte against a value read from a **hardware register**
and records a mismatch. That is the EEPROM **read-back verification**, running
from WRAM as save code usually does. It touches every payload byte for a reason
that has nothing to do with checksums.

## What that leaves

Both the obvious write and the obvious read are accounted for, so the checksum
is computed somewhere that touches the payload **through neither** -- most
likely over a different copy of the record, before the memcpy into `02168500`.

The next probe is therefore not on this buffer at all: find the **source** of
that memcpy. `02110884`'s `r0` at the moment of the copy names it, and a
watchpoint on the checksum word of *that* buffer would catch the computation
directly.

## Method note

Both eliminated candidates looked conclusive: the right value written by one,
the right byte count read by the other. Neither survived reading the code around
it. A PC that touches the data you care about is a starting point, not an
answer -- which is the same lesson as `docs/125`'s 248-caller near-miss, at a
smaller scale.
