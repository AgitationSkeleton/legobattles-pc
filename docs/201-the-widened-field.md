# 201 — The widened field renders

Status: **working.** The window fills with coherent battlefield at every size
tested, drawn by the game's own renderer out of the game's own tile data.
`docs/reference/widescreen-terrain-working.png` (1280x720) and
`docs/reference/widescreen-surface-448.png` (the 448x192 surface).

    lit cols 0..447        the whole surface, no black bands

## What finally worked

The widened tile entries live in a **host array** that only the decoder reads.
`decode_text_line` serves hardware tile columns 0..31 -- the guest's own view --
from VRAM exactly as before, so the game's picture is untouched, and takes the
columns outside that range from `lb_widemap_tile()`.

Nothing in guest memory or guest registers is modified. That is the whole point,
and it is what two earlier designs got wrong.

## The two VRAM designs that failed, and why

**Writing the layer's second screen block in place.** The screen bases are
packed end to end -- BG2 at 0x0000, BG1 at 0x0800, BG3 at 0x1000 -- so a
512-wide layer's "second block" is the next layer's visible tilemap. Writing it
destroyed BG3.

**Relocating BG1's screen base.** This looked elegant: `func_0210BC2C` computes
the guest's own DMA destination from BG1CNT, so the guest's writes follow a
relocation and block 0 fills itself. It even worked well enough to draw real
terrain in the extras for the first time. But the destination is recomputed on
every call, so it sees whichever value the register holds at that instant, and
the write histogram shows the tilemap splitting across both bases:

    0800=18928     the old block, still being written
    2800=35712     the new one, also being written

**Nothing that mutates a register the guest also reads can be made safe here.**
That is the same race the earlier IO-register write lost, arrived at from the
opposite direction. Two sessions were spent choosing better blocks when the
mechanism itself was wrong.

Also settled along the way: there is no comfortable free block anyway. VRAMCNT
says banks A-D are all 3D texture (`mst 3`), engine-A BG is bank E's 64 KB, and
the screen-base field is five bits of 0x800 -- so every addressable block lives
in that one bank, with the guest using the low ones.

## The bug that hid all of this

`LB_BG_EXTEND` was never reaching the renderer. Asking the widener directly:

    [lb_wideslot] bg_extend=0 slot=-1 of 3 bgs

with the variable set. Its hook sits inside an `if (serve)` block in main.cpp
that the interactive path never reaches, so **no layer was decoded with the
extra columns at all** and everything written for them was read by nobody.

That single fact explains every symptom that had survived scrutiny: correct tile
entries, addresses matching what the renderer computes, `width_extra` genuinely
`0x100`, tile graphics resident at `16/16`. Every check passed because every
check was asking about data nothing read. Enabling the extension is now part of
this feature rather than a second switch.

## What is not done

* **Two thin seams**, at the boundaries between each extra and the native view.
  The left one is almost certainly the pre-existing left-edge artefact of
  docs/193 -- native columns 1 and 3, black on the top tile-row of each metatile
  -- which used to sit at the screen edge and is now framed by terrain on both
  sides, so it reads as an internal strip.
* **Buildings.** `func_02034B44` stamps building tiles over the terrain, gated
  on cell visibility; the host fill does not replicate it, so a building
  standing in the widened columns will be missing from ground that is drawn.
* **Fog.** The shroud is not applied to the widened columns, so they show
  terrain the player may not have explored. That is a gameplay leak, not a
  cosmetic one, and it is the next thing to fix.
* **BG2** is left alone; only BG1, the terrain layer, is extended.
