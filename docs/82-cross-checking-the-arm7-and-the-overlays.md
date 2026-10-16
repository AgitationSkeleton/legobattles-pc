# Cross-checking the ARM7 and the overlays

**Status: the call-graph test from `docs/80` is applied to the two segments that
had only ever been byte-verified. The overlays pass it perfectly — and fail a
different one. The ARM7 turns out to have 42 unrecorded functions and 45 calls
into a region the corpus does not model at all.** **119 of 119 checks pass.**

> **`docs/92` acts on this.** The third region named below — the ARM7's
> `mainram` autoload — was excluded from `lb_scan` by a set literal
> (`EXEC_SECTIONS`), which is why every scan reported ~63 KB executable. It now
> contributes **153 functions**, and this document's check moved from asserting
> two sections to three.

## The ARM7 is three regions, not one

The corpus has **333 ARM7 functions in two disjoint sections**:

```
static     3 functions   0x02380000..0x023801CC     460-byte span
wram     330 functions   0x037F8000..0x038074B0  62,640-byte span
```

Running the check naively gave nonsense, because I took `min(address)` to
`max(end)` as "the range" — which spans **25 MB** across the gap between the two
sections and swept in addresses belonging to neither. **A minimum and a maximum
over a disjoint set is not a range.** Redone per section:

```
793  call targets inside the two mapped sections
 42  are not corpus function starts
  0  of those are interior to a known function
 42  unexplained -- every one lands in a GAP between recorded functions
```

> **Retracted by `docs/83`.** All 42 are **artefacts of linear disassembly**.
> Re-run using the scanner's own control-flow call list, the ARM7 has **zero**
> unmapped in-region call targets. The recorded extents are correct; reading them
> linearly decodes the literal pools *inside* them as instructions. The paragraph
> below is wrong and is kept so the mistake stays visible.

~~Zero interior entries is the opposite of the ARM9 result (`docs/80`: 13 of 18
interior). On the ARM7 the anomalies are not boundary disputes — **they are 42
functions the corpus does not have**, sitting in the gaps. That is a concrete
account of the ARM7 wram figure the segment table has carried at **71.5% "at its
ceiling"** for many passes: the ceiling has 42 named holes in it now.~~

### And 45 calls leave both sections

```
0x027E0000 page   25 calls
0x027F0000 page   15 calls      (docs/83: 20 by linear, 15 survive control flow)
```

**Forty** call sites target **main RAM around `0x027E0000`**, which is in
neither mapped section. The ARM7 executes code there and **the corpus does not
model that region at all** — not partially, not at a low percentage, but not at
all. Anything reading the ARM7 as "static 100% plus wram 71.5%" is missing a
third area entirely.

### What "404 of 404" actually means

The segment table's `ARM7 static code — 100%, 404 of 404` covers the **three
functions** in a 460-byte span. It is true, and it sits in a table next to
`ARM9 .text — 100%, 1,201,170 of 1,201,170`. **404 bytes and 1.2 MB are not
comparable claims** and the table's shape invites reading them as if they were.
The figure stays; this paragraph is the context it needs.

## The overlays pass the call-graph test outright

```
278  intra-overlay call targets
  0  not corpus function starts
```

**Perfect.** Every call inside an overlay lands on a recorded function start.
After the ARM9 finding this was the result worth having, and the overlays earn
it.

## But the sizes are impossible

Summing the overlay corpus's function sizes gives **174,632 bytes against 94,048
bytes of overlay image — 185.7%.** A coverage figure over 100% is not a rounding
problem, it is a structural one.

Every one of the 91 overlays has the **same skeleton**:

```
ov000  image 896 B
   +0x0000  size 810  -> ends +0x32a
   +0x0008  size 802  -> ends +0x32a      <- same end
   +0x0340  size  36  -> ends +0x364
   +0x0350  size  20  -> ends +0x364      <- same end
```

**All 91 overlays have an entry at `+0` and another at `+8` that end at exactly
the same offset.** They are not two functions; they are one function recorded
twice, with and without an 8-byte prologue — the *alternate entry point* class
`docs/81` filed a row for and `config/extra_functions.tsv` already describes.
The 36/20 pair at the end is the same thing with a 16-byte prologue.

Merging the intervals:

```
372  corpus rows
181  distinct code extents
190  alternate entries collapsed (191 by interval merge, which also folds
     the one nested extent in ov050)
```

**Half the overlay "functions" are second entry points into functions already
counted.**

## The coverage number that needs stating carefully

Merged extents cover **88,294 of 94,048 image bytes — 93.9%**, leaving 5,754
bytes. The segment table reports `overlay code — 100%, 91,540 of 91,540`.

Those denominators differ: 94,048 is every byte of every overlay image, 91,540 is
whatever the table counts as code. Between the table's 91,540 and my merged
88,294 there are **3,246 bytes** unaccounted for either way.

**I am not claiming the table is wrong** — its denominator is derived elsewhere
and I have not re-derived it. What is established is that *one independent
measure*, merging the corpus's own extents, does not reach the table's numerator.
That gap is now a flagged discrepancy rather than an assumption, and resolving it
means finding how 91,540 is computed.

## Tips and pitfalls

* **A minimum and a maximum over disjoint regions is not a range.** It swept a
  25 MB span and produced 25 phantom findings before I noticed.
* **A coverage figure above 100% is a structural signal, not a rounding error.**
  185.7% was what exposed the alternate entries.
* **Summing function sizes assumes the functions are disjoint.** They are not,
  here or in the ARM9, and nothing in either corpus says so.
* **Passing one test is not passing.** The overlays are perfect on the call
  graph and structurally broken on the size sum. Two independent tests found two
  unrelated truths.
* **Small denominators hide inside big tables.** "404 of 404" reads like the
  1.2 MB row above it.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 bytes in 3 functions — a small claim, see above |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170, one entry-point correction filed |
| overlay code | **100%** | 91,540 of 91,540 by the table; **93.9% by merged extents**, discrepancy flagged |
| ARM7 wram | 71.5% | **no missing functions** (`docs/83` retracts the 42), but an unmodelled main-RAM region is real |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 54.4% | 2,501 listed + 2,569 derived, 129 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
