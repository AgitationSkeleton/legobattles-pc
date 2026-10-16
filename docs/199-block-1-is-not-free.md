# 199 — The tilemap fill works, and block 1 is not free

Status: **implemented, run, and it fails for a structural reason worth
knowing.** `LB_WIDEMAP` is off by default and the shipped port is unaffected.

## Everything the recipe needed, confirmed in a live battle

docs/196 worked the fill out of the cartridge; this confirms every input of it
against a running game (`screenshots/exp-viewok/probe.txt`):

    view   @02152034 -> 0224B448        func_02039A5C allocates 0x1B0 and stores it
    map    @02156CB0 -> 022BD838
    camera (136, 144) -> (296, 144)     panned right: X moved, Y did not
    map width +0x14 = 64 metatiles

and the fill itself reported, from inside the battle:

    [lb_widemap] on: map 022BD838 64x64 metatiles, split 440,
                     tables 022BDBD0/02283864

So the metatile grid at `map+0x34`, the width at `map+0x14`, the split threshold
at `0214B9D0+2` and the two tileset tables at `map+0x338`/`map+0x33C` are all
correct and readable. `src/lb_widemap.cpp` turns them into tile entries exactly
as `func_020A3478` does.

## And writing them corrupts the entire screen

The capture is unambiguous: vertical stripes of wrong tiles across the **whole**
field, not merely in the widened columns. The writes are landing on the visible
picture.

The BG control registers say why. Read in gameplay:

    BG1CNT 4187  ->  screen base 1  ->  0x0800
    BG2CNT 4081  ->  screen base 0  ->  0x0000
    BG3CNT 0284  ->  screen base 2  ->  0x1000

A 512x256 text background is two 32x32 screen blocks, the second at +0x800. So:

    BG2's block 1  =  0x0000 + 0x800  =  0x0800  =  **BG1's block 0**
    BG1's block 1  =  0x0800 + 0x800  =  0x1000  =  **BG3's block 0**

**The screen blocks are packed end to end with no gaps.** There is no spare
block behind either terrain layer; the "second block" of each 512-wide
background is the next layer's visible tilemap. Writing it destroys that layer,
which is exactly what the capture shows.

This retracts the central premise of docs/196 — *"the host can widen the field
by writing 24 tile columns the guest leaves empty, without touching its staging
buffers, fill loop or DMA"*. The columns are not empty and they are not the
guest's to leave: they belong to another background.

It also explains something that was previously only described. The port's
decoder reaches block 1 by design (`(sx & width_extra) << 3` adds 0x800 for
`sx >= 256`), so when the field is widened it reads **BG3's tilemap as though it
were terrain**. That is a better account of the widened columns' contents than
"the tiles out there are empty".

## What would actually work

Relocation, not filling. There is far more BG VRAM than the three screen blocks
at 0x0000..0x1800 occupy, so:

1. pick a free two-block region,
2. point `BG1CNT`'s screen base at it,
3. each frame, mirror the guest's block 0 into the new block 0 and write the
   widened columns into the new block 1.

The guest keeps writing its own base and never notices. Two things to establish
first: whether the guest rewrites `BGxCNT` every frame the way it rewrites
`DISPCNT` (docs/190 — if so the screen base has to be reapplied on the same
cadence, which the vblank hook already runs at), and which BG VRAM is genuinely
unused, from `VRAMCNT` rather than from assumption.

## Cost

Measured at 38.7 fps with the fill on against 30-36 in neighbouring runs, so
writing 624 entries a frame through `nds_video_write` is not obviously
expensive. That number is one sample and the scenes differ; it is recorded only
to say the approach is not disqualified on cost.

---

# Relocation attempted: two real bugs fixed, and it still renders wrong

`LB_WIDEMAP` is now a STAGE, not a flag, because the first version killed the
emulator with nothing in the log to say which call did it:

    LB_WIDEMAP=1   compute and write VRAM, leave BGxCNT alone (screen unchanged)
    LB_WIDEMAP=2   also repoint BG1's screen base at the relocated blocks

Both are off by default and `default_check` confirms the shipped build is
untouched: correct terrain, correct HUD, window filled, no corruption.

## Two bugs worth keeping

**`nds_video_write`'s width is in BYTES, not bits.** `bus.cpp` calls it with
4/2/1; this passed 32 and 16. That made `addr &= ~(width - 1)` align the address
to 32 bytes and then `store()` write 32 bytes out of a four-byte value — an
overflow that killed the emulator on its first frame in a battle. It is an easy
mistake to make from the call site alone, since every other width argument in
this project's tooling (`read_mem`, `read_io`, `write_mem`) is in BITS.

**The fill must be gated to the field scene.** Without that it also ran through
the menus, where the view and map pointers are stale but still look like
main-RAM addresses, so it computed a nonsense VRAM base and wrote to it. The gate
is display mode 1 with BG0 as the 3D layer and BG1 enabled — DISPCNT `00010F08`.
The menus have their own VRAM layout and nothing here means anything outside a
battle.

## And it still does not work

With BG1 repointed to block 3 the field renders as a fine dither of wrong tiles.
Mirroring every frame instead of only on camera movement — on the theory that
animated terrain rewrites the tilemap continuously — made it worse, not better:
the layout collapsed entirely, with the HUD filling the main window.

Since stage 1 performs exactly the same VRAM writes and looks correct, the
writes themselves are fine. What is wrong is the assumption underneath the
mirror: **that BG1's visible tilemap lives at `screen_base * 0x800`.**

There is direct evidence against it. Reading the character bases from the same
registers gives BG2 char base 0 and BG1 char base 1, so BG2's tile GRAPHICS
would start at 0x0000 — exactly where BG2's screen block was placed. Those
cannot both be true, so at least one of the two derivations is wrong, and the
mirror is built on the same arithmetic.

The thing to settle next is not another guess at the layout: it is
`func_0210BC2C`, which returns the base the scroll callback DMAs into
(`func_0210CA60(src, offset, len)` computes `dest = func_0210BC2C() + offset`).
That is the address the guest itself considers its tilemap to live at, measured
rather than derived, and it can be read straight out of the running game.

---

# Render-time relocation: the mechanism works, the destination does not

Three things were wrong at once, and each hid the next.

## 1. The build had been silently failing

`apply_hooks.py` and `lb_widemap.cpp` both had `\n` inside C string literals
turned into REAL newlines by a heredoc — the escape-mangling trap this project
has a standing note about. The build exited non-zero for several iterations
while the test command was

    ./build.sh | grep -E "^  ok|error:" && cp ... && run

and `grep` returning 1 on no match short-circuited the `&&`, so the deploy never
happened and every "test" ran a STALE BINARY. The lesson is not about heredocs,
which is already written down; it is that a build step must be checked by its
EXIT CODE, never by grepping its output.

## 2. Writing BGxCNT never worked

A counter in the vblank hook settles it. Over 600 consecutive frames:

    [lb_widemap] BGCNT@0400000A: ours 0, guest's 600 (cur block 1)

The register read back as the guest's base **every single time**. The game
rewrites BGxCNT every frame exactly as it rewrites DISPCNT (docs/190), so an IO
write at vblank is outrun before any scanline is drawn.

And it was worse than a no-op: the write landed transiently, so some scanlines
composed from the relocated base and others from the guest's. **That is the
"fine dither of wrong tiles" this document previously blamed on stale mirror
data.** It was a timing fault, not a data one.

The fix is the pattern already in the codebase for exactly this: `LB_BGMASK`
edits DISPCNT inside `render_engine_line`, where it cannot be outrun. Hook 0q
now sets BG1's screen base there.

## 3. The "free" VRAM was not free

`LB_VRAMLOG=1`, a per-2KB-block histogram of guest VRAM writes, in a battle:

    0000=48732  0800=45200  1000=26848  1800=9472  2000=8704

The three screen blocks are 0x0000/0x0800/0x1000 as expected — but the guest
also writes **0x1800 and 0x2000**, which is precisely where the first relocation
attempt put its blocks. So that attempt was overwriting live data, and the
speckle it produced was never a tilemap problem either.

## Where it stands

With the relocation applied at render time and the blocks moved past the
measured writes, **the left portion of the field renders correctly** — real
terrain, through the game's own renderer, out of the relocated block. The
mechanism is sound.

The right portion is still corrupt, and the boundary sits at about native column
132 (tile column ~16) regardless of whether blocks 5+6 or 6+7 are used. That it
does not move with the block choice says the remaining fault is not the
destination pair.

Two candidates, untested: the mirror is racing the guest's DMA (this copies at
vblank; the guest DMAs later in the frame), or 0x2800..0x3800 holds tile
GRAPHICS rather than being free — the histogram only sees CPU writes through
`nds_video_write`, and a hardware DMA of level assets would not appear in it at
all. The second would also explain why the corruption tracks terrain type
rather than screen position.

The honest next step is to stop choosing addresses by elimination and read the
bank mapping: `VRAMCNT` says which banks back engine-A BG at all, and a DeSmuME
savestate of a real battle would show what is actually resident in each block.

---

# The guest base was being inferred, and the inference ate itself

## VRAMCNT, measured rather than assumed

    A=83 B=8B C=93 D=9B    mst 3 -- all four are 3D TEXTURE banks
    E=81 ofs 0             mst 1 -- engine-A BG at 0x06000000, 64 KB
    F=91 ofs 2             mst 1 -- engine-A BG, second window
    G=83  H=03(off)  I=81

So engine-A BG is bank E's 64 KB. The screen-base field is five bits of 0x800,
so every addressable screen block lives inside that bank -- which is why picking
blocks by elimination kept landing on live data.

## The bug that survived three block choices

The render-time hook mutates `u.bgcnt[1]`, and that Unit **is** the emulator's
register state -- so after the first frame `nds_io_debug_read` returns OUR base,
not the guest's. `lb_widemap` was inferring the guest's base as "whatever the
register says when it is not ours", and that inference then froze on a menu
frame's value of **0**.

Block 0 is BG2's tilemap. So the mirror was faithfully copying **BG2's tilemap
into BG1's relocated block**, every frame, and no choice of destination block
could ever have fixed it. The hook now publishes the base it overwrites
(`lb_widemap_guest_base`) and the fill reads that.

`extern "C"` on that symbol is load-bearing: the declaration sits inside
gpu2d.cpp's anonymous namespace, so a plain `extern` declares an *internal*
symbol and the link fails with ``(anonymous namespace)::lb_widemap_guest_base``.

## Where it stands

The entries are now demonstrably right:

    col 0 (abs 49, mx 16/64) -> entry 01E0 at 06003300 ok
    col 1 (abs 50, mx 16/64) -> entry 0012 at 06003302 ok

and 0x3300 is exactly the address the renderer computes for hardware column 256
(`row_base 0x2B00 + ((sx & 0xF8) >> 2) + ((sx & 0x100) << 3)`). The left of the
field renders real terrain out of the relocated block.

The right of the field is still a repeating wrong-tile pattern, and the widened
columns still read black despite `LB_BG_EXTEND=1`. Next candidate, untested:
`line.width_extra`. If it is not 0x100 for a 512x256 background then
`(sx & width_extra) << 3` contributes nothing, hardware column 256 folds back
onto block 0 entry 0, and both symptoms follow from that one value.

## Build discipline

Two runs' worth of "results" came from a stale binary again, for two new
reasons, both now fixed in how the commands are written:

* a build must be checked by **exit code**, never by grepping its output;
* the emulator must be **killed before** the build, not after -- otherwise the
  link fails with `cannot open output file nds_runner.exe: Permission denied`
  while the compile succeeds, which looks like success in every other respect.

---

# The widened columns render real terrain

`lit cols 0..447` — for the first time the whole 448-column surface is drawn,
and the left and right extras contain real map: grass and a path on one side,
a sand shore on the other (`docs/reference/widemap-extras-working.png`).

Three faults had to come off, in this order, and each masked the next.

## `LB_BG_EXTEND` was never reaching the renderer

The decisive probe was asking the widener what it had chosen rather than
reasoning about it:

    [lb_wideslot] bg_extend=0 slot=-1 of 3 bgs

With `LB_BG_EXTEND=1` on the command line. The hook that calls
`nds_gpu2d_set_adaptive_bg_extend` sits inside an `if (serve)` block in main.cpp
that the interactive path never reaches, so **no layer was ever decoded with the
extra columns** and everything written into the second screen block was read by
nobody.

That single fact explains every symptom that had survived: the extras were black
while their tile entries were correct, their addresses matched what the renderer
computes, `width_extra` really was `0x100`, and the referenced tiles' graphics
were resident (`nonzero 16/16`). Each of those checks passed because each was
asking about data nothing was reading.

Relocating a layer and then not extending it is never useful, so `lb_widemap`
now enables the extension itself rather than depending on a second switch.

## Two hypotheses that were wrong, and cheap to kill

* **`width_extra`** was the prime suspect at the end of the previous session —
  `line.width_extra = (line.cnt & 0x4000u) ? 0x100u : 0u`, and BG1CNT has bit 14
  set, so it is `0x100`. Wrong, settled in one grep.
* **Tile-graphic residency** — the theory that streamed tiles outside the view
  were never uploaded, which would render transparent. Also wrong: every
  extension tile reported `nonzero 16/16`.

## And the mirror really did have to go

With the extension finally visible, the picture inverted: the extras came out
correct and the NATIVE MIDDLE turned to garbage. That is what clobbering a live
block with a stale copy looks like, and it confirms the conclusion Ghidra gave
and this document then talked itself out of: `func_0210BC2C` reads BG1CNT for
the DMA destination, the render-time hook mutates the register the emulator
actually holds, so **the guest fills the relocated block itself.** The mirror
could only ever overwrite that with older data. Removed for good.

## What is left

A garbage band remains in the middle-right of the native region, roughly native
columns 104..234. It is not the extension (block 6 entries 12..19 are unwritten
but map to hardware columns 352..415, which are never displayed) and it is not
the mirror, which no longer exists. Next step is to measure the band's exact
column range off the surface rather than by eye, and to check whether the
guest's 832-entry DMA is landing where its 1024-entry block is being read.
