# Filling block 1 is visible through BG1

> **WRONG, retracted by [docs/117](117-block-1-is-bg1s-map-and-the-layers-are-packed.md).**
> The 60.6% change was BG1's map being CORRUPTED by this test, not a hidden
> overlap being revealed. BG1 is not an empty layer: it is a populated,
> actively-streamed terrain layer whose map is block 1. Do not act on the plan
> below.

`docs/115` predicted that block 1 is shared -- BG2's second half and BG1's first
half -- and that filling it would make the tiles appear twice. It recorded that
as untested. This is the test.

## Method

In a live match, copy block 0's **live tile entries** into block 1, so the fill
is real terrain rather than a synthetic pattern that might be rejected for
looking wrong, then capture the field before and after.

```
block0 first entries: 0064 0064 0064 0064 0064 0064 0064 0064
block1 after fill:    0064 0064 0064 0064 0064 0064 0064 0064   (write confirmed)
```

## Result

**60.6% of the field's pixels changed.**

Visually the field is not corrupted -- it still reads as a correct scene -- but
it gains terrain that is not supposed to be there: a dense band of trees down
the left where there had been a thin partial row, and ground texture patches
across previously plain grass.

That is BG1, at priority 3, rendering the newly-filled block **behind** BG2 and
showing through wherever BG2's tiles are transparent. Which is precisely the
mechanism `docs/115` described.

So the prediction is confirmed, and **block 1 cannot simply be filled**. It is
not spare memory.

## What this establishes about BG1

Before the fill, the field looked correct with block 1 unwritten, so BG1 was
displaying nothing. Its map is all transparent because nobody ever writes it.
**BG1 is an enabled but empty layer** -- it costs a layer slot and contributes
no pixels.

That is what makes the widescreen path viable rather than blocked: the conflict
is with a layer that is not being used for anything.

## The corrected plan

Filling block 1 for BG2's columns 32..63 requires that BG1 stop pointing at it.
Two ways, in preference order:

1. **Repoint BG1's screen base** to an unused 2 KB block. Engine A's BG banks
   are VRAM_E and VRAM_F; blocks 0, 1 and 2 are in use and the bank is far
   larger, so a free block exists. BG1 keeps working exactly as it does now,
   which is to say it keeps displaying nothing, and block 1 becomes exclusively
   BG2's.
2. **Disable BG1** in DISPCNT. Simpler, but it changes the layer count, and a
   layer that is empty *now* may not be empty in every scene -- menus, other
   maps, campaign missions. Repointing is the conservative choice for the same
   result.

Either is a guest-side change to the recompiled code, permitted when it follows
from decomp evidence, and presentation-only, so `docs/85`'s zero Sim->User calls
keep it lockstep-safe.

Neither has been done yet. What is established is that block 1 is reachable,
writable, visible, and currently owned by a layer that draws nothing.
