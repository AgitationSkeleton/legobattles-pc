# How complete is this, actually

**Status: measured, reproducible, and deliberately not reduced to one number.**
Run `python tools/lb_progress.py` for the current figures; this document
explains what they mean and what they are not.

## The headline

```
DECOMPILATION      0.271%   3,380 bytes over 789 functions verified against the ROM
  of which hand-written  0.003%   32 bytes over 4 functions with real names and types
  GCC-reachable shapes are now exhausted; the rest needs mwccarm (docs/45)
ARM9 .text code     100%    1,201,170 of 1,201,170 -- VERIFIED (docs/37)
ARM7 wram          71.5%    43,408 of 60,736 code bytes; remainder labelled (docs/38)
ARM7 code mapped   69.5%    43,868 of 63,128 section bytes, 333 functions
  ARM7 static      100%     404 of 404 code bytes, verified (docs/35)
Overlay code        100%    91,540 of 91,540 -- VERIFIED (docs/39)
ARM9 annotated     53.8%    2,501 listed + 2,506 derived of 9,314; 996 are names (docs/40-43, 57, 61)
Data decoded       99.4%    3,891 of 3,915 cartridge files (71.4% by bytes)
```

## Why there is no single percentage

"Percent complete" is the easiest number in a project like this to state
dishonestly, so `lb_progress.py` refuses to produce one. Four different things
are being measured and blending them would hide all four.

**Decompilation is 0.271%, and it is measured, not estimated.** `docs/34` built
the harness: every function in `config/symbols.tsv` is compiled with devkitARM
and byte-compared against the cartridge, and only exact matches count.

The second row matters as much as the first. Of the 789 matched functions, 785
are **generated stubs** with invented names and untyped arguments — 405 of them
functions whose entire body is `bx lr` (`docs/61`). They reproduce the bytes —
the only criterion — but recover none of the understanding the word
"decompiled" implies. Four are hand-written against real struct definitions.
**0.264% is the decompilation figure; 0.003% is the understanding figure.**

What bounds this is not effort, and `docs/62` measures the bound. Of the 1,724
uncovered functions of 24 bytes or fewer: **41.7% call something** (a relocation,
so the bytes are not final), **37.2% materialise a constant from a literal pool**
(GCC uses an immediate instead), **15.5% need a scratch register** (mwccarm
chains through `r0`, GCC reaches for `r3`), and **5.6% keep everything in `r0`**
— which is an upper bound on what remains reachable, not an estimate.

A fourth blocker has no workaround at all: for `x == 0` the ROM uses
`cmp/moveq/movne`, and GCC emits `clz/lsr` at `-mcpu=arm946e-s`, `rsbs/movcc` at
`-march=armv4t`, and `clz/lsr` at `-Os`. Three settings, three idioms, none of
them the original's.

**Mapped is not understood.** 99.997% of the ARM9's `.text` sits inside a known
function boundary with a known ARM/Thumb mode. Almost all of that came from the
third-party `ds-decomp` configuration (`docs/02`), and it says nothing about
what any of it does. It is a prerequisite, not an achievement — and `docs/36`
is the account of how it was published as 87.3% for twenty passes because the
denominator was the whole section rather than the code.

**Annotated is not analysed.** 2,454 of the 2,501 *listed* functions — 98% of
them — came **free** from the RTTI the game shipped with (`docs/07`). They cost
one parsing pass. Counting that as analytical progress would be misleading, so
the tool prints the RTTI share on the same line as the total.

**And annotated is not named.** Of the 4,961 functions carrying anything, 948
have something like a name and **1,512 have only a neighbourhood** — a class
that owns them, or a translation unit they sit in. The tool prints that split
every run. `docs/57`'s tier L alone added 995 attributions and zero names.

**Byte-weighted data coverage is close to meaningless here.** The cartridge is
128 MB, of which a single `.sdat` sound archive is 62.9 MB and 24 `.mods` videos
are 34.6 MB — **80% of the ROM before any gameplay data at all**. Every format
this project actually solved totals about 2 MB. So "1.7% of ROM bytes decoded"
mostly measures whether anybody ran an audio extractor. The file count (6.8%) is
the more honest figure, and even that undersells it: the 268 decoded files are
the ones that define how the game *plays*.

The four tiers the tool uses for data:

| tier | meaning | files | bytes |
| --- | --- | ---: | ---: |
| decoded | a parser here reads and validates it | 3,891 | 86.6 MB |
| partial | container validated, payload not interpreted | 24 | 34.7 MB |
| known | documented Nintendo format, no parser here | 0 | - |
| opaque | no known structure | 0 | - |

`known` is kept strictly out of `decoded`. Knowing that `.ncgr` is a standard
Nintendo tile format is real, but it is not this project's work and it does not
belong in this project's score.

That tier is also where work goes to be forgotten, and so is `opaque`. Both
are **empty as of `docs/55`**. The `.sdat` sat in `known` for the whole project;
the `.sadl` sat in `partial` for eight passes described as "audio body not
decoded", about a file with no body; and four `opaque` formats turned out to
declare their structure in their first sixty-four bytes.

**Three passes running, the unread work was hiding in a tier label rather than
in a hard problem.** Each label was accurate when it was written and nobody
re-read it. Re-read the tier assignments, not just the tier totals.

And when a format resists, the fix is not more statistics over the files. It is
to find the code that reads it: `docs/56` moved `.hps` from partial to decoded
in twenty minutes by reading one `add` instruction in the ARM9's seek loop,
after an hour of size analysis had concluded the body was unwalkable.

## A plausible percentage is not a correct one

`docs/35` took the ARM7 static section from 99.2% to a verified 100%, and found
the 99.2% was wrong in **both directions at once**: the numerator counted 24
bytes of MODULE_PARAMS as a function, and the denominator counted those bytes
plus a literal-pool word as code. The errors partly cancelled, which is exactly
why the figure looked ordinary.

`tools/lb_verify.py` now re-runs fourteen completeness claims against the
cartridge on every invocation. A claim that is not re-checked is a claim that
has stopped being measured.

## An extent is not a coverage measurement

`docs/31` first reported overlay coverage as 93.9% "reached". That number is the
span of the discovered function extents, and an extent is a **high-water mark**:
a single branch near the end of a function raises it to the end while the walk
skips everything between. The share actually visited is **49.1%**.

Both figures are kept, because the ARM9 and ARM7 rows above use the *extent*
definition — "bytes inside a known function boundary" — and dropping it would
make the overlay row incomparable. What was wrong was the word "reached".

Note that the ARM9's extents come from the third-party `ds-decomp`
configuration, which carries real function sizes rather than walk high-water
marks, so its 87.3% is an extent figure of better provenance than the ARM7's or
the overlays'. `docs/32` has the full account.

## Two denominator traps, both caught

**Summing function sizes double-counts.** `docs/17` recorded a coverage metric
that printed **188.3%** on the ARM9 — impossible, therefore caught — and a
quietly wrong 77.8% on the ARM7 for a true 69.5%. `lb_progress.py` merges
intervals and reports the overlap it removed.

**The ARM7's denominator nearly went wrong the same way.** The first run of this
tool reported ARM7 coverage as **27.5%**, because it summed all three ARM7
sections including the 96 KB `mainram` block. `docs/14` established that block
is a *data mirror* the ARM7 copies to `0x027E0000`, not code. Excluding it gives
69.5% — which reproduces `docs/17`'s independently-derived figure exactly, and
that agreement is what confirms the fix rather than the reasoning behind it.

Note the direction: this error **understated** progress. An error that flatters
the project gets scrutinised; one that does not, does not. Both need the same
check.

## What the mapped figure does *not* include

- **The 91 overlays are now scanned** (`docs/31`) — 93.9% of their RAM bytes —
  but their corpus is kept in its own file keyed by overlay id, because an
  address alone does not identify an overlay function.
- The ARM9 denominator is the whole section, which also holds rodata and data.
  So 87.3% is a **floor** on code coverage, not a ceiling — the true share of
  *executable* bytes reached is higher.

## What would move each number

| metric | what raises it |
| --- | --- |
| decompilation | **mwccarm**. GCC can verify two-instruction accessors and little else: the moment a function needs one intermediate register, compiler identity decides the bytes (`docs/34`). Until the original compiler is in the loop, a mismatch is uninformative. |
| ARM9 mapped | **done** — verified 100% of .text code (`docs/37`) |
| overlays | **done** — verified 100% against the config's own function list (`docs/39`) |
| ARM7 mapped | probably nothing — three techniques converge (`docs/33`). Labelling is **done**: 43% of the unreached bytes name their own hardware and the bulk is the Wi-Fi stack (`docs/38`). |
| annotated | reading code. The cheap derivations are spent through `docs/57`; 4,353 functions still carry nothing, and moving the 1,512 associations to actual names needs analysis rather than another rule. |
| data decoded | nothing is opaque; what is left is 24 `.mods` whose Mobiclip bitstream is a proprietary codec. Data has gone as far as reading can take it (`docs/56`) |

## Honesty rule

This document and the tool exist because the project's first rule is that the
ROM is the source of truth and unread values are reported as unknown. A
completion percentage is a value like any other: it gets measured, or it gets
marked unknown. It does not get estimated to sound better.
