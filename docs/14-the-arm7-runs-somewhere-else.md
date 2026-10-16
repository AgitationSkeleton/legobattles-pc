# The ARM7 keeps 488 of its 159,528 bytes where the header says

**Status: found and handled.** This is the trap the LSW2 playbook ranks as worth
a week if it is discovered in month two, and it is closed on day one.

## What the header says, and what is true

The cartridge header puts the ARM7 at `0x02380000`, `0x26F28` bytes,
uncompressed. That is where it is *loaded*. Almost none of it *runs* there.

The boot stub ends like this:

```arm
0x023800CC  ldr r1, =0x0380FFFC     ; the ARM7's IRQ handler vector
0x023800D0  ldr r0, =0x037FB488     ; the handler — in the relocated block
0x023800D4  str r0, [r1]
0x023800D8  ldr r1, =0x037F8000     ; the relocated block's entry
0x023800DC  ldr lr, =0xFFFF0000
0x023800E0  bx  r1                  ; and it never comes back
```

Before that, `func_02380118` walks an autoload table and copies the blocks out.
The table is at `0x023801D0` and describes:

| section | RAM address | size | bss | what it is |
| --- | --- | ---: | ---: | --- |
| static | `0x02380000` | `0x0001E8` (488) | — | the boot stub, and nothing else |
| wram | `0x037F8000` | `0x00F4B0` (62,640) | `0x3E64` | **the ARM7's actual program** |
| mainram | `0x027E0000` | `0x017878` (96,376) | `0x1968` | its data, in main RAM |

The copier consumes the image up to `0x023A6F10`, which is exactly where the
autoload list itself begins — so every byte is accounted for.

**99.7% of the ARM7 runs at an address the header never mentions.** Any tool that
maps the ARM7 at `0x02380000` — a disassembler, a mode classifier, a lifter
config — is reading almost all of it at the wrong address. On LSW2 the
equivalent block was 62 million interpreted instructions and the difference
between 21 fps and 60.

## Why the usual detection fails here

`docs/04` finds the ARM9's sections through the SDK `MODULE_PARAMS` structure,
identified by the magic pair `0xDEC00621` / `0x2106C0DE`. **The ARM7 has no such
magic anywhere in its image.** Its table is a plain structure that the boot stub
reaches through a literal. Scanning for the magic returns nothing and the
natural conclusion — "no autoloads, it all runs at `0x02380000`" — is wrong.

`tools/nds_extract.py` now falls back to a **structural scan**: three consecutive
words where the first two bound a whole number of 12-byte entries inside the
image, the third points into the image, every entry has a plausible RAM
destination and a size that fits, and the running total lands on the table
itself. That last condition is what makes it precise rather than merely
plausible — a real autoload table describes the whole image and stops exactly
where the list starts.

It finds the ARM7's table with no help, and it finds the ARM9's too, so the
magic is now an optimisation rather than a requirement.

## The block spans two memories

`0x037F8000 + 0xF4B0 = 0x038074B0`, and ARM7 WRAM begins at `0x03800000`. So the
62,640-byte block is **32,768 bytes of shared WRAM followed by 29,872 bytes of
the ARM7's own 64 KB WRAM**, which are contiguous in the ARM7's address map. The
game treats it as one region, and the installed IRQ handler at `0x037FB488` sits
in the shared-WRAM half.

That contiguity is also why the image has 291 references to `0x0380FFF4` — the
top of ARM7 WRAM, where the DS keeps its interrupt plumbing.

## The two address spaces overlap, and tools must not merge them

The ARM7's data block lands at `0x027E0000`. That is **also the ARM9's DTCM
address**. There is no conflict on hardware — DTCM is private to the ARM9, and
the ARM7 sees `0x027E0000` as an ordinary mirror of main RAM — but a tool that
maps both cores into one address space will answer ARM9 questions with ARM7
bytes and never say so.

`tools/lb_code.py` therefore maps **one core at a time**:

```
$ python tools/lb_code.py map
  itcm     0x01ff8000 .. 0x01ff8320       800 bytes      6 functions
  static   0x02000000 .. 0x02150880   1378432 bytes   9154 functions
  dtcm     0x027e0000 .. 0x027e0060        96 bytes      0 functions

$ python tools/lb_code.py --cpu arm7 map
  static   0x02380000 .. 0x023801e8       488 bytes      0 functions
  mainram  0x027e0000 .. 0x027f7878     96376 bytes      0 functions
  wram     0x037f8000 .. 0x038074b0     62640 bytes      0 functions
```

The extractor names sections per-CPU for the same reason: `0x027E0000` is
`dtcm` in the ARM9's table and `mainram` in the ARM7's. Calling it `dtcm` in
both would be a comment that lies.

## Consequences

- **The ARM7 has no function corpus at all yet** — 0 of its ~62 KB of code is
  catalogued. The third-party `ds-decomp` config covers only the ARM9. This is
  now the largest single gap in the corpus.
- **The sound system lives here.** On DS, the ARM7 owns audio, touch, SPI, RTC
  and Wi-Fi. Anything about the mixer, the `SDAT` playback path or the wireless
  stack is in `wram`, not in the ARM9.
- **A recompiler needs this as a separate program** with base `0x037F8000`, and
  a second one at `0x027E0000` for the data. Emitting it at `0x02380000` would
  make every branch target wrong.
