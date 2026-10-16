# 303 — The ARM7's interrupt handlers were invisible to a call-graph finder

docs/302 established that the ARM7's interpreted share is not a validation
problem — but it reached that by classifying an old reference capture against
declared TOML spans, which is not the same question as "is this address in the
dispatch table". Measured properly, against the tables the lift actually emits
and on a capture from the build being measured:

```
exp-a7valid1, a real match (19 [lb_shroud] lines, 53.3 fps)

ARM7 ROOT: 583 entries, 157,847 hits
   pc IS in a dispatch table :         0    0.0%
   pc is NOT in any table    :   157,847  100.0%

ARM9 ROOT: 352 entries,  39,869 hits
   pc IS in a dispatch table :    39,771   99.8%
```

**Every ARM7 interpreter entry is at an address with no dispatch entry at
all**, and every one of them is outside every declared span — 0 of 583 fall
inside one. So the ARM7 is interpreted because the code is undeclared, full
stop. docs/302's "92.9% mid-declared-function" figure came from a different
capture and does not hold here; it is retracted there.

The ARM9's mirror image is also worth recording: 99.8% of its interpreter
entries ARE in a table, and the new `static_reject_bytes` counter shows 6,719
validation rejections with the first at `0x0203DCB8` — beside the touch patches
this port writes at `0x0203DD14`/`0x0203DD20`. That is **by design**: a patched
function must not run from lifted code that predates the patch. It is not a
defect, and it is the whole of the ARM9's interpreted work.

## Where the undeclared ARM7 work is

```
region                                   hits   share   inside a declared span
0x037FB63C..0x037FB788  ( 50 pcs)      98,858   62.6%   0 / 50
0x038043A4..0x0380488C  (157 pcs)      23,319   14.8%   0 / 157
0x03801D50..0x03801ED4  ( 21 pcs)      10,328    6.5%   0 / 21
0x03803A48..0x03803B70  ( 51 pcs)       6,809    4.3%   0 / 51
0x03803E60..0x03803F24  ( 33 pcs)       6,607    4.2%   0 / 33
0x03804030..0x038041FC  ( 55 pcs)       5,638    3.6%   0 / 55
0x03802730..0x03802C10  ( 88 pcs)       2,878    1.8%   0 / 88
0x03803CB4..0x03803CC8  (  4 pcs)       2,437    1.5%   0 / 4
```

Eight regions are 99.3% of it.

## Why a recursive-descent finder could never have found the first one

`lb_scan.py` walks from a seed and follows `BL`/`BLX`. It says so in its own
docstring, and it is right to: "a function is either reached by a real call or
it is not reported". The ARM7's interrupt handlers are **never called**. The
interrupt dispatcher indexes a table and branches, so no `BL` anywhere names
them, and no amount of seeding from entry points reaches them.

Finding them without guessing means finding the table. Every word in the image
that points at the region lands in one contiguous run:

```
0x0380739C  0x037FB748      0x038073AC  0x037FB718      0x038073BC  0x037FB6C8
0x038073A0  0x037FB638      0x038073B0  0x037FB728      0x038073C0  0x037FB6D8
0x038073A4  0x037FB638      0x038073B4  0x037FB738      0x038073C4  0x037FB6E8
0x038073A8  0x037FB708      0x038073B8  0x037FB638      0x038073C8  0x037FB6F8
```

**25 consecutive slots, all 25 pointing into ARM7 code, 10 distinct targets** —
the ARM7 IRQ callback table, one slot per interrupt source. `0x037FB638` is a
four-byte `bx lr`, the do-nothing handler every unused source shares.

The handlers themselves are a trampoline table into one body:

```
0x037FB63C  push {r3,r4,r5,lr}   ...  the common dispatcher, 0x78
0x037FB6C8  ldr ip,[pc,#4]; mov r0,#0; bx ip; .word 0x037FB63C
0x037FB6D8  ... #1   ... through ...   0x037FB738  ... #7
0x037FB748  push {r3,lr}         ...  a ninth handler, 0x44
```

The recompiler independently agrees about the table: lifting the region now
reports `0x0380739C <- afunc_0380739C (auto jump_table)`, and correctly emits
no dispatch entry for it.

## A real bug in the finder, found on the way

`lb_scan.walk` treated `bx <reg>` as **falling through**. `is_return` catches
`bx lr` and nothing else, so a walk starting at a trampoline ran straight
through `bx ip`, decoded the literal word that follows as an instruction, and
carried on into the next trampoline — swallowing the whole table into one bogus
function.

An unconditional `bx` never falls through, so this is simply wrong, and fixing
it can only tighten extents. Two changes, both in `walk`:

* `bx <reg>` ends the block (a **conditional** `bxeq` does fall through and is
  left alone);
* a pc-relative `ldr <reg>,[pc,#N]` is remembered per basic block, so a
  following `bx <reg>` resolves its target **exactly, from the literal pool the
  CPU itself reads**, and records it as a tail call the way a `BL` would.

Measured on the same invocation, the fix removes 612 bytes of the over-run the
tool already warned about in its own output (`extents overlap by 1128 bytes` ->
`516`), and adds no bytes anywhere.

## The change, and why it is additive

`--auto` for the ARM7 now also seeds from the IRQ table, **reading the targets
from the image** rather than listing this build's contents.

Regenerating the corpus outright was rejected. Measured, a full regenerate is
**+325 functions, -38, and 26 shrunk** — a real improvement in aggregate, but
38 functions that lift today would stop lifting, and that needs its own
before/after run rather than a loop tick's confidence. So `lb_scan` grew
`--merge`: every row of the existing corpus is kept byte-for-byte and only
addresses it lacks are added.

```
merge: kept 469 existing rows, added 11
   0x037FB638  0x4     0x037FB6E8  0xC     0x037FB728  0xC
   0x037FB63C  0x78    0x037FB6F8  0xC     0x037FB738  0xC
   0x037FB6C8  0xC     0x037FB708  0xC     0x037FB748  0x44
   0x037FB6D8  0xC     0x037FB718  0xC
existing rows changed: 0
```

Every size matches the disassembly read by hand, and the region accounts to the
byte. `port/config/lb_arm7_wram.toml` gains 11 entry points and nothing else;
`lb_arm9.toml`'s only diff is three blank lines.

After relifting, all four probe addresses resolve, and `0x0380739C` — the table
itself — correctly gets no entry:

```
0x037FB63C  {0x037FB63Cu, 0u, lb_arm7_wram_afunc_037FB63C, ...}
0x037FB684  {0x037FB684u, 0u, lb_arm7_wram_afunc_037FB63C, ...}   <- the return point
0x037FB718  {0x037FB718u, 0u, lb_arm7_wram_afunc_037FB718, ...}
0x037FB748  {0x037FB748u, 0u, lb_arm7_wram_afunc_037FB748, ...}
0x0380739C  absent
```

Dispatch table: 9,524 -> 9,596 entries.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** the ARM7 IRQ callback table at `0x0380739C`, 25 slots, 10
distinct handlers, found by exhaustive search of the image rather than assumed.

**ESTABLISHED:** `walk` mis-handled `bx <reg>`, and the fix is strictly a
tightening — 612 bytes of over-run removed, none added.

**ESTABLISHED:** the corpus change is additive; zero existing rows differ.

**ESTABLISHED (measured below):** this lowers the ARM7's interpreted share.
The region's interpreter entries go to zero and the ARM7's total falls 63.0%,
with `cache_hit` as a length control at 0.4% between the two runs.

**NOT ESTABLISHED:** any effect on the silent unit sounds, the flicker, or the
framerate. fps in particular must not be read from a single run — this
project's own harness swings 29-38 fps with no change at all.

## Next

The other seven regions, 36.7% between them, are not IRQ handlers — the table
accounts for region 1 only. Each needs the same treatment: find what points at
it, confirm by disassembly, seed, merge. And the full regenerate (+325/-38/26
shrunk) is worth doing properly, with the 38 losses examined one by one.

## MEASURED: the region goes to zero

Same route, same harness, before and after the eleven entry points:

```
run        shroud  fps      a7 cache_hit  a7 absent   absent%  a7 ROOT hits
a7valid1     19    53.32       4,389,942  3,304,104    42.9%       157,847
a7irq1       17    50.98       4,408,343  1,921,556    30.4%        58,477

IRQ-handler region 0x037FB63C..0x037FB788 ROOT hits:
   before 98,858      after 0
```

**The region is eliminated completely** — not reduced, zero — which is what a
correct entry-point declaration should do and is far harder to get by accident
than a percentage that merely moves.

* ARM7 native-to-interpreter boundaries: **157,847 -> 58,477, down 63.0%**
* ARM7 `cache_hit_absent` share: **42.9% -> 30.4%**

**The length control matters here** and it holds: `cache_hit` is 4,389,942
against 4,408,343, a 0.4% difference, so the two runs did comparable ARM7 work
and the drop is not a shorter route. Both reached a real match (19 and 17
`[lb_shroud]` lines).

**No fps claim.** 53.32 against 50.98 is inside this harness's own noise — the
project has already retracted one framerate conclusion drawn from single runs,
and this is not a second attempt at it.

**No sound claim.** Whether this makes silent units audible is item 2 and stays
unclaimed until heard in a play session.
