# There is no free VRAM in engine A's BG banks

`docs/119` left one gate on widening: the map packing from `docs/117` has to be
undone, which needs somewhere to move BG1's and BG3's maps to. This measures
whether that space exists. It does not.

## Occupancy

Engine A's BG banks are **VRAM_E** (`VRAMCNT_E = 81`, MST 1, 64 KB at
`06000000`) and **VRAM_F** (`VRAMCNT_F = 91`, MST 1, offset 2, 16 KB at
`06010000`). Reading all 80 KB and classifying each 2 KB block:

```
06000000  non-zero  960/2048   used   (BG2 map, block 0)
06000800  non-zero 1577/2048   used   (BG1 map, block 1)
06001000  non-zero 1152/2048   used   (BG3 map, block 2)
06001800  non-zero    0/2048   EMPTY
06002000  non-zero    0/2048   EMPTY
06002800 .. 06013800           used   (34 further blocks, all non-zero)
```

Exactly **two** blocks read as all-zero, and they sit immediately after the
three maps -- precisely where a widened layout would want them.

## They are not free

Filling both with `A5A5A5A5` and letting the game run one frame:

```
field screen changed:  99.99%
top   screen changed:   0.43%
```

**The entire field is destroyed.** So the two zero blocks are live **tile data**
that the field references everywhere -- blank tiles, whose pixels are all
transparent or all one colour, which is why they read as zero while being in
constant use. Zero content is not evidence of zero use.

The 0.43% on the top screen is engine B animation, not an effect.

Restored to zero afterwards.

## Consequence

Engine A's BG VRAM is **fully allocated**. There is no slack to relocate BG1's
or BG3's map into, so the packing described in `docs/117` cannot be undone
within the current bank assignment, and the widening plan in `docs/119` is
blocked at its structural step.

Constants 1 to 3 of that plan -- the `#0xa` column count, the `0x40` row stride,
the 832-entry DMA length -- remain correct and are unaffected. It is only the
destination that has nowhere to go.

## What is left to try

Making room means changing the **VRAM bank assignment**, not shuffling within
it:

* **VRAM_H is disabled** (`VRAMCNT_H = 03`, enable bit clear), 32 KB unused. Its
  MST 1 maps it to *engine B* BG, not engine A, so it cannot directly help --
  but engine B currently has **no BG bank at all** among the enabled banks,
  which is worth resolving before relying on that reading.
* **VRAM_G** (`83`, MST 3) and the four texture-slot banks A to D (`83`, `8B`,
  `93`, `9B`) are candidates for reassignment, at the cost of whatever presently
  uses them -- 3D textures, in the case of A to D.
* Alternatively, accept a **narrower widening**: the field does not have to
  double. Any extra columns need extra map space in proportion, and a modest
  widening needs proportionally less.

None of these has been measured. What is established here is only that the easy
version -- move a map into existing slack -- is not available.
