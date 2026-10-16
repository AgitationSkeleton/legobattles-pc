# Thunks, and a detector that was too specific

**Status: annotation reaches 37.9%. A fourth tier adds 334 tail-call thunks,
and the static-initialiser detector from `docs/41` is corrected — it was
matching an incidental detail and undercounting by 26.**

## The correction first

`docs/41` detected static initialisers by requiring a function to call **both**
`func_020F2BA0` and `func_02121CBC`. Reading the next family along,
`0x02122BF8`, showed the same eight-instruction shape with different contents:

```arm
push {r3, lr}
ldr  r0, [pc, #0x14]     ; 0x02151890 -- the static object
bl   func_020F13F0       ; a DIFFERENT constructor
ldr  r0, [pc, #0xc]
ldr  r1, [pc, #0xc]      ; Battles::SList::virtual_00 -- its destructor
ldr  r2, [pc, #0xc]
bl   func_02121CBC       ; the same atexit push
pop  {r3, pc}
```

**The constructor is not the signature; the atexit push is.** `func_020F2BA0`
is merely the most common constructor — 209 of 235. Fifteen initialisers use
`func_020F13F0`, and a dozen more use one-offs including
`Battles::Vector2s16::Vector2s16`.

Relaxed to *calls the atexit push, small, loads at least three literals*, the
count is **235** — and all 235 still construct a distinct object, no address
repeating, so the looser rule did not become a loose rule.

The lesson is a specific one and worth naming: **the first example of a pattern
teaches you the pattern plus its accidents, and you cannot tell which is which
from one example.** `docs/41` read four members before writing the rule and
still encoded an accident, because all four happened to use the common
constructor. The fix came from reading a *fifth* family that the rule had
already rejected — which is the useful habit: check what your filter throws
away, not only what it keeps.

## Tier T — tail-call thunks

A thunk loads a function pointer from its literal pool, jumps to it, and never
returns to its own caller:

```arm
0x0210D560:  ldr ip, [pc, #4]   ; = func_0210D4D8
             mov r0, #0
             bx  ip
```

**407 in the ARM9, 334 of them unnamed, pointing at 232 distinct targets.**
Each becomes `thunk_to_<target>`, which — like tier F — is *complete*
information. A thunk has no behaviour beyond forwarding, so naming the target
names the function.

The most-thunked targets have eleven, ten, nine and eight thunks each. That
shape is a compiler emitting one entry per overload or per default-argument
combination, all funnelling into one body.

### The detector found zero on its first run

Worth recording because the failure was silent rather than noisy. The first
version required the tail branch to be the **last decoded instruction**. It
found **zero thunks in a corpus that has 407**.

The cause: a function's literal pool sits inside its extent, and capstone
decodes those words as instructions — so the last "instruction" of
`0x0210D560` is not `bx ip` but `andseq sp, r0, #216, #8`, which is the pointer
`0x0210D4D8` read as code.

Zero is a good failure. A detector that returned *nine* thunks would have looked
plausible and been shipped. The rule now looks for the tail branch **anywhere**
in the function and requires that no return-to-caller (`bx lr`, `pop {…pc}`)
appears — because a function that can return to its own caller is doing work,
not forwarding.

## The tiers interact, and that is a check

Adding tier T dropped tier C from 354 to **327**. Those 27 functions were
thunks that the caller-attribution rule had been labelling with a class.

Both labels were true — a thunk called only from one class's methods *is* that
class's, and it *is* a thunk — but `thunk_to_X` says strictly more. Claiming
tiers in order S → F → T → C, strongest first, is what makes the assignment
deterministic rather than dependent on which check happened to run first.

```
2,501 of 9,314 functions  -> 26.9%
plus 1,031 derived -> 37.9%
   S  135  named from a sole string reference
   F  235  C++ static-object initialisers
   T  334  tail-call thunks
   C  327  attributed to a class: every caller is a method
S, F and T are names; C is an OWNER, not a name.
```

**704 of the 1,031 are now real names**, up from 344 two passes ago, and the
proportion matters more than the total: tier C's share of the derived set has
fallen from 72% to 32%.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling; no second source exists |
| ARM9 annotated | **37.9%** | 2,501 listed + 1,031 derived |
| data decoded | 6.8% | |
| decompilation | 0.143% | 351 functions byte-verified |

The structural seams are not exhausted. `0x0210C9A0` (16 × 96 bytes) and
`0x020879DC` (8 × 52, allocating `0x1C` bytes with tag `0x22`) are two families
still unidentified, and the accessor family at `0x02002EF0` — six copies of
`ldr r0,[r0,#4]; ldrb r0,[r0]; bx lr` — is a *decompilation* candidate rather
than an annotation one, though `docs/34` predicts GCC will route its double
dereference through `r3` and mismatch.
