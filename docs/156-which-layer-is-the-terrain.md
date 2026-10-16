# Which layer is actually the terrain

`docs/117` said the terrain is **BG2**. It is not. Disabling each engine A layer
in turn and measuring how much of the field changes:

```
BG0 (3D)  off ->   5.7%     the units and structures
BG1       off ->  92.8%     THE TERRAIN
BG2       off ->   1.3%     a minor overlay
BG3       off ->   0.0%     nothing visible at all
```

With BG1 off the ground vanishes completely and only the castle, the units and a
strip at the left edge remain. That is the terrain layer, and it is **BG1**.

## Why the earlier reading was wrong, and why this one is consistent

`docs/117` reasoned from priority, treating BG2's priority 1 as "the main
layer". On the DS a **lower priority number draws in front**, so the ordering
here is:

```
BG3 prio 0   frontmost
BG2 prio 1
BG0 prio 2   the 3D units
BG1 prio 3   backmost   <- terrain belongs here
```

Terrain is the thing everything else stands on, so it should be **backmost** --
the highest priority number. BG1 is exactly that, and the measurement agrees
with the reasoning once the reasoning is done in the right direction.

## What this does to the widescreen problem

The layers and their blocks (screen base in 2 KB units, 512x256 spans two):

| layer | prio | size | blocks | contributes |
| --- | --- | --- | --- | --- |
| BG1 | 3 | 512x256, base 1 | **1, 2** | **92.8%** -- terrain |
| BG2 | 1 | 512x256, base 0 | 0, 1 | 1.3% |
| BG3 | 0 | 256x256, base 2 | 2 | **0.0%** |

The terrain is BG1, and **BG1's columns 32..63 live in block 2**. Block 2 is
BG3's map -- and **BG3 draws nothing**.

So the widescreen path is far shorter than `docs/155` supposed:

* it does **not** need VRAM_D reassigned;
* it does **not** need tile data relocated;
* it needs **BG3 out of block 2**, and BG3 appears to be contributing nothing to
  begin with.

Then filling block 2 with the terrain's columns 32..63 gives BG1 a genuine
512-pixel-wide map -- double the current field of view -- through the layer that
actually draws the ground.

## What still has to be true

* **BG3 must be unused in general**, not just in this scene. 0.0% in one Free
  Play match is suggestive and no more; menus, campaign missions and other maps
  have not been checked, and `docs/155` recorded the same caveat about texture
  slot 3 for the same reason.
* **The streamer must produce those columns.** `docs/119` found the bound --
  `cmp sb, #0xa`, ten metatile columns of three tiles -- and the row stride
  `0x40`. Filling block 2 means extending what the builder emits, which is the
  remaining real work.

`docs/155`'s VRAM_D result stays valid and useful; it is simply not needed for
this, which is the better outcome.
