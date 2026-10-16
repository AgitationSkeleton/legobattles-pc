# A corpus defect the new check found on its first run

**Status: the call-graph check added in `docs/80` found a provable error in the
function corpus within one iteration of existing. One recorded function start is
two bytes too high; the address two bytes below it has two callers and the
recorded one has none.** The correction is filed in the established place;
**117 of 117 checks pass**. Annotation unchanged at 54.4%.

## The defect

```
0x020EB940    2 call sites    NOT a corpus function
0x020EB942    0 call sites    IS a corpus function (10 bytes)
```

The code settles it:

```asm
020EB940  lsls r1, r0, #4        ; index * 16
020EB942  ldr  r0, [pc, #4]      ; the table base
020EB944  ldr  r0, [r0, r1]      ; table[index * 16]
020EB946  bx   lr
020EB948  <literal: the table pointer>
```

**The function is a strided table lookup and it begins at the shift.** Starting
at `0x020EB942` produces a function that reads `[r0, r1]` with `r1` never set —
which is why nothing calls it. The corpus recorded the second instruction as the
entry.

## Filed, not fixed

`config/extra_functions.tsv` already exists for exactly this: entry points
verified from the ROM that the third-party ds-decomp configuration does not
carry. It already held **two** rows of the *same class*, described there as

> alternate entry points ... the callable code physically starts earlier than
> the recorded entry

so this is the **third instance of a known pattern**, and the row went in
alongside them.

**The corpus is not regenerated.** `tools/build_corpus.py` needs the ds-decomp
configuration at `config/eur/`, which is third-party and not in this repository,
so the merge cannot run here. The row is in the right place and will take effect
the next time the corpus is built; until then `out/eur/corpus.tsv` still carries
the wrong start. **Stating that plainly matters more than claiming a fix** —
generated output is never hand-edited, and quietly patching
`corpus.tsv` would have hidden the discrepancy instead of recording it.

The new check verifies the *evidence* rather than the row: every
`extra_functions` address must be something the ROM actually calls. All three
rows pass.

## The thirteen secondary entry points

`docs/80` counted 13 call targets interior to known functions. Enumerated, they
concentrate:

| host function | size | interior entries | busiest |
| --- | ---: | ---: | ---: |
| `func_020DAD5C` | 3580 B | 4 | `0x020DBABE`, **35 calls** |
| `func_020988F6` (`Squad_Tick`) | 3378 B | 3 | `0x02099602`, 11 calls |
| `func_020DC298` | 2716 B | 2 | `0x020DCD24`, 8 calls |
| four others | 328–2180 B | 1 each | |

**Three functions, all between 2.7 and 3.6 KB, hold nine of the thirteen.** The
largest functions in the corpus are the ones the call graph disagrees with —
which is what you would expect if the disagreement is about where one function
ends and the next begins.

`0x0211FFC8` is a different animal: it sits **8 bytes into an ARM function**,
right after a `bxeq lr`, which is the classic shape of a signed/unsigned pair
sharing one body — the same family as `__aeabi_idivmod` from `docs/78`.

## A heuristic of mine that was wrong

To tell "merged functions" from "genuine interior entries" I tested whether each
entry was **preceded by a return**. It reported three. Two of those three were
preceded by `b <addr>` — an unconditional branch, which does end a basic block
but says nothing about function boundaries — and the third by **`bxeq lr`**.

**`bxeq` is a conditional return.** Control falls straight through when the
condition is false, so it does not end anything. My test counted it as an
epilogue and would have had me split a function that is plainly one function.

Predication is the ARM-specific version of a mistake this project keeps making
in other clothes: **reading an instruction's mnemonic without reading its
condition**, exactly as earlier passes read an offset without reading its width
(`docs/76`, `docs/77`). The general form is *the qualifier is part of the
instruction*.

## Tips and pitfalls

* **A check earns its keep by failing on something you did not plant.** This one
  found a real corpus error one iteration after being written, from a direction
  no byte-counting check could reach.
* **`bxeq`, `popeq`, `moveq pc` are conditional.** Any "does this end a function"
  test must look at the condition code, not the mnemonic.
* **When a tool cannot be regenerated, say so.** A correction filed in config
  with the generated artefact left stale is an honest state; a hand-edited
  artefact is not.
* **Count data rows, not lines.** `wc -l` on `extra_functions.tsv` said 17; the
  file has **3** rows and fourteen lines of comment explaining the evidence
  standard.
* **Disagreement clusters where the objects are biggest.** Nine of thirteen
  interior entries are in the three largest functions in the corpus.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170, with one entry-point correction filed |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 54.4% | 2,501 listed + 2,569 derived, 129 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
