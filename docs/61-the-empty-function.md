# The empty function

**Status: decompilation moves from 0.157% to 0.264% — 377 matched functions to
782 — by recognising one shape the stub generator had been skipping. And a
second shape was tried, measured at 0 of 48, and removed.** Annotation gains a
new tier for the functions read by hand in `docs/56`–`60`. Two new checks;
**83 of 83 pass**.

## 405 functions that do nothing

`lb_stubs.py` recognised two-instruction functions and had generated 373 of
them. Classifying every corpus function of 20 bytes or fewer by *shape* shows
what it was leaving on the table:

```
 263  arm    bx lr
 160  arm    push {rX, lr} ; mov rX, rX ; bl #N ; mov rX, rX ; pop {rX, pc}
 142  thumb  bx lr ; movs rX, rX
 125  thumb  push {rX, lr} ; adds rX, rX, #N ; bl #N ; adds rX, rX, #N ; pop
  63  thumb  ldr rX, [pc, #N] ; bx lr
  48  arm    ldrb rX, [rX, #N] ; orr rX, rX, #N ; strb rX, [rX, #N] ; bx lr
```

1,912 small functions were uncovered in total. The largest group is the one the
classifier could not see because it required **exactly two** instructions:

**263 ARM and 142 Thumb functions whose entire body is `bx lr`.**

A virtual method that does nothing, or a body the original compiler folded away.
`void f(void) {}` compiles to `bx lr` and nothing else, in both instruction
sets, and all 405 match.

### The Thumb size trap

A Thumb `bx lr` is **two** bytes. The corpus records these functions as four,
because the assembler pads to a 4-byte boundary and the padding disassembles as
`movs r0, r0`. `lb_match.py` truncates the compiled section to the claimed size
and compares, so claiming four bytes compares two bytes of real code against two
bytes of padding GCC has no reason to emit.

**Claim what the function is, not what the slot is.** The generator now emits
size 2 for a Thumb empty, and the padding is correctly treated as not part of
the function.

## A shape that was tried and removed

The 48 `ldrb / orr / strb / bx lr` functions are bit-setters:

```c
void f(void *p) { *(unsigned char *)((char *)p + 0x40) |= 2; }
```

That C is correct. It was generated, compiled and compared, and it matched **0
of 48**. Here is why, from `func_0200E8C4`:

```
ROM  40 10 d0 e5   ldrb r1, [r0, #0x40]
GCC  40 30 d0 e5   ldrb r3, [r0, #0x40]
ROM  02 10 81 e3   orr  r1, r1, #2
GCC  02 30 83 e3   orr  r3, r3, #2
ROM  40 10 c0 e5   strb r1, [r0, #0x40]
GCC  40 30 c0 e5   strb r3, [r0, #0x40]
ROM  1e ff 2f e1   bx lr
GCC  1e ff 2f e1   bx lr                    <- identical
```

**Three instructions differ by one register field and the fourth is byte-exact.**
mwccarm picked `r1`; GCC picked `r3`. The source is right and the bytes are
wrong, which is exactly the situation `lb_match.py`'s docstring warns about:

> a MISMATCH is **not** evidence the C is wrong.

`docs/34` stated the rule — *only shapes that need no temporary register* — and
this is that rule with the bytes attached. It is worth having concretely,
because the abstract version invites the thought that a cleverer prototype might
fix it. Nothing about the C changes which register the allocator picks.

The shape was **removed** rather than left generating 48 known mismatches. The
experiment is recorded here; the tool is back to the rule.

### The shape not tried, and why

63 Thumb functions are `ldr r0, [pc, #0] ; bx lr` followed by a literal — and
the literals are `1`, `2`, `3`, `4`. GCC compiles `return 1;` to `movs r0, #1`,
because the value fits an immediate; mwccarm went to the constant pool anyway.
That is a code-generation choice with no C-level expression, so the shape is
listed as unreachable rather than attempted.

## Tier R: the functions read by hand

`docs/56` through `docs/60` read and named around thirty functions — the
particle loader, the allowance functions, the harvest payout, the damage
applier, the construction tick. **None of them were in `annotations.tsv`.** The
annotation figure was understating the work by exactly the part of it that took
the most effort.

`config/read_functions.tsv` now carries them, one row each:

```
address   name                        doc       evidence
0x02080F14 AstarSearch_SearchStep     docs/58   the only assert site names it
0x0206E970 Combat_ApplyDamage         docs/59   rolls [min,max], scales, subtracts
0x02071DC4 Produce_CanProduce         docs/60   the six preconditions
```

**Every row cites a document, and the check enforces both halves**: the address
must be a real corpus function, and the document must exist. A name whose
evidence has been deleted stops being a name.

This is the only tier a human writes directly and it is deliberately the
smallest — 28 rows against tier L's 994. It is also the strongest: someone read
the disassembly. It counts as a **name**, not an association.

## The numbers

```
DECOMPILATION   0.157% -> 0.264%     377 -> 782 functions
                                     1,960 -> 3,296 matched bytes
ANNOTATED        53.3% -> 53.6%      2,460 -> 2,487 derived
                                     976 names, 1,511 associations
```

The decompilation figure is still small, and the honest reading of it has not
changed: 778 of the 782 matched functions are generated stubs with invented
names and untyped arguments. What moved is the *count of bytes proven to
recompile*, which is the only thing that figure has ever claimed.

## Tips and pitfalls

* **Classify by shape before writing a recogniser.** One `Counter` over
  normalised disassembly showed 1,912 uncovered functions ranked by how many
  share a form. The largest group was invisible to a classifier that required
  exactly two instructions.
* **A padded slot is not a longer function.** Thumb code is 2-byte aligned in a
  4-byte world; the corpus size includes the padding and the compiler will not.
* **Test a shape, do not reason about it.** The bit-setter looked plausible and
  failed 48 times; the failure is one nibble per instruction and is worth seeing
  once.
* **Remove what does not match.** Leaving 48 known mismatches in the manifest to
  "show the attempt" would corrupt the only metric that means anything here.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **53.6%** | 2,501 listed + 2,487 derived |
| decompilation | **0.264%** | 782 functions byte-verified |
