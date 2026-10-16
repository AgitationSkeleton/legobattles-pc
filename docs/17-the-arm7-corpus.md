# An ARM7 function corpus, from nothing

`docs/14` closed the ARM7's *location* problem — 99.7% of it runs at addresses
the cartridge header never mentions — and left the larger one open: **the ARM7
had no function list at all**. The third-party `ds-decomp` configuration covers
only the ARM9. Nothing named a single ARM7 function, and that is the core the
sound system, touch, SPI and Wi-Fi live on.

`tools/lb_scan.py` now produces one by walking the code.

```
python tools\lb_scan.py --cpu arm7 --auto --expand --scan-data --tsv out\eur\arm7_corpus.tsv
```

```
functions found: 334
   by discovery tier: tier 1 = 138, tier 2 = 135, tier 3 = 61
   static     arm        4
   wram       arm      322
   wram       thumb      8
   mode conflicts (a function called both ways): 0
   undecodable instructions hit: 0
bytes inside a function: 43,892 of 63,128 executable (69.5%), over 225 runs
```

**Read the tier breakdown, not the total.** The measured precision of each tier
is below, and it is very different per tier.

## Mode is derived, never guessed

The instruction set of each function is **exactly determined by the call that
reached it**: in ARM, `BL` stays ARM and `BLX` switches to Thumb; in Thumb, `BL`
stays Thumb and `BLX` switches to ARM. The scanner propagates that and reports
nothing it cannot reach.

This is deliberate. The LSW2 playbook measured a prologue-matching heuristic at
**four errors in 5,860** — every one a Thumb function lifted as ARM, which is not
subtly wrong but entirely wrong. A tool that guesses mode buys coverage and
sells correctness, and you cannot see the price until something checks it.

The check here is that **334 functions produced zero mode conflicts**: no
function was ever reached by two calls disagreeing about its instruction set.
Had the propagation been wrong anywhere, the call graph would almost certainly
have contradicted itself somewhere. It did not.

**The ARM7 is overwhelmingly ARM code — 322 of 330 in WRAM.** That is unusual
for a DS ARM7, where Thumb is normally preferred for density, and it fits a core
whose job is audio mixing at a fixed deadline.

## Three tiers, kept apart

Each tier widens the rule, so each is reported separately rather than being
folded into one number.

| tier | rule | found | trust |
| --- | --- | ---: | --- |
| 1 | reached by a direct `BL`/`BLX` from a seed | 138 | certain: a real call, and the mode follows from it |
| 2 | a code pointer in a literal pool, fed back as a seed | +135 | very likely a function pointer — a vtable slot, callback or jump-table entry — and its low bit carries the mode |
| 3 | any word anywhere pointing into code **and** opening with a plausible prologue | +61 | the widest, and the only one using a byte pattern |

Tier 2 alone took coverage from 24.0% to about 59% over three expansion rounds.
Tier 3 examined 56 candidate words, of which **21 opened with a plausible
prologue** and were walked; the other 35 were dropped rather than guessed at.

## The tiers were measured, and tier 2 is only ~80% right

The ARM7 has nothing to check against — which is exactly why the same scanner
was run over the **ARM9**, where an independent 9,312-function corpus exists.

| run | functions found | already known to `ds-decomp` | precision | mode disagreements |
| --- | ---: | ---: | ---: | ---: |
| tier 1 only | 505 | 503 | **99.6%** | 0 |
| tier 1 + tier 2 | 2,386 | 1,998 | **83.7%** | 0 |

So tier 2 contributed 1,881 addresses of which about 386 are **not functions**.
Inspecting them settles what they are: `0x0200913A` disassembles to
`add sp, #0x88 ; pop {r3-r7, pc}` — a function *epilogue*. `0x02000BA0` is the
`BuildInfo` data block. Only 22 of the 388 unknown addresses even decode to a
plausible opening instruction. They are branch targets and switch-table entries
stored in literal pools, not entry points.

This is the LSW2 playbook's warning reproduced from scratch on a different game:

> Widening a heuristic buys coverage and sells accuracy, and you cannot see the
> price until you have something to check against.

**The honest reading of the ARM7 corpus** is therefore: 138 functions that are
about as certain as the ARM9's, and 196 more that are right roughly four times
in five. The TSV records the discovery tier per function so a consumer can take
tier 1 only. A recompiler should.

What survives unqualified is the **mode**: 1,998 shared ARM9 addresses, **zero
disagreements with `ds-decomp`**, and zero mode conflicts internally on either
core. Mode propagation through `BL`/`BLX` is exact, and that was the part most
worth being sure of.

The seeds are the three addresses `docs/14` established: the header entry
`0x02380000`, the block the boot stub branches to at `0x037F8000`, and the IRQ
handler it installs at `0x037FB488`.

## What the missing 30.5% is

222 gaps totalling 19,232 bytes, and the size histogram says most of them are
small: **147 of 222 are under 16 bytes**. Those are literal pools — the scanner
stops at a function's return, and the pool sits just past it.

The larger gaps are a different thing, and they are real code. Every one begins
with a few pool words and then an ARM prologue:

```
0x03802970  a2 01 00 04   ; 0x040001A2 - an MMIO register
            58 10 9f e5   ; ldr r1, [pc, #0x58]
0x0380539c  9c b0 80 03   ; 0x0380B09C - a RAM address
            10 40 2d e9   ; push {r4, lr}
```

So there are functions reached by **neither a call from the seeds nor any
pointer in the image**. The obvious candidates are the ARM7's IPC command
dispatch — the ARM9 sends a command id and the ARM7 selects a handler — and
interrupt vectors. Finding that dispatch table is the next step here, and it
should close most of the remainder in one go.

Recorded as unresolved rather than papered over: **69.5% of bytes, of which only
the tier-1 share is trustworthy — not "the ARM7 is done."**

### A coverage bug the ARM9 run exposed

The first version of this tool computed coverage by summing function sizes. On
the ARM9 that printed **188.3%**, which is impossible and therefore obvious. The
same bug on the ARM7 had quietly printed **77.8%** for a true 69.5%, and nothing
about that number looked wrong.

The cause: the walk follows forward branches, so two functions sharing a tail —
or one whose extent overruns into the next — both claim the same bytes. Coverage
is now computed from merged intervals and the overlap is reported explicitly
(5,232 bytes on the ARM7).

**A metric that cannot exceed 100% will tell you when it is broken. One that can
will not.** Running the instrument on a case where the answer is known is what
made the difference, and it is the same reason the ARM9 comparison above
exists.

## Why this was worth building rather than running Ghidra

Ghidra would find these too. This took one tool and a few seconds to run, it is
reproducible from the ROM with no project database, and — the part that matters
— **its rule is written down and its widenings are separable**. A Ghidra
auto-analysis pass produces a function list whose provenance is a black box; this
produces one where every entry can be attributed to tier 1, 2 or 3 and dropped
if that tier turns out to be unsound.

That is the same property `docs/09` valued in the ARM9 corpus: two derivations
that can be checked against each other. When Ghidra is eventually run over the
ARM7, this corpus is what its output gets checked against.
