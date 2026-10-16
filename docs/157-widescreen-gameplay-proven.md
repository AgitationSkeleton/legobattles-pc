# Widescreen gameplay, proven end to end

The field renders at **448x192 with real terrain in the widened columns** and no
black bars. This is the first time gameplay field of view has actually increased
in this port.

## What it took

Three things, none of which needed the VRAM reassignment `docs/155` found or the
tile relocation it proposed:

1. **`LB_BG_EXTEND=1`**, a new hook. Upstream already has
   `nds_gpu2d_set_adaptive_bg_extend`, which lets the **furthest-back** text BG
   decode into the widened columns -- and nothing in the runner ever called it.
   Furthest back means largest BGCNT priority, which `docs/156` established is
   **BG1, the terrain**. The mechanism was written for exactly this case and was
   simply switched off.

2. **Terrain data in block 2.** BG1 is 512x256 at screen base 1, so its columns
   32..63 live in block 2. Filling block 2 gives those columns something to
   show. For the proof this was a copy of block 1.

3. **BG3 disabled.** Block 2 is also BG3's map, and BG3 is **frontmost**
   (priority 0). With terrain written into it, BG3 drew that terrain over the
   entire scene -- the whole frame washed green with the castle showing as a
   silhouette. Turning BG3 off removes it, and `docs/156` measured BG3's
   contribution as **0.0%**, so nothing is lost.

The middle step is the one that made the green wash legible rather than
mysterious: a frontmost layer sharing a map with a backmost layer will draw
whatever the backmost layer needs, on top of everything.

## The result

```
before:  448x192, left margin black 4608/4608     (letterboxed)
after:   448x192, left margin black    0/4608     (terrain)
```

and the capture shows grass, trees and rocky ground continuing out to both
edges, with the castle and units correct in the centre.

## What is still wrong, and why

There is a **visible vertical seam** about 96 pixels in on each side. Block 2
holds a *copy of columns 0..31* rather than the true columns 32..63, so the
terrain restarts at the seam instead of continuing. That is an artifact of the
proof, not of the approach.

Removing it means the tile streamer must produce the real columns 32..63. That
is `docs/119`'s work: the bound `cmp sb, #0xa` at `0203A114`, ten metatile
columns of three tiles each, and the `0x40` row stride. The map, the display
path and the layer arrangement are now all proven to carry it.

## Caveat carried forward

BG3 draws nothing **in this scene**. Menus, campaign missions and other maps
have not been checked, and this approach requires BG3's map block. If BG3 is
used anywhere, that scene needs the extension disabled or BG3 repointed -- and
`docs/155`'s freed VRAM_D is exactly where it could be repointed to, which is
what that finding is now for.
