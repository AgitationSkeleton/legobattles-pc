# The ARM9's `.text` is complete

**Status: 1,201,170 of 1,201,170 code bytes. A verified 100%, zero unexplained,
re-derived from the image on every run of `tools/lb_verify.py`.** The last 36
bytes turned out to be two alternate entry points no source listed.

`docs/36` left the ARM9 at 99.997% with 36 bytes in two islands: 24 at
`0x0211EF58`, 12 at `0x0211F928`. Both were unreferenced and neither decoded
sensibly *at first reading*. That reading was wrong.

## They were code all along

Disassembled as ARM, both are the **XOR-swap idiom**:

```arm
0x0211EF58:  eor r1, r1, r3 ; eor r3, r1, r3 ; eor r1, r1, r3   ; swap r1 <-> r3
             eor r0, r0, r2 ; eor r2, r0, r2 ; eor r0, r0, r2   ; swap r0 <-> r2

0x0211F928:  eor r0, r0, r1 ; eor r1, r0, r1 ; eor r0, r0, r1   ; swap r0 <-> r1
```

Six instructions swapping two register pairs, and three swapping one. That is
not a pattern data falls into.

`docs/36` had reported them as "nonsense in both instruction sets" — which was
true of the *Thumb* reading and simply not checked carefully enough in ARM.
**A block that decodes as garbage in one instruction set and as a textbook idiom
in the other has decoded, and the failure was in the reader.**

## What they are: alternate entry points

Two facts settle it, and they point the same way:

- **nothing branches into either from the preceding function**, so they are not
  tails that the walk failed to follow;
- **both fall through into the function recorded at the next address** —
  `func_0211EF70` and `func_0211F934`.

So the callable code physically begins *earlier* than the entry the `ds-decomp`
configuration records. This is the ordinary shape of a library helper with two
entries: one normalises argument order and drops into the common body, the
other is entered directly. The region is `0x0211xxxx`, which `docs/19` placed in
the ARM9's library sixth, and comparison and division helpers are exactly where
this idiom lives.

Neither is reachable by any call in the image, which is why no scan found them
and why the config does not list them — the same reason `docs/33` gave for the
ARM7's unreached remainder.

## Recorded as evidence, not edited in

Generated output is never hand-edited, and `out/eur/corpus.tsv` is
generated. So these went into a new tracked input, `config/extra_functions.tsv`,
merged by `tools/build_corpus.py` as a **third source** alongside the dsd config
and the RTTI walk:

```
address     mode  size  name                 evidence
0x0211EF58  arm   0x18  alt_entry_0211EF70   swaps r1<->r3 and r0<->r2, then falls through into func_0211EF70
0x0211F928  arm   0xC   alt_entry_0211F934   swaps r0<->r1, then falls through into func_0211F934
```

**Every row carries an `evidence` column, and that is the point.** This is the
only input in the project whose contents were established by hand rather than
derived by a tool, which makes it the one place a wrong entry would be
indistinguishable from a fact. A claim without evidence does not belong in it.

The builder reports what it merged and refuses to override the dsd config where
they overlap, so the file can only add.

## The result

```
[PASS] ARM9 .text: gaps are accounted for as data
       2,994 of 2,994 gap bytes are secure area, literal pools, padding
       or referenced data; 0 unexplained
[PASS] ARM9 .text: every code byte covered
       1,201,170 of 1,201,170 code bytes -> 100.000%
```

And the cross-check that has held since `docs/09` still holds after the merge:
**2,454 addresses shared between the dsd config and the RTTI walk, 2,454 agree
on ARM/Thumb, zero disagree.**

## Two figures, deliberately

`lb_progress.py` reports the ARM9 twice and they are both correct:

- **99.751%** of the `.text` *span* — 1,201,170 of 1,204,164 bytes;
- **100.000%** of the `.text` *code region* — the span minus 2,994 bytes of
  interior data.

The difference is the secure area, the literal pools, four bytes of alignment
padding and one referenced data island. Reporting only the first understates;
reporting only the second invites the question of what happened to 2,994 bytes.
`docs/36` was about denominators being guesses — the answer is not a better
guess, it is showing both and naming what separates them.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| ARM7 wram | 71.5% | of code; at its scanning ceiling (`docs/33`) |
| overlays, code | 52.9% | |
| ARM9 annotated | 26.8% | |
| data decoded | 6.8% | |
| decompilation | 0.143% | 351 functions byte-verified |

Two of the four code segments are now complete and checked. The ARM7's wram
moves from 69.3% to **71.5%** once its 1,904 bytes of literal pools leave the
denominator — a much smaller correction than the ARM9's, because that section is
almost entirely code and its last function ends exactly at the section end.

That small correction is itself informative: it says the ARM7's missing 28% is
**not** an accounting artefact the way the ARM9's missing 12% was. It is real
unreached code, and `docs/33` already argued it is linked-in but unused SDK. The
next honest step there is labelling it, not chasing it.

## The pitfall, stated for the third time

Three passes, three coverage figures, three different faults:

| pass | fault |
| --- | --- |
| `docs/35` | denominator included MODULE_PARAMS; numerator counted it as a function |
| `docs/36` | denominator included 174 KB of rodata; corpus included other CPUs' functions |
| `docs/37` | a code block dismissed as data because only one instruction set was checked |

The first two were about *what to divide by*. This one was about **giving up on
36 bytes because they looked like noise**. All three were caught by refusing to
round a figure to "basically done" — and the tell each time was a number that
did not reconcile, not a suspicion that something was wrong.
