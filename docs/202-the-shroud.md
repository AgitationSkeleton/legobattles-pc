# 202 — The shroud, decoded

Status: **RETRACTED.** The central claim below -- that nothing is shrouded in
these matches -- is wrong. Free Play always has fog unless the no-fog cheat is
enabled, and a capture of the very frame these probes read is wall-to-wall grey
cloud.

The mask at 0x02151F28 really is empty; it is just not the record that drives
the picture. func_0203420C refills it from entity extents every frame, clamped
to 0x20 x 0x18. The fog is drawn by BG2 from a 3x2 metatile, and BG3 tile 0x1F
is a transparent marker saying where it goes. See docs/203, which supersedes
this document. The reading of func_0203A4B4 and func_020EEB1C below is still
accurate and is what docs/203 builds on -- including the "100", which turns out
to be a staging value caught before expansion.

## How the game draws fog

`func_0203A4B4(view, force)` is the shroud pass. Ghidra makes it short enough to
quote:

    for (ty = 0; ty < 0x18; ++ty)                  // 24 tile rows
      for (tx = 0; tx < 0x20; ++tx)                // 32 tile columns
        if (func_020EEBF4(fog, tx, ty))
          *func_0203D90C(tx, ty + parity) = 100;   // tile index 100
    func_020EEB1C(view + 0x164, fog[0x08], fog[0x0C]);
    func_02110758(fog[0x10], view[0x174], view[0x168] * view[0x170]);
    view[0x67] = 1;

So the shroud is **tile index 100**, written into a staging buffer, for every
tile the visibility test flags. The two calls afterwards copy the mask to
`view+0x174` so the next frame can skip the work when nothing changed — the
function opens by comparing `fog[0x10]` against `view[0x174]` byte for byte and
returning early when they match.

`view + 0x67` is the second dirty flag. The scroll callback checks `+0x66` and
`+0x67`, and DMAs two staging buffers: **`+0x66` is the terrain (BG1) and
`+0x67` is the shroud (BG2)**. That settles what the second layer is for.

## The visibility test and the destination

Both are one-liners:

    bool func_020EEBF4(fog, tx, ty) {              // set means SHROUDED
        row = *(u32*)(*(u32*)(fog + 0x14) + ty * 4);
        return row[tx >> 3] & (1 << (tx & 7));
    }

    u16* func_0203D90C(x, y) {                     // where that tile entry goes
        return rowtable[y] + x * 2;
    }

The mask is **screen space**: one bit per tile over a 32x24 grid, addressed
through a row-pointer table at `fog+0x14`. That is the field that moved when
`LB_VIEW_WIDTH` resized the object, which is now explained — the row table is
reallocated because the rows get wider.

## LB_VIEW_WIDTH really does widen the mask

Read from a live battle with `LB_VIEW_WIDTH=448`:

    fog: stride 7 width 56 height 24 rowtable 022452A0

56 columns, 7 bytes per row. So the earlier verdict that the patch "changes
nothing" was about its effect on the PICTURE, and this is the part it does
change. The draw loop above is still hard-coded to `tx < 0x20`, so a wider mask
alone was never going to draw wider fog.

## Why the shroud cannot be validated here yet

RETRACTED -- see the note at the top. This section reasons from an empty mask to
"nothing is shrouded", and the conclusion does not follow: the mask is a
per-frame scratch buffer, not the explored record. Both candidate explanations
offered below (a Reveal Map cheat, a Free Play fog option) were also wrong.

Every bit of the mask is clear:

    row  0: ........................................................
    row  8: ........................................................
    row 16: ........................................................

**Nothing is shrouded in these matches.** That is not a bug in the widening —
it means the terrain the widened columns show is correct as drawn, because there
is no fog anywhere on screen to omit.

It also means the "gameplay leak" this project has been worrying about is not
demonstrated. Before claiming the widened columns handle fog correctly, a match
that actually HAS fog is needed. Two candidates for why these do not, neither
confirmed:

* the port carries a **Reveal Map** cheat (`lb_overlay.cpp`, flags[0], code
  SKQMXPL, commented "Verified live: setting flags[0] revealed the whole
  minimap"), and the seeded profile is a fully-unlocked save;
* the Free Play rules screen, which the route clicks straight past with
  Continue, may carry a fog option.

## What extending it will require

The mask is screen space, so it has no data for the LEFT widened columns at all
— those are hardware tile columns -12..-1, and the mask starts at 0. The right
columns (32..43) do fall inside a 56-wide mask, if the guest's fog update
populates them rather than merely having room for them.

The clean answer is probably not to extend the screen-space mask but to find
what GENERATES it each frame — there must be a map-space explored/visible record
behind it, since fog persists as ground is explored. That record would give
visibility for any tile, in the same way the metatile grid gave terrain for any
tile (docs/196). Not yet located.
