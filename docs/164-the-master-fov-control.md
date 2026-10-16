# The master field-of-view control

`02151F28 + 8` is the game's viewport width in tiles. Writing it widens the
game's **own** rendering, persistently, and everything else follows.

## The chain, end to end

```
02151F28   the ROOT viewport descriptor   {+4 stride, +8 width=32, +12 height=24}
   |  copied every frame by func_0203A5BC
   v
0224B590   the view's copy  (view + 0x164)   -> its +8 is view+0x16C
```

`docs/163` found the view's copy and saw writes to it revert within a frame.
That is because the view's copy is **refreshed from the root**. The root itself
is written **once**, at init, by `func_020EEB1C` -- confirmed by watching it from
boot and seeing exactly two writes, `32` and `24`.

So the root is the durable place to change it, and the per-frame refresh becomes
an ally rather than an obstacle: it propagates the new value instead of undoing
it.

## It works

```
before:  root w=32  view copy w=32
write:   root +8 = 56, root +4 = 7      (56 bits needs 7 bytes of stride)
after:   root w=56  view copy w=56
nudge:   root w=56  view copy w=56      <- survives
```

And the field renders with terrain across nearly the full 448 pixels, castle and
units correct.

**Critically, this run did neither of `docs/157`'s hacks** -- block 2 was not
filled by hand and BG3 was not disabled. The extra terrain is the game's own
streamer covering a wider rectangle because the game now believes the viewport
is wider. That is the difference between widening the picture and widening the
*view*, and it is what `docs/162` argued the requirements actually need.

## What is verified and what is not

Verified: the control exists, propagates, persists, and the game fills the wider
area with terrain itself.

**Not yet verified**, and each needs its own check:

* **units and buildings** beyond the old 256 -- `gx_polygons` stayed at 3, but
  the castle and units were inside the original view throughout, so the test was
  not diagnostic. It needs a unit deliberately placed in the new margin.
* **fog of war / shroud** -- the root descriptor is a one-bit-per-tile mask
  (`docs/163`), so widening it without enlarging its backing buffer may read or
  write past the allocation. The stride was raised from 4 to 7 bytes here, which
  changes the buffer's required size from 96 to 168 bytes; nothing was allocated
  for that, and that is a latent overrun to resolve before this ships.

  **RETRACTED -- there is no overrun. See docs/183.** `func_020eeb1c` does not
  merely assign the stride: it frees the old buffers and ALLOCATES the mask at
  stride * height and a row-pointer table at height * 4. Verified live -- with
  the width patched to 56 tiles the stride becomes 7 and the row table moves by
  exactly the 72 bytes the larger mask needs. This paragraph read eleven
  instructions too few and stated a shipping blocker that does not exist.
* **the minimap camera box** -- untested.
* **the in-field UI buttons** -- untested.

## The remaining black strip

A narrow black band survives at the right edge. 56 tiles is 448 pixels exactly,
so the arithmetic is right; the likely cause is the staging buffer's 32-entry
rows, which cannot hold 56 columns. The picture is nonetheless mostly correct,
which suggests the extra columns are landing in the neighbouring block -- the
same block 2 `docs/157` filled by hand.

That is the next thing to resolve, and it is now a buffer-sizing question rather
than a question of how to widen the view at all.
