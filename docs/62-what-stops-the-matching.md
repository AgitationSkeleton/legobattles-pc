# What stops the matching

**Status: decompilation 0.264% → 0.271%, 782 → 789 functions, from one more
shape. More usefully, the ceiling on what devkitARM can ever match here is now
measured rather than assumed: of the 1,724 small functions still uncovered, **at
most 96 are reachable** — and that is an upper bound.** 84 of 84 checks pass.

`docs/34` said the ceiling exists and showed one example. `docs/61` hit it once
and removed the shape. This pass measures how far away it is.

## One more shape: the load-and-shift

```
ROM  10 00 90 e5   ldr r0, [r0, #0x10]
     80 00 a0 e1   lsl r0, r0, #1
     1e ff 2f e1   bx lr
```

```c
int f(void *p) { return *(int *)((char *)p + 0x10) << 1; }
```

Three instructions, but the value never leaves `r0`, so `docs/34`'s rule still
holds. **7 functions, all matching.** The generator now recognises `lsl`, `lsr`
and `asr` after any of the three load widths.

## Four reasons a correct function does not match

Every uncovered function of 24 bytes or fewer, classified by what would stop it:

```
  719   41.7%   calls something, or uses svc
  642   37.2%   loads a constant from the literal pool
  267   15.5%   needs a scratch register
   96    5.6%   the value stays in r0        <- potentially reachable
 1724           total uncovered
```

### 1. It calls something — 41.7%

A `bl` to another function leaves a relocation in the section, and
`lb_match.py` refuses to compare a section whose bytes are not final. This is
not a compiler disagreement; it is the harness being honest. Matching these
needs a link step against a full symbol map, which is a different project shape
from "compile one file and diff".

### 2. It materialises a constant — 37.2%

63 Thumb functions are `ldr r0, [pc, #0] ; bx lr` with the literal `1`, `2`, `3`
or `4` sitting after them. GCC compiles `return 1;` to `movs r0, #1` — the value
fits an immediate, so there is no pool. mwccarm went to the pool anyway.

**There is no C expression that asks a compiler to use a constant pool.** It is
a code-generation decision below the language.

### 3. It needs a scratch register — 15.5%

`docs/34`'s original finding, now with a count. Three measurements this pass and
last:

```
bit-setter, 48 functions:
  ROM  ldrb r1,[r0,#0x40] ; orr r1,r1,#2 ; strb r1,[r0,#0x40] ; bx lr
  GCC  ldrb r3,[r0,#0x40] ; orr r3,r3,#2 ; strb r3,[r0,#0x40] ; bx lr

tail-call thunk, 75 functions:
  ROM  ldr ip,[pc] ; bx ip   ; .word target
  GCC  ldr r3,[pc] ; bx r3   ; .word target      <- literal identical

double load, 10 functions (and the project's own two standing mismatches):
  ROM  ldr r0,[r0,#8] ; ldrh r0,[r0,#0x6c] ; bx lr
  GCC  ldr r3,[r0,#8] ; ldrh r0,[r3,#0x6c] ; bx lr
```

In all three the offsets, the widths and the final `bx lr` are byte-identical
and **only the register field differs**. mwccarm chains through `r0` or the
ABI's `ip`; GCC reaches for `r3`.

### 4. It uses a different idiom — new, and worse than the others

This one is not in `docs/34`, and it is the most awkward, because it survives
every knob. `int f(void *p) { return *(int *)((char *)p + 0x18) == 0; }`:

```
ROM                      cmp r0,#0 ; moveq r0,#1 ; movne r0,#0
GCC -mcpu=arm946e-s      clz r0,r0 ; lsr r0,r0,#5
GCC -march=armv4t        rsbs r0,r0,#1 ; movcc r0,#0
GCC -Os                  clz r0,r0 ; lsr r0,r0,#5
```

**Three settings, three idioms, none of them the ROM's.** Register allocation
can at least be imagined away by a luckier prototype; instruction selection
cannot. `clz` exists on the DS's ARMv5TE and mwccarm simply did not use it.

## The ceiling

**96 of the 1,724 uncovered small functions keep every value in `r0`** and are
therefore candidates. That is 5.6%, and it is an **upper bound, not an
estimate**: the double-load shape is inside that 96 and is already known to
fail, because GCC introduces a scratch register where the original did not need
one. The true figure is lower.

So for functions of this size, the matchable population is essentially
exhausted. Larger functions are worse, not better — every extra line is another
chance for the two compilers to differ.

**This segment is compiler-bound, and the bound is measured.** `docs/34` listed
mwccarm as the unlock; this quantifies what unlocking it would be worth: the
other 94.4%.

## What this means for the figure

```
DECOMPILATION   0.271%   3,380 bytes over 789 functions
```

785 of the 789 are generated stubs. The number has moved from 0.143% to 0.271%
across `docs/34`, `61` and this pass — roughly a doubling — and essentially all
of that came from recognising shapes that were always matchable, not from
getting better at matching. **That well is now dry**, and the honest projection
is that GCC cannot take this figure past about 0.3% without a link step.

## Tips and pitfalls

* **Measure the ceiling before optimising toward it.** Four categories and a
  `Counter` turned "we should match more functions" into "at most 96 remain",
  which is a decision rather than an aspiration.
* **Distinguish the blockers.** A relocation is the harness being careful; a
  register choice is a compiler difference; an idiom choice is a compiler
  difference that no flag fixes. They need different answers and only one of
  them is worth arguing with.
* **A negative with bytes attached is worth more than a negative without.**
  "GCC disagrees" invites another attempt. `clz r0,r0` against
  `cmp/moveq/movne` does not.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 53.6% | 2,501 listed + 2,487 derived |
| decompilation | **0.271%** | 789 functions byte-verified |
