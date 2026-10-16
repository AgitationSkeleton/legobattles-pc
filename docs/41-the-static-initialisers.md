# 209 static initialisers, found by looking for the shape

**Status: annotation reaches 34.3%. A third derivation adds 209 functions —
every C++ static-object initialiser in the ARM9 — found by pattern rather than
by a source that lists them.**

`docs/40` closed with the observation that both cheap derivations were spent and
the next lever would be reading code. That was too pessimistic by one lever.

## The lead came from measuring, not from reading

Rather than pick functions to read, the corpus was asked a structural question:
**are there runs of adjacent functions with identical size and mode?** A
compiler emits families like that; a human does not.

```
48 strided families of >=4 identical-size adjacent functions
   392 functions, 383 of them unnamed
```

| start | count | size | mode |
| --- | ---: | ---: | --- |
| `0x021235E4` | 80 | 32 | thumb |
| `0x020092D4` | 31 | 36 | arm |
| `0x0212337C` | 17 | 32 | thumb |
| `0x0210C9A0` | 16 | 96 | arm |
| `0x0204B750` | 14 | 12 | thumb |
| `0x021240D8` | 14 | 32 | thumb |

Two were already understood — `0x020092D4` is the 31 game-rule toggles from
`docs/28`, `0x0204B750` the container thunks from `docs/33`. **Identifying one
member of a family names the whole family**, which is what makes this worth
doing in bulk.

## The shape

Reading one member of the largest family, and then of three others, gave the
same eight instructions every time:

```arm
push {r3, lr}
ldr  r0, [pc, #0x10]     ; a static object's address
blx  func_020F2BA0       ; construct it
ldr  r0, [pc, #8]        ; the same object again
ldr  r1, [pc, #0xc]      ; a destructor
ldr  r2, [pc, #0xc]      ; object + 8, the list node
blx  func_02121CBC       ; register the destructor
pop  {r3, pc}
```

`func_02121CBC` is already known: `docs/31` identified it as the **atexit push**,
an intrusive linked list whose head sits at `0x0216A2DC`, four bytes below the
overlay load address, and noted that all 91 overlays call it because all of them
have static objects. This is the ARM9 side of the same mechanism.

So the shape is: *construct one global, then arrange for its destruction.* A
C++ static-object initialiser.

## Matching it, not the stride

The families were the lead; they are not the rule. Searching by the *behaviour*
instead — a function under 64 bytes that calls `func_020F2BA0` and then
`func_02121CBC`, and loads at least one literal — finds **209**, considerably
more than the 145 the strided families held. The rest sit alone or in runs of
two or three that the stride test discarded.

> **Corrected by `docs/42`.** The rule below requires `func_020F2BA0`, which
> is only the *most common* constructor (209 of 235), not the signature. The
> signature is the atexit push alone. Relaxed, the count is **235**.

**All 209 were unnamed, and all 209 construct a distinct object.** Not one
address repeats, which is the check that the pattern is not matching something
generic: 209 initialisers for 209 globals is what a program has; 209
initialisers sharing a handful of targets would have meant the match was too
loose.

Each is recorded as `static_ctor_<object address>`.

## Why this is a name and tier C is not

`docs/40` was careful that tier C gives an *owner* rather than a name — 354
functions labelled only with the class they belong to. Tier F is different in
kind: **`static_ctor_02150880` says what the function does** (it initialises the
global at `0x02150880`) and that statement is complete. There is nothing further
to learn about it; a static initialiser has no behaviour beyond the object it
builds.

So the metric now reports three tiers rather than lumping them:

```
2,501 of 9,314 functions  -> 26.9%
plus 698 derived -> 34.3%
   S  135  named from a sole string reference
   F  209  C++ static-object initialisers
   C  354  attributed to a class: every caller is a method
S and F are names; C is an OWNER, not a name.
```

The tiers are disjoint, and one of the reasons is worth stating because it was
not obvious in advance: **no tier-F function appeared in tier C**, because a
static initialiser is called from the init array rather than by a `BL` from
another function. It has no callers in the call graph at all, so the
caller-attribution rule could never have reached it. The two derivations look
for opposite things — one for functions with a distinctive callee, the other for
functions with distinctive callers — and that is why they compose.

## A note on where they live

The tier-F functions cluster in `0x0212xxxx`, the tail of `.text` just before
the rodata boundary `docs/36` established at `0x02125FC4`. That is where a
linker puts the static-initialisation section, and its position corroborates the
boundary from a direction that had nothing to do with how the boundary was
found.

## What is left in the families

Of the 383 unnamed functions in strided families, 209 are now explained. The
largest remaining are `0x0210C9A0` (16 × 96 bytes, ARM) and `0x020879DC`
(8 × 52, which allocates `0x1C` bytes with tag `0x22` and calls
`func_020877A0` — another factory family, of the kind `docs/32` resolved for the
objectives). Both are leads for the same method: identify one member, name the
family.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling; no second source exists |
| ARM9 annotated | **34.3%** | 2,501 listed + 698 derived |
| data decoded | 6.8% | |
| decompilation | 0.143% | 351 functions byte-verified |
