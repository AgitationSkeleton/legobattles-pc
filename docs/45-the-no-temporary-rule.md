# The no-temporary rule

**Status: decompilation moves from 0.143% to 0.157% — 377 functions, 1,960
bytes. All 26 newly added functions matched on the first attempt, because they
were selected by a criterion that predicts matching rather than by size.**

This pass began by chasing the UI layout format and abandoned it. Both halves
are recorded.

## The parser chase, and where it stopped

`docs/44` located `func_020E16C8` as the layout loader called by 63 of the 64
screens, and named it as the obvious next lead. Following it:

```
func_020E16C8   a guard: if not already loaded, call the next one and set a flag
func_020E4438   registers func_020E484C as a callback, then calls func_020EFE80
func_020E484C   looks the resource up, and on success calls a virtual through
                [[r0]+0x20] -- so the actual parse is a virtual dispatch
```

Three hops in, the chain reaches a **virtual call**, and the layout parsing is
whatever the screen's own vtable slot does. That is not a dead end, but it is a
different and larger job than reading one function.

> **Resolved in `docs/46`.** It was not a larger job: every screen's slot 8
> *calls its base first*, so there is one parser, `ScreenType::virtual_08`. The
> container is now decoded and validated on 132 of 132 layouts.

One reading was wrong along the way and is worth correcting rather than
dropping. `func_02003618(obj, 0x0214F454, 0x2F9)` looked like an assert taking a
file pointer and line number `761`. It is not: `0x0214F454` is a **zeroed
global**, not a string, and `func_02003618` reads three fields from its first
argument and calls an allocator. The constant is a size or an id. *A
three-argument call whose middle argument is a pointer and whose last is a small
integer looks exactly like `assert(x, __FILE__, __LINE__)`, and here it was a
memory request.*

**Data decoded stays at 6.8%.**

## Changing method, and the criterion that came out of it

Rather than sink further into one format, the decompilation segment got the
remaining time — with a question that had not been asked properly before:
**which shapes of trivial function will GCC reproduce exactly?**

`docs/34` had the answer without generalising it. The two `CollectableItem`
accessors failed to match because of *one register*: the ROM chases a pointer
reusing `r0`, GCC routes through `r3`. So:

> **A shape matches when it needs no temporary register.** Where the original
> used a scratch register, the compilers will pick different ones and the bytes
> cannot agree however correct the C is.

That is a predicate on the *shape*, not a guess about the compiler, and it can
be applied before writing a line of C.

Enumerating the 140 unclaimed 2–3 instruction functions by shape and applying
it:

| shape | temp needed? | count |
| --- | --- | ---: |
| `str r1, [r0, #N]` | no — the value is already in `r1` | 17 |
| `lsl r0, r0, #N` | no — operates on its own argument | 4 |
| `mvn r0, #N` | no — writes the result register directly | 3 |
| `adds r0, #N` (thumb) | no | 6 |
| `mov r1, #N ; str r1, [r0, #N]` | **yes** — `r1` is a scratch | 14 |
| `mov r1, #N ; strb r1, [r0, #N]` | **yes** | 7 |
| `ldr r0, [r0, #N] ; ldr r0, [r0, #N]` | **yes** — the pointer chase | 6 |

The first four were added; the last three were left alone, predicted to
mismatch.

**All 26 added matched.** 351 → 377 functions, 1,788 → 1,960 bytes, and not one
failure. The criterion did the work that trial and error would otherwise have
done at the cost of a manifest full of recorded mismatches.

## Why this is the useful form of the result

The number moved by 0.014 percentage points, which is nearly nothing. **The
criterion is the result, not the number.**

Before this pass, extending the decompilation meant picking functions and seeing
what happened. Now there is a rule that sorts candidates in advance, and it is
falsifiable: any temp-free shape that fails to match refutes it. The three
shapes deliberately skipped are the standing prediction — 27 functions that
should mismatch under GCC and should match under mwccarm, which makes them a
ready-made test of the toolchain the moment mwccarm is available.

`docs/34` named mwccarm as the unlock for this segment and that has not changed.
What has changed is that the GCC-reachable set is now **characterised** rather
than sampled: the temp-free shapes are exhausted, and everything remaining
needs the original compiler.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling; no second source exists |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | 6.8% | unchanged; the UI parse is a virtual dispatch |
| decompilation | **0.157%** | 377 functions byte-verified |
