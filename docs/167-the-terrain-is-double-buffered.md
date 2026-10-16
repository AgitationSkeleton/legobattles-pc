# 167 — The terrain is double-buffered, so there is no off-screen field to reveal

Status: **measured**. Supersedes the widening premise in docs/164 and narrows
docs/166.

## What this answers

Every widescreen attempt so far has produced the same symptom: the composite
grows to the requested width, and everything past roughly 256 px is black or
garbage. docs/162 blamed culling. docs/164 claimed a master FOV control and was
withdrawn by docs/166. docs/166 concluded the DS text BG caps terrain at 512 px
and treated that 512 as headroom still to be claimed.

That headroom does not exist. This doc measures why.

## Measurement

State: mid-battle, warped save, engine A driving the bottom screen.

Registers (engine A, `read_io` at width 16 — note `read_io` takes `width`, not
`size`, and a 32-bit read of 0x04000008 returns BG0CNT | BG1CNT<<16):

    DISPCNT_A = 00012F08   bgmode 0, BG0 = 3D
    BG0CNT = 0002   prio 2  charbase 0  screenbase 0  size 0  (256x256)
    BG1CNT = 4187   prio 3  charbase 1  screenbase 1  size 1  (512x256)  256-col
    BG2CNT = 4081   prio 1  charbase 0  screenbase 0  size 1  (512x256)  256-col
    BG3CNT = 0284   prio 0  charbase 1  screenbase 2  size 0  (256x256)

BG1 is the terrain (docs/156: 92.8% of field pixels) and it is a **512x256**
text BG — 64 columns of map, twice what the screen shows. That is the fact
docs/166 read as headroom.

Counting populated columns in the two 2 KB screen blocks says all 64 columns
carry a non-zero tile index. Read no further and you conclude there is a second
screen's worth of terrain sitting off-screen.

Counting *distinct* tile indices is what actually settles it:

    BG1 terrain   left block: 38 distinct tiles     right block:  1 distinct tile
    BG2           left block:  1 distinct tile      right block: 38 distinct tiles

and the two live blocks are byte-identical, row for row:

    BG1 left  row0:  29 00 1E0 12 00 1E0 12 00 1E0 12 00 1E0 12 00 1E0 12
    BG2 right row0:  29 00 1E0 12 00 1E0 12 00 1E0 12 00 1E0 12 00 1E0 12

The "populated" off-screen half is one filler tile repeated 32x32 times — 0x1F
for BG1, 0x64 for BG2. It is not terrain. It is the other side of a flip.

## What the hardware is being used for

BG1 and BG2 are a **scroll double-buffer**. Each is a 512-wide map used as two
256-wide buffers; the game writes the next frame's tiles into the idle half and
flips which half is shown. The pair alternates, which is why one is live while
the other is filler, and why the live halves match while a flip is pending.

This is consistent with everything already recorded and previously misread:

- docs/143's "scroll registers constant by design". BGnHOFS reads back 0 because
  it is write-only, and the game re-writes the scroll every frame. Writing
  BG1HOFS from the debug server and stepping a frame changes nothing visible:
  the guest overwrites it before the scanline renderer consumes it.
- docs/156's layer census. BG2 contributes 1.3% of pixels despite holding a full
  copy of the terrain, because its displayed window is the uniform *transparent*
  filler tile while its real content waits off-screen.
- The black bars. Past 256 px the compositor has nothing but filler to fetch.

## Consequence

**Only 256 px of terrain is materialized in VRAM at any instant.** Widening the
DS 2D path cannot show more field, at any width, because the extra field is not
in memory to show. The 448 and 1024 ceilings raised in the runner are real
ceilings on the *canvas*, and raising them was not wrong, but a wider canvas
over a 256-px-wide source is exactly the black bar the player sees.

So the honest statement of the ceiling, replacing docs/166's:

- Guest-side widening ceiling: **256 px**, set by the double-buffer, not 512 set
  by the text BG, and not 448 set by the runner.
- Vertical is worse still: the adaptive compositor is fixed at 192 lines
  (`kScreenHeight`), and a 96x96 map is 768 px tall (docs/15).

A 1920x1080 or 3440x1440 window showing the whole battlefield is therefore not
reachable by widening the emulated 2D engine. It requires a host-side field
renderer that reads the level map and tileset directly and draws the map at
arbitrary resolution, compositing the guest's units over it. The map format is
solved (docs/15, `tools/lb_map.py`), which is what makes that tractable.

## Retractions

- docs/166's "the DS text BG caps terrain at 512 px" is superseded. The cap is
  256 px, and the 512-wide BG is a double-buffer, not a wide field.
- `port/runner/src/lb_wide2d.inc`'s comment "extending a 256-wide text BG to 448
  is exactly TILING it: the map is 32 columns" is wrong about BG1. BG1 is 64
  columns. The comment reached the right behaviour (there is nothing real to
  fetch) from the wrong reason, which is how it survived this long.
- `port/game.toml`'s premise that the field is the touch screen is **correct**
  and is not retracted. A mid-session reading that the field was the top screen
  was wrong and is discarded: the debug server's `framebuffer` command takes an
  `engine` key that actually selects a **screen** (`"A"` -> 0 = top, `"B"` -> 1
  = bottom), so its captures are screen-indexed. POWCNT1 = 0x020F (bit 15 clear)
  routes engine A to the lower screen, and engine A is the one with BG0 = 3D,
  which docs/156 already tied to the units. Field = bottom = engine A. All three
  agree.

## Debug-server parameter names, since three of them cost measurements here

    read_mem     addr, len       (not size — size yields an empty read)
    write_mem    addr, value, width=16|32   (not len/hex — silently a no-op)
    read_io      addr, width     (not size)
    run_cycles   arm9=<absolute target>     (not cycles, and not a delta:
                                             a delta below the current count
                                             returns reached:true immediately)
