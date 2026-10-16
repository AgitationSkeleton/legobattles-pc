# The function corpus, and a clean cross-check

```powershell
python tools\build_corpus.py --dsd "…\Lego-Battles-Decomp-master\config\eur\arm9" ^
                            --rtti out\eur\rtti.json --out out\eur\corpus.tsv
```

| source | functions | method |
| --- | ---: | --- |
| third-party `ds-decomp` config | 9,312 | delinking analysis over the ARM9 and 91 overlays |
| this project's RTTI walk | 2,454 | vtable slots read out of the image |
| **merged** | **9,312** | |

## The result that matters

```
in both sources:        2454
known only to the RTTI: 0
known only to dsd:      6858

ARM/Thumb agreement on the 2454 shared addresses: 2454 agree, 0 DISAGREE
```

**Two derivations that share no method agree on every one of 2,454 addresses,
including every instruction-set flag.**

That is worth stating carefully, because it settles two separate questions at
once:

1. **The dsd corpus is trustworthy.** Its ARM/Thumb data is the input a static
   recompiler most depends on, and it now has 2,454 independent confirmations.
   The LSW2 project measured a prologue-matching heuristic at 4 errors in 5,860
   — every one a Thumb function lifted as ARM, which produces something entirely
   wrong rather than subtly wrong. That failure mode is now excluded for a
   quarter of this corpus by direct evidence.
2. **The RTTI walk is not producing junk.** Every vtable slot landed exactly on
   a function entry point that an unrelated analysis had already identified.
   None fell mid-function; none pointed at data. Had the walker been misreading
   vtable extents, the "known only to the RTTI" count would not be zero — it is
   the number that would have exposed the mistake, and it is the one that was
   checked first.

The second point is the one that could have gone wrong quietly, and it is worth
noting *why* it was checked: an instrument whose failure looks like a result is
the trap the LSW2 playbook ranks third by cost. A vtable walker that overruns
into the next object emits plausible-looking addresses. The test that
distinguishes "working" from "overrunning" is whether its output lands on
independently-known function starts, and it does, 2,454 times out of 2,454.

## What the corpus looks like

`out/eur/corpus.tsv` — one row per function:

| column | |
| --- | --- |
| `address` | runtime address |
| `mode` | `arm` or `thumb` |
| `size` | extent, where dsd knows it |
| `sources` | `dsd`, `rtti`, or `dsd+rtti` |
| `name` | the dsd name — 45 are meaningful, the rest are `func_…` |
| `owners` | `Class::virtual_NN` for every vtable slot pointing here |

2,454 of the 9,312 (26.4%) carry at least one class attribution. 3,767 vtable
references resolve to those 2,454 addresses, so some functions appear in several
vtables — the ordinary result of a base class's implementation being inherited
rather than overridden, and itself a signal about which slots a subclass
actually changes.

## Ghidra headless is confirmed working

Day-one item 2 of the LSW2 playbook, checked before anything was built on it:

```
Ghidra 12.1.2 (build 2026-06-05), JDK 21.0.12, both vendored under the LSW2 tree
analyzeHeadless … -import out/eur/overlays/ov000.bin
                  -processor ARM:LE:32:v5t -loader BinaryLoader
                  -loader-baseAddr 0x0216A2E0 -noanalysis -overwrite
→ Import succeeded
```

Raw-binary import at an explicit base address works, which is the shape every
program in this ROM needs — none of them are ELF and all of them have a load
address that only the header, `MODULE_PARAMS` or the overlay table knows.

## What is still missing from the corpus

- **The ARM7.** No function list at all yet. `0x26F28` bytes at `0x02380000`,
  uncompressed. It also has not been checked for a relocated block in shared
  WRAM — on LSW2 that block was in no header and was worth 62 million
  interpreted instructions.
- **Data ranges.** Undeclared data decodes as code. Nothing has been exported
  yet; this is a Ghidra `ExportDataRanges` pass.
- **Non-virtual functions have no names.** 6,858 of 9,312 are `func_…`. The
  RTTI gives classes, not members. Names for those come from the string
  references, the literal-pool cross-reference, and ordinary reading.
- **A frozen closure.** The playbook's day-one item 4 — decide what set of
  functions the project needs and report against it from the first week, so
  progress is never a feeling. Not yet written.
