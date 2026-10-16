# Verifying the hundred percents

**Status: `tools/lb_verify.py` re-checks fourteen completeness claims against
the ROM on every run, and all fourteen pass. The ARM7's static section is the
first segment taken to a *verified* 100%, and getting there found two errors
that had been cancelling each other out.**

The working rule this pass: take the segment nearest 100%, finish it, and prove
it is finished before moving on. The nearest was the ARM7 static section at
99.2% — four bytes short.

## The four bytes were not code, and neither were twenty-four others

Chasing a 4-byte gap at `0x023801CC` turned up two independent faults.

**The gap is a literal pool.** `0x023801CC` holds `0x027FFFFA`, and the code at
`0x02380194` loads it PC-relative. It is data belonging to the function that
ends just before it — the walk's extent simply stopped at the return without
including the pool.

**The 24 bytes after it are MODULE_PARAMS, and the corpus called them a
function.** `header.json` says `module_params_address = 0x023801D0`, and the
words there are the header's own fields:

```
0x023801D0  0x023A6F10   autoload_list_start
0x023801D4  0x023A6F28   autoload_list_end
0x023801D8  0x023801E8   autoload_start
0x023801DC  0x023801E8   static_bss_start
0x023801E0  0x023801E8   static_bss_end
```

Every value matches. Yet `arm7_corpus.tsv` listed a `0x18`-byte ARM function at
that address, because **two literal-pool words in the boot code point at it**
(`0x02380104` and `0x02380184`), the tier-2 code-pointer scan offered it as an
entry, and its first word happens to decode as a conditional ARM instruction —
so it passed the prologue check.

### Two errors, opposite signs

This is the part worth keeping. The section was **over-counted and
under-denominated at the same time**:

- the numerator included 24 bytes of data reported as a function;
- the denominator included those 24 bytes *plus* the literal-pool word, none of
  which is code.

They partly cancelled, which is why 99.2% looked like an ordinary
almost-finished number rather than a wrong one. **A plausible percentage is not
evidence of a correct percentage.** Only defining what is in the numerator and
what is in the denominator, separately, exposes this.

Corrected and verified:

```
[PASS] ARM7 static: MODULE_PARAMS identified   0x023801D0..0x023801E8 is data, 24 bytes, matches header.json
[PASS] ARM7 static: every code byte covered    404 of 404 code bytes -- 100%
[PASS] ARM7 static: no data reported as a function   clean
```

**404 of 404.** The section is 488 bytes: 404 of code, 56 of literal pools
referenced by that code, 24 of MODULE_PARAMS, 4 of the pool word that started
this.

## Fixed at the source, not in the checker

The false function was removed by teaching `lb_scan.py` about
**header-declared data regions**, not by editing the generated corpus — the
project never hand-edits generated output, and a hand-edit would be undone by
the next regeneration.

```
header-declared data regions excluded: 0x023801D0..0x023801E8 (3 candidates rejected)
```

Three candidates, not one: the same block was being offered by the walk queue,
the tier-2 pointer list, and the tier-3 data scan independently. Fixing one
site would have left the other two.

**The principle: a pointer into a region is not evidence the region is code.
Where the header says otherwise, the header wins.**

A second, smaller consequence: tier-2 rows are *pointer sightings* and carry no
size, so `lb_verify.py` and `lb_progress.py` both had to be hardened against a
blank size column — which they previously parsed straight into `int()` and
crashed on.

## The fourteen checks

`tools/lb_verify.py` re-runs every completeness claim in the documentation
against the cartridge. Claims rot: "136 of 136 files parse" was true when
written, and nothing guarantees it survived the next parser change.

| check | doc | result |
| --- | --- | --- |
| ARM7 static: MODULE_PARAMS identified | `docs/14` | 24 bytes, matches header |
| ARM7 static: every code byte covered | `docs/17` | **404 of 404** |
| ARM7 static: no data reported as a function | `docs/17` | clean |
| `Entities.ebp` has 551 records | `docs/11` | 551 |
| unlock slots are 0..288, contiguous | `docs/22` | 289 slots |
| build times are all multiples of 30 | `docs/25` | 180 units, 0 exceptions |
| speeds round-trip via tiles/second | `docs/26` | 12 distinct, 0 fail |
| Minifig → unit by name | `docs/27` | 84 of 85 (one shipped typo) |
| minikit awards are 18 distinct Minifigs | `docs/27` | 18, all Minifigs |
| `ObjectiveBase` has exactly 13 subclasses | `docs/32` | 13 |
| all 13 objective types used | `docs/32` | none unused |
| 122 map files | `docs/15` | 122 |
| 136 tile blueprints | `docs/21` | 136 |
| 8 language files | `docs/07` | 8 |

Three outcomes only — PASS, FAIL, SKIP — and every check prints its actual
counts rather than a verdict alone, so a number that drifts is visible before it
becomes a claim.

Note the Minifig check is written as `>= len - 1`, not `== len`. That is
deliberate: `docs/27` established the single mismatch is a **typo in the game's
own data** (`Minifig_E_SigeThree`), so a check demanding 85 of 85 would fail
forever on a fact that is already understood. **A verification that cannot
express a known exception will be switched off**, and a switched-off check is
worse than none.

## Where the segments stand

| segment | figure | verified? |
| --- | ---: | --- |
| ARM7 static code | **100%** | yes, 404 of 404 |
| overlays, function extent | 93.9% | next target |
| ARM9 code mapped | 87.3% | denominator includes rodata/data |
| ARM7 wram | 69.5% | at its scanning ceiling (`docs/33`) |
| overlays, visited | 49.1% | |
| ARM9 annotated | 26.8% | |
| data decoded | 6.8% | |
| decompilation | 0.143% | 351 functions byte-verified |

The next segment by that ordering is the overlays' 93.9%. Its denominator has
the same weakness the ARM7's just did — `ram_size` includes each overlay's
literal pools and data — so the first job there is separating code from data
before chasing the last 6%.
