# The mission scripting interface

**Status: the 96 mission-API entry points were 55% anonymous. Ten are now read
and named, including the game's most-called function (838 sites) and its
intrusive list library — and the most-used entry point turns out not to be
gameplay at all.** Annotation 53.8% → 53.9%; **94 of 94 checks pass**.

`docs/69` ended on the rule that *"no callers" is a claim about where you
looked*, and noted the overlays had barely been scanned. The natural next step
was the overlay-to-ARM9 call graph.

## First, a check that saved the pass

I swept all 92 overlay images for calls into the ARM9 and found 110 targets,
expecting new territory. **`docs/31` already had 96 of them.** Of the 14 extras,
13 are linear-sweep false positives — bytes inside data that decode as a `bl` —
and one, `0x020281D0`, is a real function with a single call site.

`docs/31`'s mission API was already complete. **Checking whether a job is
already done cost five minutes and saved an afternoon**, and it is the cheaper
half of the rule `docs/69` recorded.

So the question changed from *what is the API* to **how much of it is legible**:

```
mission API entry points   96
  carrying a name or an annotation   43
  carrying nothing                   53
```

**More than half the interface every mission is written against was anonymous.**
That is the gap worth closing, so I read the most-used ones.

## The most-used entry point is not gameplay

`func_02121CBC` is called from **all 91 overlays** — the only function with
perfect coverage. Thirty-two bytes of ARM:

```c
void Runtime_RegisterStaticObject(void *arg, void *dtor, Node *node) {
    node->next = g_static_objects;   /* head, from a literal-pool global */
    node->dtor = dtor;
    node->arg  = arg;
    g_static_objects = node;
}
```

A three-word push onto a global list of destructor records. This is the
**Metrowerks static-object registration** the compiler emits so that a
translation unit's non-trivial globals get destroyed at shutdown. It is called
from **430 sites in 285 ARM9 functions** as well.

**"Used by all 91 missions" is a signature of static initialisation, not of
importance.** Ranking an API by call-site count puts compiler runtime glue at
the top, and I ranked by exactly that before reading anything. The ordering was
still useful — it just does not mean what it looks like.

## The mission object holds two intrusive lists

Four entry points turn out to be one coherent structure. `func_020EF828` and
`func_020EF978` are a general **intrusive doubly-linked list**:

```
list  +0x04 head   +0x08 tail   +0x0C count
node  +0x00 vtable +0x04 next   +0x08 prev
```

```c
Node *List_Remove(List *l, Node *n) {   /* func_020EF978 */
    ...unlink, fixing head/tail for the four cases...
    n->prev = n->next = 0;
    l->count--;
    return n;                            /* returns it -- does NOT free */
}
```

`List_Remove` **returns the node rather than freeing it**, which is why every
caller that wants it gone follows with a virtual destructor call.

Now the four mission functions read as one API over two of these lists, one
based at `mission+0x08` and one at `mission+0x30`:

| function | overlays | what it does |
| --- | ---: | --- |
| `Mission_AddObjective` `0x02049F14` | **91** | `List_PushBack(m+8, o)`, defaulting `o->+0x11` |
| `Mission_LookupByKey` `0x0204A070` | 60 | walk `m+0x34` for `+0x12 == key`, return `+0x10` |
| `Mission_RemoveByKey` `0x0204A0E8` | 47 | remove **and destroy** every `+0x12 == key` in *both* lists |

The cross-check is exact and was not designed for: `Mission_AddObjective` passes
`mission+8` to `List_PushBack`, and `Mission_RemoveByKey` reads its head from
`mission+0x0C` — **head at list+4, independently confirmed from two functions
that never mention each other.** Same for the second list: base `+0x30`, head
`+0x34`, which is where `Mission_LookupByKey` starts.

`Mission_AddObjective` also carries a small rule worth porting exactly:

```c
if (!o->flag_11 && flag) o->flag_11 = flag;   /* set only if unset */
```

The caller's flag is a **default, not an assignment** — an objective that
already carries one keeps it.

## The allocator, and what the build kept

> **`docs/71` corrects the two names in this section.** These are not generic
> "AllocObjective" helpers — `docs/32` had already identified them by RTTI as
> the `DestroyMarkedUnitsObjective` and `TriggerAreaObjective` factories, two of
> a family of thirteen. The structural names below were superseded and the tier
> R rows replaced.

Two more entry points (`0x0204732C`, `0x020474EC`) are object factories with the
same two-step shape:

```c
void *p = Heap_New(0x28, file, 0x45);
if (p) Ctor(p, a, b, c, d);
```

`func_0204C6E4` is `Heap_New`, and at **838 call sites across 557 functions it
is the most-called function found in this project.** It resolves to a heap
pointer from its literal pool plus a size, with a null result routed to a
handler.

Its signature is the debug-heap `operator new(size, const char *file, int
line)`. The interesting part is what survived stripping: **both factories pass
the same `file` pointer, and it points at a zero byte** — the file-name strings
were blanked to one shared empty string — **while the line numbers are intact
and distinct**, `0x45` and `0xC1`, 69 and 193.

So the shipped ROM carries per-call-site line numbers for every one of those 838
allocations with no file names to attach them to. For anyone rebuilding this:
the third argument is real data that has to be reproduced, and it is `__LINE__`
of the original source file.

## Read but not concluded

`func_02049B00` (49 overlays) collects entities into a stack vector, and for
each one tests a predicate against the local player, skipping on true; on the
first entity that fails it calls `func_02005460` with the entity's `+0x14` and
returns. The shape is "find the first entity not belonging to me and do one
thing with its position" — plausibly a camera focus. **The predicate and
`func_02005460` are unread, so the name is not claimed.** Recorded structurally
in tier R with the interpretation flagged.

> **Answered by `docs/71`, and the claim below is wrong.** Mission objectives
> and fog circles are **not** nodes of the same list library. `ObjectiveBase`
> derives from `Battles::DListItem` and `User::FogCircle` derives from
> `Battles::SListItem` — two separate libraries, doubly and singly linked. The
> offsets coincide by accident, which is `docs/60`'s rule exactly.

One coincidence deliberately not resolved: mission list nodes use `+0x10` as a
value, `+0x11` as a flag and `+0x12` as a key, and `docs/63`'s fog circles use
`+0x10` radius, `+0x11` mark-explored, `+0x12` footprint. ~~Both are nodes of
this same list library, so they may share a base class~~ — or `docs/60`'s rule
may apply and **the same offset in two record types is two fields.** The next
step is the fog circle's constructor and vtable, not more offset matching.

## Tips and pitfalls

* **Check whether the job is already done before doing it.** The sweep that
  opened this pass duplicated `docs/31` entirely.
* **Call-site count ranks glue above gameplay.** The top entry point across all
  91 missions is the compiler's static-destructor registration.
* **Naming a leaf pays out across the tree.** `Heap_New` alone gives context to
  557 functions; two list primitives explain the shape of every container
  record in the game.
* **A list primitive that returns the node is a memory-ownership statement.**
  `List_Remove` unlinks and hands back; destruction is the caller's job.
* **Stripped strings do not mean stripped data.** Blank file names, intact line
  numbers.
* **A check's own constants need verifying too.** The list cross-check failed
  on its first run because I hand-computed the Thumb encodings for
  `ldr r4,[r7,#0xc]` and `[r7,#0x34]` and got both wrong. The claim was
  right. `docs/68` recorded that a failing check deserves a diagnosis before
  a fix; this is the second time running that the check was at fault, so the
  sharper rule is to **read the encoding out of the ROM rather than deriving
  it** -- the same standard the project applies to everything else.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **53.9%** | 2,501 listed + 2,517 derived, 70 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
