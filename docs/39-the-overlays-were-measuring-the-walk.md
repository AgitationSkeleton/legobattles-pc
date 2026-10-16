# The overlays were never 52.9% — the walk was

**Status: 91,540 of 91,540 overlay code bytes, a verified 100%, with zero
non-zero bytes unaccounted. Three of the four code segments are now complete
and checked.**

`docs/31` reported the overlays at 93.9%. `docs/32` corrected that to 52.9% and
called the first figure a mistake. **Both numbers were measuring this project's
own control-flow walk, not the overlays**, and the second was no closer to the
truth than the first — it was wrong in the other direction.

## The source that was there all along

`docs/31` seeded the overlay scan from each overlay's `static_init` table,
walked from there, and measured what it reached. That was the best available
source *if* the only source were the ROM.

It was not. The `ds-decomp` configuration this project has used since `docs/02`
carries **a symbol file per overlay** — `config/eur/arm9/overlays/ov008/symbols.txt`
and ninety more — each listing that overlay's functions with real sizes from
that project's delinking analysis:

```
func_ov008_0216a2e0 kind:function(thumb,size=0x8)   addr:0x0216a2e0
func_ov008_0216a2e8 kind:function(thumb,size=0x2cc) addr:0x0216a2e8
func_ov008_0216a5b4 kind:function(thumb,size=0x10)  addr:0x0216a5b4
func_ov008_0216a5c4 kind:function(thumb,size=0x20)  addr:0x0216a5c4
```

Four functions — the same count the walk found — but the sizes differ. The walk
gave `0x2ac` where the config gives `0x2cc`, `0x24` where the config gives
`0x10`. `docs/32` had already established *why*: **a control-flow walk
under-reads a compiled mission**, because much of it is straight-line setup
entered from branches the walk does not take, and its "extent" is a high-water
mark rather than a measurement.

The config's 364 functions were sitting in the same directory tree the ARM9
corpus is built from. They were never loaded because `build_corpus.py` reads
only the ARM9 static module, and nothing had asked the question the other way.

## The result

An overlay's bytes turn out to be exactly three things:

```
ram total                94,048
inside a function        91,540
static_init tables        1,092
zero fill at the end      1,416
non-zero, no function         0
```

**Nothing else.** Every non-zero byte in all 91 overlays is inside a function
the config names. Code region is `ram - init - zero = 91,540`, coverage is
`91,540`, and the check is not "close to 100%" but *exhausted*: the residual
category is empty.

The zero fill was worth confirming rather than assuming. Every one of the 78
leftover runs is entirely `00` bytes, sitting after the `static_init` table at
the end of an overlay — trailing zero-initialised space, not unreached code.

## Three figures for one thing, and only one of them measured it

| doc | figure | what it actually measured |
| --- | ---: | --- |
| `docs/31` | 93.9% | walk extents ÷ `ram_size` — extents overran into pools, denominator included data |
| `docs/32` | 52.9% | walk *visited* bytes ÷ code — an honest measure of **the walk** |
| here | **100%** | config functions ÷ code |

`docs/32`'s correction was right about the specific fault it fixed — the extent
figure was not a coverage figure — and still landed 47 points from the truth,
because it replaced a bad measurement of the overlays with a good measurement of
the wrong thing.

**The lesson is narrower than "check your numbers", which this project has now
said three times.** It is: *when a measurement depends on a tool you wrote, the
error bar is the tool's, not the artefact's.* The walk's coverage of the
overlays was a fact about the walk. Asking whether a better source existed
should have come before either number was published — and one had been in the
repository the whole time.

## What this does not change

The `docs/31` findings that came from the **linear sweep** stand: the 96-entry
mission API, the 13 objective factories, the per-mission objective matrix. Those
were computed by sweeping the whole overlay range with a known-target filter,
not by the walk, which is precisely why `docs/32` had to introduce the sweep to
fix the objective counts. The analysis was sound; only the coverage arithmetic
was not.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its scanning ceiling; remainder labelled (`docs/38`) |
| ARM9 annotated | 26.8% | |
| data decoded | 6.8% | |
| decompilation | 0.143% | 351 functions byte-verified |

Three of four code segments complete. The ARM7's wram is the only one left, and
`docs/33` and `docs/38` between them argue it is at its ceiling with the
remainder identified as mostly the unused Wi-Fi stack — so the mapping phase of
this project is, for practical purposes, finished.

**That makes the next segment the annotation figure at 26.8%**, and after it the
decompilation figure at 0.143%. Both need analysis rather than tooling, which is
a different kind of work from the last five passes.
