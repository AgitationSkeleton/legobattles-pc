# The ARM9↔ARM7 protocol is ten channels

**Status: the transport and the dispatch are solved. The channel *meanings* are
partly identified and the rest is marked unknown.**

`docs/17` left the ARM7's uncovered 30% pointing at one suspect: functions
reached by neither a call nor a pointer, which on a DS means the **IPC command
dispatch** — the ARM9 sends a word, the ARM7 picks a handler. That dispatch is
now found.

## Finding it

Searching the ARM7 for the IPC registers is the whole first step. `IPCFIFOSEND`
and `IPCFIFORECV` appear as literals **zero times**, which is worth knowing —
they are reached as offsets and immediates, not through a pool. `IPCFIFOCNT`
(`0x04000184`) and `IPCSYNC` (`0x04000180`) do appear, and they lead straight to
`func_037FE2C0`, the ARM7's IPC initialiser:

```arm
ldr  r2, =0xC408
ldr  r1, =0x04000184        ; IPCFIFOCNT
strh r2, [r1]               ; enable FIFO, IRQ on receive, flush
bl   func_037FB964          ; disable the IRQ while installing
ldr  r1, =0x037FE4A8        ; <- the receive handler
mov  r0, #0x40000           ; IRQ bit 18 = IPC recv FIFO not empty
bl   func_037FB7BC          ; set handler
bl   func_037FB8F0          ; enable it
```

It also zeroes a **32-entry table at `0x03807708`** first — the channel handler
table.

## The word format

`func_037FE4A8`, the receive handler, drains the FIFO and decodes each word:

```arm
r7 = 0x04100000             ; IPCFIFORECV
loop:
  ...check FIFO not empty...
  r6 = *r7                  ; the word
  r0 = (r6 << 27) >> 27     ; channel = bits 0..4
  if channel == 0: continue
  r3 = table[channel]       ; ldr r3, [r4, r0, lsl #2]
  if r3:
      r2 = (r6 << 26) >> 31 ; flag  = bit 5
      r1 = r6 >> 6          ; payload = bits 6..31
      bx r3                 ; handler(payload, flag)
  else:
      if !(r6 & 0x20): r6 |= 0x20; func_037FE448(r6)   ; deferred
```

| bits | field |
| --- | --- |
| 0–4 | **channel**, 0–31; channel 0 is ignored |
| 5 | a flag, passed to the handler as its second argument |
| 6–31 | **payload**, 26 bits, passed as the first argument |

An unregistered channel does not drop the message: bit 5 is set and it is
re-queued through `func_037FE448`, which is how a message that arrives before
its service is up survives.

## Registration, and the bitmask the ARM9 reads

`func_037FE39C(channel, handler)` is the setter, and it does two things:

```arm
str  r5, [r1, r4, lsl #2]   ; table[channel] = handler
ldr  r2, [r3, #0x38C]       ; r3 = 0x027FFC00
orr  r1, r2, #1 << channel  ; (or clears the bit when handler is NULL)
str  r1, [r3, #0x38C]
```

`0x027FFC00 + 0x38C` = **`0x027FFF8C`**, inside the DS system region — memory
both CPUs can see. So the ARM7 **publishes which channels it has registered** as
a bitmask, and the ARM9 can test it before sending. Anything emulating one side
has to maintain that word or the other side will wait forever.

## The ten channels

Fourteen registration calls, ten distinct channels:

| channel | handler | registered from | what it reaches |
| ---: | --- | --- | --- |
| 4, 6, 8, 9 | `0x03803CE8` | `0x03803874` | one handler shared by four channels |
| 7 | `0x038025CC` | `0x03801ED8` | |
| 11 | `0x038031D4` | `0x03802618`, `0x03802624` | |
| 12 | `0x037FDDC4` | `0x037FDD64`, `0x037FDD7C` | |
| 13 | `0x03806444` and `0x03806474` | `0x03806230` | two handlers, same channel — re-registered |
| 14 | `0x03803544` | `0x038034EC` | `POWCNT2` (`0x04000304`) and the sound registers → **audio** |
| 16 | `0x038064A8` | `0x03806230` | the timer registers `0x04000100/104/106` → **timing** |
| 17 | `0x038067EC` | `0x03806230` | IPC and IRQ registers |

Channels 0–3, 5, 10, 15 and 18–31 are never registered.

**Only three channels are identified**, and only weakly — most handlers touch no
hardware within two call levels because they queue work for the ARM7's main loop
rather than doing it inline.

## The ARM9 side, and what it reveals

`IPCFIFOSEND` (`0x04000188`) appears as a literal **zero times on the ARM9 too**,
for the same reason: `func_02111CD4` loads `IPCFIFOCNT` and writes `[r2, #4]`.
That function is the exact mirror of the ARM7's decode —

```arm
func_02111CD4(r0 = channel, r1 = payload, r2 = flag):
    word = (word & ~0x1F) | (channel & 0x1F)
    word &= ~0x20
    word |= (flag & 1) << 5
    word  = (word & 0x3F) | (payload << 6)
    str word, [r2, #4]          ; r2 = 0x04000184, so this is IPCFIFOSEND
```

— which confirms the word format from both ends independently.

Ten call sites, and comparing the two sides splits the channels into three
groups:

| channels | ARM7 registers | ARM9 sends | reading |
| --- | :---: | :---: | --- |
| 6, 7, 13, 14 | yes | yes | **ARM9 → ARM7 requests** |
| 1, 5, 10 | no | yes | **ARM7 → ARM9 replies** — the ARM9 must register its own handlers for these |
| 4, 8, 9, 11, 12, 16, 17 | yes | not found | requests the ARM9 sends from a site the channel-immediate extraction could not resolve — six call sites pass the channel in a register rather than as an immediate |

That split is itself a finding: **the protocol is bidirectional over one channel
space**, not two separate spaces, and a channel number identifies a service
rather than a direction.

The sending functions are thin wrappers with no strings and no MMIO of their
own, so naming them means walking up the ARM9 call graph. That is the next step
and it is tractable, because the ARM9 has a full function corpus.

## It did not close the coverage gap

The hope in `docs/17` was that this dispatch would explain the ARM7's unreached
code. **It does not.** All fourteen handlers were already in the corpus — they
are reached by ordinary calls from the registering functions, which the scanner
had already walked.

So the ARM7's remaining ~30% is still unaccounted for, and the IPC table is not
the explanation. Recorded as a failed hypothesis rather than quietly dropped;
the remaining candidates are the interrupt vector table and genuinely dead code.

## Why this matters beyond the ARM7

For the port, this **is** the ARM9↔ARM7 contract. A recompilation that runs the
ARM9 natively and models the ARM7 — or vice versa — has to implement exactly
this: a 32-slot table, a five-bit channel, a 26-bit payload, one flag bit, the
re-queue path for unregistered channels, and the registration bitmask at
`0x027FFF8C`. Ten channels is a small enough surface to implement by hand if the
ARM7 ever needs to be replaced rather than recompiled.
