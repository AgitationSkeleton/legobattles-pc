# VRAM_D is free: 128 KB for engine A

`docs/120` concluded "there is no free VRAM in engine A" and that conclusion
gated the whole widescreen thread. It was true as stated -- **within VRAM_E and
VRAM_F** -- and it was the wrong question. A *bank* can be reassigned.

## The test

`VRAMCNT_D` was `9B`: enabled, MST 3, offset 3 -- **texture slot 3**. Setting it
to `89` makes it engine A BG at `0x06020000`.

One detail first: **an 8-bit write to `0x04000243` is ignored** by this path,
and reads back unchanged. A 32-bit write to `0x04000240` carrying the new byte
works:

```
VRAMCNT_A..D  9B938B83  ->  89938B83
```

Every earlier attempt to poke a single VRAMCNT byte would have silently done
nothing, which is worth knowing before trusting any such experiment.

## The result

With `VRAM_D` handed to engine A BG:

* **the field renders correctly** -- castle, units, terrain, all intact, 48686
  of 49152 pixels non-black, and the capture looks right rather than merely
  non-black (`docs/130`'s lesson);
* **GX polygons unchanged at 3**, so the 3D units did not lose geometry;
* `0x06020000` is now **writable and readable** -- `A5A5A5A5` written and read
  back -- so the whole 128 KB is available.

**Texture slot 3 was unused.**

## What this unblocks, and what it does not

It removes the hard blocker. The widescreen path needs somewhere to put a map so
that block 1 stops being BG1's, and there is now room.

But **not directly**: a BG's `screenbase` field is five bits in 2 KB units, so
maps can only live in `0x06000000 .. 0x0600FFFF` -- VRAM_E's range. `0x06020000`
is out of reach for a map. `charbase` is four bits in 16 KB units and reaches
`0x0603C000`, so the new bank **can** hold tile data.

So the shape of the fix is: move **tile data** into VRAM_D, which frees low
VRAM_E blocks, which gives BG1 a new map, which frees block 1 for BG2's columns
32..63.

## The risk to validate

Slot 3 is unused **in this scene**. A later mission, faction or cutscene could
use it, and the failure would be missing unit textures rather than a crash. The
DMA capture in `docs/118` shows transfers to `06204000` and `06207800` but none
to `0686xxxx`, which is weak supporting evidence and not proof.

Before this is made permanent, texture-slot usage should be sampled across more
than one scene.
