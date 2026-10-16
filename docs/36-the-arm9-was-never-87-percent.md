# The ARM9 was never 87% mapped — it is 99.997%

**Status: the ARM9's code coverage is corrected from a published 87.3% to a
verified 99.997%, and `tools/lb_verify.py` now re-derives it on every run. Two
of the three code segments are effectively complete; the overlays are not, and
that figure moved the other way.**

Every completion report since `docs/30` has carried `ARM9 code mapped 87.3%`.
That number was measured against the wrong denominator, in exactly the way
`docs/35` had just found for the ARM7 — only an order of magnitude larger.

## The denominator was the section, not the code

The ARM9's static section is **1,378,432 bytes**. Its `.text` is not.

Sorting the corpus into address order and merging shows the last function
inside the section proper ends at **`0x02125FC4`**. Everything from there to the
section end is rodata and data — **174,268 bytes, 12.6% of the section** — and
`docs/27` already proved as much without noticing the implication: the red brick
table it found sits at `0x02125FF4`, forty-eight bytes past the end of code.

Measured against `.text` instead of the section:

```
ARM9 .text  0x02000000..0x02125FC4   1,204,164 bytes
covered                              1,201,134   ->  99.75%
```

## The corpus also contained functions that are not the ARM9's

A second error, found by the same sort. The `ds-decomp` configuration carries
**158 functions outside the ARM9 static section**:

- **6 in ITCM**, below `0x02000000`;
- **152 in overlay space**, at `0x0216A2E0` and above — the same addresses
  `docs/31` scans per-overlay, which are meaningless without saying *which*
  overlay.

Those inflate the function count and, worse, stretch the "span" used in any
naive coverage calculation from 1.2 MB to 1.5 MB. They are now excluded, which
is why the function count in the completion report drops from 9,312 to 9,154.

## The remaining 3,030 bytes, accounted for

99.75% leaves 3,030 bytes in 26 gaps. Classifying them:

| bytes | what |
| ---: | --- |
| 1,974 | the **secure area**, `0x02000000..0x02000800` |
| 896 | a data structure at `0x02122060` |
| 100 | data at `0x02000BA0`, referenced by `Entry` |
| 20 | literal-pool targets |
| 4 | alignment padding, two 2-byte gaps |
| **36** | **unexplained** |

**The secure area is the big one and it is not missing code.** The DS ARM9's
first 2 KB is a fixed block holding the entry stub and the BIOS SVC thunks, and
the `ds-decomp` config records each thunk with a nominal `size=4`:

```
0x02000064 thumb size=0x4  next=0x020000C0  gap= 88   RLUnCompReadNormalWrite8bit
0x020000C0 thumb size=0x4  next=0x02000114  gap= 80   Halt
0x02000114 thumb size=0x4  next=0x02000190  gap=120   CpuFastSet
```

Eighteen thunks, each nominally four bytes, each 80–140 bytes apart. The gaps
are the block's own content, not unreached functions.

The 896-byte island at `0x02122060` decodes to nonsense in both ARM and Thumb,
and **eleven words in the image point into it — including two from inside
itself**. A self-referential pointer structure is data.

That leaves **36 bytes** across two islands, `0x0211EF58..0x0211EF70` (24) and
`0x0211F928..0x0211F934` (12). Nothing references either, and neither decodes
sensibly. They are recorded as unexplained rather than assumed.

> **Resolved by `docs/37`.** They are code: the ARM XOR-swap idiom, and
> **alternate entry points** that fall through into the functions recorded at
> the next address. "Neither decodes sensibly" above was true of the Thumb
> reading and wrong about ARM. With them recorded in
> `config/extra_functions.tsv`, the ARM9's `.text` is a verified **100%**.

```
[PASS] ARM9 .text: gaps are accounted for as data
       2,994 of 3,030 gap bytes are secure area, literal pools, padding
       or referenced data; 36 unexplained
[PASS] ARM9 .text: every code byte covered
       1,201,134 of 1,201,170 code bytes -> 99.997%
```

## The overlays moved the other way

Applying the same discipline to the overlays — the segment this pass set out to
work on — makes them **worse**, and that is the honest result.

Each overlay's `ram_size` includes its `static_init` pointer table and its
literal pools. Measuring those across all 91:

```
94,048 RAM bytes
   6,664 data (static_init + literal pools)   7.1%
  87,384 code                                92.9%
  46,194 visited by the walk    ->  52.9% of code
```

And the previously published extent figure, 88,294 bytes, **exceeds the 87,384
bytes of code**. An extent total larger than the code it is meant to cover is
not a coverage figure at all; it is proof the extents overrun into the literal
pools that follow each function. `docs/32` already retracted the word "reached"
for this number — this is why.

So the overlays sit at **52.9% of code**, not 93.9%, and they are the furthest
of the three code segments from complete rather than the nearest.

## The pattern, stated once

Three segments, three whole-section denominators, three wrong numbers:

| segment | published | correct | error |
| --- | ---: | ---: | --- |
| ARM7 static | 99.2% | **100%** | section included MODULE_PARAMS and a pool |
| ARM9 `.text` | 87.3% | **99.997%** | section included 174 KB of rodata |
| overlays | 93.9% | **52.9%** | extent overran; `ram_size` included data |

Two overstated, one understated by twelve points. **The direction is not
predictable, which is the argument for checking rather than trusting.** A
denominator that is "the whole thing" is a guess about where code ends, and this
project made that guess three times without noticing it was a guess.

The rule now applied everywhere: **a coverage figure names its code region and
justifies the boundary**, and `lb_verify.py` re-derives the boundary from the
image each run rather than caching a constant.

## Segments by completeness

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` | **99.997%** | 36 bytes unexplained |
| ARM7 wram | 69.5% | at its scanning ceiling (`docs/33`) |
| overlays, code | 52.9% | corrected this pass |
| ARM9 annotated | 26.8% | |
| data decoded | 6.8% | |
| decompilation | 0.143% | 351 functions byte-verified |

Next by the ordering is the ARM7's wram at 69.5% — but `docs/33` showed three
independent techniques converge there, so the useful work is labelling the dead
SDK regions rather than trying to reach them. After that, the overlays.
