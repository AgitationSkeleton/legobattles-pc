# The first matching bytes

**Status: the decompilation figure is no longer zero. 351 functions and 1,788
bytes compile to code byte-identical with the cartridge, verified on every run
by `tools/lb_match.py`.** Four of those are hand-written with real names and
types; the other 347 are generated stubs, and this document is careful about
the difference.

For twenty passes `docs/30`'s headline read `DECOMPILATION 0.00%`. Every other
number moved and that one could not, because nothing had been compiled. This
changes that, and — more usefully — builds the harness that makes it a
*measured* number from here on.

## The harness

devkitARM 15.2.0 was already installed. That was the missing piece:

1. `config/symbols.tsv` lists a claim per function — symbol, ROM address, size,
   ARM/Thumb, and the source file that should produce it.
2. `tools/lb_match.py` compiles each source with `-ffunction-sections`, which
   puts every function in its own `.text.<name>` section, extracts that
   section's bytes with `objcopy`, and compares them to the same address range
   in the cartridge.
3. Three outcomes only: **MATCH**, **MISMATCH** (with a byte diff), **MISSING**.

`-ffunction-sections` is what makes extraction exact rather than approximate —
no symbol-table parsing, no guessing where one function ends. Any section
carrying a relocation is **skipped and reported**, because a relocated word's
value is not final until link and comparing it to the ROM would be comparing a
placeholder.

### The compiler caveat, which cuts one way only

LEGO Battles was built with **mwccarm** (Metrowerks). This uses **GCC**. They do
not generally agree, so the two outcomes are not symmetric:

- a **MATCH is conclusive**. The bytes are the bytes; whatever produced them,
  source that reproduces the ROM exactly is decompiled.
- a **MISMATCH proves nothing**. The C may be correct and merely compiled
  differently.

That asymmetry dictated the starting point: **two-instruction functions**, where
only one encoding is sensible and compiler choice barely enters into it.

## The first four, by hand

The `MagicInfluencedUnitStats` accessor block from `docs/29`:

```c
u8  MagicInfluencedUnitStats_GetAttackInterval(MagicInfluencedUnitStats *self)
{ return self->attack_interval; }
```

All four matched immediately. They are worth more than their 32 bytes because
they carry a **real struct definition** — `src/battles/types.h` holds the
`MagicInfluencedUnitStats` layout `docs/29` recovered, with each field citing
the document that established it.

### And two that did not match, for an instructive reason

The `CollectableItem` accessors, which chase a pointer before loading:

```
rom  08 00 90 e5   bc 06 d0 e1   1e ff 2f e1     ldr r0,[r0,#8] ; ldrh r0,[r0,#0x6c] ; bx lr
got  08 30 90 e5   bc 06 d3 e1   1e ff 2f e1     ldr r3,[r0,#8] ; ldrh r0,[r3,#0x6c] ; bx lr
```

Identical instructions, identical offsets — **one register apart**. mwccarm
reuses `r0`; GCC routes through `r3`. The C is almost certainly right and there
is no reasonable way to make GCC agree.

These rows stay in the manifest, reported as MISMATCH and counted toward
nothing. Deleting them would hide a real and recurring obstacle: **the moment a
function needs one intermediate value, compiler identity starts to matter**, and
that is the ceiling on what GCC can verify here.

## The 347, generated

348 functions in the ARM9 are exactly two instructions. `tools/lb_stubs.py`
recognises four shapes and emits C for each:

| shape | count | C |
| --- | ---: | --- |
| `mov r0, #imm ; bx lr` | 263 | `int f(void) { return imm; }` |
| `ldr r0, [r0, #n] ; bx lr` | 60 | `int f(void *p) { return *(int *)((char *)p + n); }` |
| `add r0, r0, #n ; bx lr` | 11 | `void *f(void *p) { return (char *)p + n; }` |
| `ldrb` / `ldrh` variants | 14 | as above, narrower type |

**347 of 348 matched.** 1,756 bytes.

### What that is actually worth

Less than the number suggests, and saying so is the point.

A generated stub has an **invented name** (`sub_02003D50`) and an **untyped
argument** (`void *`). It reproduces the bytes — the only criterion for a
matching decompilation, and genuinely met — but recovers none of the
understanding the word usually implies. Nobody learns anything about LEGO
Battles from `int sub_02040594(void) { return 1; }`.

This is the same distinction `docs/30` already draws for annotation, where 2,454
of 2,499 annotated functions came free from the shipped RTTI. Both are large
numbers that were cheap, and both are reported with their provenance attached
rather than folded into a headline. So `lb_progress.py` prints two figures:

```
1,788 of 1,247,742 mapped code bytes   -> 0.143%   (351 functions)
hand-written with real names and types:   32 bytes (4 functions, 0.003%)
```

**0.143% is the honest decompilation figure. 0.003% is the honest
*understanding* figure.** Both are real; they measure different things.

## Two measurement bugs, caught in the same pass

**The metric counted source-file bytes.** The first version of section 1 in
`lb_progress.py` summed the size of the `.c` files on disk. With sources
present it reported **18,942 "matched bytes"** against 1,788 bytes of actually
matching code — and would have grown every time a comment was added. Fixed to
read `lb_match.py`'s verified results.

**The headline printed the wrong percentage.** `p` was reused by later sections,
so `HEADLINE: decompilation 26.836%` was the *annotation* figure wearing the
decompilation label. Caught because 26.8% was obviously impossible for a project
that had been at 0% an hour earlier.

Both are the same class of error as the one `docs/32` retracted: **a number that
is computed but not defined.** The defence is the same — state what the
denominator is and check the result against something already known.

## Where this goes

The trivial seam is now mined: 347 of 348 candidates are done, and what remains
needs real work. Two paths:

- **mwccarm.** Until the original compiler is in the loop, a mismatch is
  uninformative and anything beyond an accessor is unverifiable. `docs/03`
  identified `mwccarm` as the right toolchain and the LSW2 tree next door has
  one. This is the unlock.
- **Meanwhile, replace stubs with real ones.** Every generated stub whose owning
  class is known from the RTTI can be rewritten with a real name and a real
  struct field. That moves bytes from the 0.143% column to the 0.003% column
  without changing the total — which is exactly the kind of progress the two
  figures exist to make visible.
