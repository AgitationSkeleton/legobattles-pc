# Four map buffers, no terrain row table, and a better plan

## What the search found

Searching RAM for pointers to the terrain buffer `021523A0` returns **two hits,
both literal pools in code**, not a row table. The second pool holds four
buffer pointers together:

```
020398A8: 021523A0     the terrain buffer (block 1, BG1)
020398AC: 02152A20     block 0, BG2  -- the one docs/159's row table serves
020398B0: 02151F28
020398B4: 021530A0
```

So there are **four** map buffers, and `docs/159`'s row-pointer table at
`02152090` serves `02152A20` specifically. The terrain buffer is not reached
through it, and no equivalent table for it was found -- its rows are most likely
contiguous at `base + row * 0x40`, which is what the builder's stride constant
computes directly.

That is a **negative result for the plan in `docs/159`**: rewriting a row table
would widen BG2's map, not the terrain's.

## A field that looked like the answer and is not

`func_020398B8` reads the camera at `+0x5E`/`+0x60` **and** `+0x62`/`+0x64`,
computing `x + [+0x62]` and `y + [+0x64]` -- exactly the shape of a view
rectangle, and a data-driven field of view would have been the cleanest possible
fix.

Read live, **`+0x62` and `+0x64` are both 0**. They are a delta, not a size, and
the function computes the same point it started from. Not the answer.

## The better plan

`docs/157` proved the display side completely: with `LB_BG_EXTEND=1`, terrain in
block 2 and BG3 off, the field renders at 448 wide with terrain to both edges.
The only defect is that block 2 held a *copy* of columns 0..31, so the terrain
restarts at the seam.

Rather than teach the builder to emit 64 columns, **run the existing builder
twice**:

1. let the game rebuild normally -- block 1 gets terrain columns 0..31;
2. in the vblank hook, save the camera, add 256 to it, set the dirty bytes, and
   call `func_02039FB4` -- the streamer -- directly through `runtime_dispatch`;
3. copy the resulting block 1 into **block 2**, which is exactly columns
   32..63 for the shifted camera;
4. restore the camera and block 1.

This needs **no knowledge of the map format**, no code patching, and no row
table. It uses the game's own lookup to produce the correct tiles, which is the
only way to be certain they are correct.

It also sidesteps `docs/143` entirely: an override cannot be *installed* on
`func_02039FB4`, but the port can *call* it, exactly as `set_owner_override`
already calls `Entity::SetOwner`.

The cost is one extra streamer run per rebuild, and the risk is re-entrancy --
running the streamer from a hook while the game believes it owns that state.
Both are worth measuring before committing to it.
