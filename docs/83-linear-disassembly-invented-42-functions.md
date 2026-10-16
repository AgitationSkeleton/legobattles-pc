# Linear disassembly invented 42 functions

**Status: `docs/82`'s central ARM7 finding is retracted. All 42 "missing
functions" were artefacts of reading a function's recorded extent linearly, which
decodes the literal pools inside it as instructions. Re-run against the
scanner's own control-flow call list, the ARM7 has **zero** unmapped in-region
targets. The same re-test **strengthens** `docs/80` and `docs/81`.** **120 of 120
checks pass.**

## The retraction

`docs/82` reported 42 ARM7 call targets that were not corpus function starts and
not interior to any known function, and concluded there were 42 functions the
corpus did not have. Both numbers came from disassembling each function's
recorded extent from start to end and collecting every `bl`.

`tools/lb_scan.py` returns the calls it reaches **by following control flow**.
Comparing the two directly:

```
unmapped ARM7 targets by CONTROL FLOW ......  0
unmapped ARM7 targets by LINEAR reading .... 42
in linear but not control flow ............. 42
in control flow but not linear .............  0
```

**Every one of the 42 is an artefact.** And the extents were not the problem —
`0 of 333` ARM7 functions have a recorded size shorter than their walked extent.
The sizes are right; the reading was wrong.

## Why linear reading fails here

ARM code puts **literal pools between basic blocks**, not only at the end of a
function. A pool word is four bytes of data sitting inside the function's
extent, and roughly one in sixteen random words decodes as a `bl` with a
plausible in-range target. Walking the extent instruction by instruction, a
disassembler cannot tell it from code — it has no reason to skip it.

Control flow can. It never reaches the pool, because nothing branches there.

This is the same false-positive class `docs/70` found sweeping the overlays and
`docs/80` *suspected* for four ARM9 targets. The difference is that this time it
was the whole finding rather than a footnote.

## The ARM9 survives, and improves

The worry was immediate: `docs/80` and `docs/81` used the same linear method. Re-run
across all 9,314 ARM9 functions by control flow:

| | linear (`docs/80`) | control flow |
| --- | ---: | ---: |
| unmapped targets | 18 | **14** |
| interior to a known function | 13 | **13** |
| not covered | 5 | **1** |

**The thirteen interior entry points stand unchanged**, and `0x020EB940` — the
corpus defect `docs/81` filed — is still there with two call sites and still
uncovered. The four targets `docs/80` guessed were "literal-pool words decoded as
`bl`" are **exactly the four that vanish**. The guess was right and is now
proven.

So the two ARM9 documents needed no correction, and the one ARM7 document needed
a retraction. That asymmetry is worth noticing: `docs/80` hedged its four
unexplained targets and `docs/82` did not hedge its forty-two.

## What does survive in `docs/82`

**The calls leaving both ARM7 sections are real.** Forty of the forty-five
survive control flow, targeting main RAM around `0x027E0000`:

```
0x027E0000 page   25 sites
0x027F0000 page   15 sites
```

The ARM7 executes code in a region the corpus does not model, and that finding
is unaffected. `docs/82` is corrected in place rather than rewritten — the wrong
paragraph is struck through and left visible.

## Two measurements worth having

**503 of 9,314 ARM9 functions have a walked extent longer than their recorded
size.** `lb_scan.py` prints exactly this caution about itself — *"a walk that
follows forward branches can overrun into the next function"* — so the over-walk
is the tool's, not the corpus's. Both numbers are useful and neither is a defect.

**A fresh `--auto` ARM7 scan reaches 138 functions and 23.2% of executable
bytes**, against the committed corpus's 333 and the table's 71.5%. The committed
corpus was therefore built with a richer invocation — `--expand`, feeding
literal-pool pointers back as seeds. That provenance was not written down
anywhere; it is now.

## Tips and pitfalls

* **Never read a function's extent linearly to find its calls.** ARM interleaves
  literal pools with code. Use the control-flow walk, which cannot see them.
* **The tool printed the caveat and I did not read it.** `lb_scan.py` says in its
  own output that walks can overrun; three passes went by before that mattered.
* **A hedge is a debt you may not have to pay.** `docs/80` marked four targets as
  probably artefacts and was vindicated; `docs/82` asserted forty-two and had to
  retract.
* **Re-test the old result with the new method, immediately.** The moment the
  ARM7 collapsed, the ARM9 was suspect for the same reason — checking took one
  command and converted a worry into a confirmation.
* **Record how a generated artefact was generated.** Nothing said the ARM7
  corpus needed `--expand`; a fresh `--auto` run produces a very different file.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 bytes in 3 functions |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170; 13 interior entries and 1 filed defect, both confirmed by control flow |
| overlay code | **100%** | 91,540 by the table; 93.9% by merged extents, discrepancy flagged |
| ARM7 wram | 71.5% | **call graph consistent** — zero unmapped targets; a main-RAM region remains unmodelled |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 54.4% | 2,501 listed + 2,569 derived, 129 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
