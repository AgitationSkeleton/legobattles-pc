# Who fills the field's tilemap

> **Corrected by [docs/110](110-setlayerscroll-and-the-508.md).** Every argument
> below that reasons from the BG scroll registers being "pinned at 508" is
> unfounded: 508 is a hardcoded constant argument to `SetLayerScroll`, written
> every frame by `0203891C`, so the registers never carried any information
> about the camera. The conclusion that the field scrolls by rewriting the
> tilemap survives and is in fact strengthened. **[docs/111](111-the-d-pad-scrolls-the-view.md)
> further withdraws this document's retraction of the D-pad**: the D-pad does
> scroll the view, measured in pixels; the registers simply never show it.
> **[docs/114](114-the-engines-were-inverted.md) withdraws a banner that briefly
> appeared here**: docs/113 claimed this document measured the top screen. It
> did not. The field is engine A, its BG1 and BG2 really are 512x256, and this
> document's conclusion -- that widening means filling block 1 -- stands.

Groundwork for real widescreen (`docs/107`): the field can only show more of the
world if the guest writes more tile entries. This is how those writes happen.

## The tool: `LB_WATCH`

`bus.cpp`'s watchpoint ranges were hard-coded firmware-debugging addresses, so
answering "which code writes these bytes?" for anything else meant editing and
rebuilding. There is now an env-configurable range:

    LB_WATCH=6000000:6002000        # hex lo:hi, ARM9 addresses

When set it **replaces** the defaults rather than adding to them. That matters:
the shipped `0x04000130`-`0x04000138` range is busy enough to flush the 512-entry
ring before a capture can be read, which is exactly what happened on the first
attempt -- the tilemap writes were real and had already been overwritten.

This is how the answer below was found. Scanning `gpu2d`'s address region for
`0x06xxxxxx` literals found nothing, because nothing addresses VRAM literally.

## The answer: DMA, from a VRAM transfer job

With the watch pointed at the BG map region and a Free Play match warped into
via `tools/lb_warp.py`, every one of 512 captured writes came from a single PC:

```
pc=0x01FF8260  x512  width=2  addr 06000A80..06000E7E
```

`0x01FF8260` is in **ITCM**, which is why it never appeared in a main-RAM scan.
Breaking there (`run_to_pc`, serve mode) and reading registers:

```
pc=01FF8260  lr=01FF823C  sp=027E3668
r0=00000080  r1=000000D4  r2=040000B0  r3=040000D4
```

`r2` and `r3` are **DMA0 registers**. So the ITCM routine is a DMA kick, and the
writes the watch attributes to it are the DMA transfer itself -- the CPU program
counter simply happens to be there. The tilemap is filled by DMA, not by stores.

The stack at that point holds the logical callers:

| return address | function |
| --- | --- |
| `0x0210FE38` | `func_0210FDD8` (112 B) |
| `0x0210CEC4` | `func_0210CE80` (96 B) |
| `0x0204F1BC` | **`func_0204EFBC`** (756 B) |

`func_0204EFBC` is the game-level one. It switches **32 ways** on
`*(u32*)(r1 + 0x10) - 0xF`, which is a job-type dispatch, and it has exactly one
caller, `func_0204E73C`. That matches the `Battles::VramTransferData`,
`Battles::NNSVramTransferData` and `Battles::GXVramTransferData` classes in the
RTTI: **VRAM writes are queued as transfer jobs and executed by DMA.**

## What this means for widening

The field's tile entries are produced by whatever enqueues these jobs for the
visible map window. To widen the view, that producer must be asked for a wider
column range -- the hardware already allows it, since BG1 and BG2 are 512-column
maps and only 448 columns are needed (`docs/107`).

**Next:** find the enqueuer. `func_0204E73C` is the single caller of the job
dispatcher and the way up to it. The question to answer there is where the
column span comes from, because that span is the field of view.

## The full chain, traced with LB_WATCH

Watching the **job object itself** (`LB_WATCH=23DFA80:23DFAC0`) rather than its
destination names the producer directly. The job at `0x023DFA88` is a
**`Battles::GXVramTransferData`**, identified by its vtable `0x02148968`, linked
through an `SListObjectItem` at `+0x20`:

| offset | value in the captured job | meaning |
| --- | --- | --- |
| `+0x00` | `0x02148968` | vtable, `GXVramTransferData` |
| `+0x10` | `0x21` | the type `func_0204EFBC` switches 32 ways on |
| `+0x14` | `0x40` | length |
| `+0x18` | `0x022AE9AC` | source |
| `+0x20` | `0x02148A50` | `SListObjectItem` vtable -- it is queued in a list |

287 writes landed on it, from a cluster of small constructors and setters
(`func_0204DCE0` writes `+0x18`, `func_0204DE50` writes `+0x10`, and so on).
Climbing their single-caller chain:

```
func_0204E0E0   <- 19 callers, one of them func_02038890
  func_0204E318
    func_0204E3CC
      func_0204D988  -> builds the GXVramTransferData
        ...                       (queued)
func_0204E73C   drains the queue at VCOUNT
  func_0204EFBC   32-way dispatch on job type
    ITCM 0x01FF8260  DMA kick -> the tilemap
```

**`func_02038890` is among `func_0204E0E0`'s 19 callers**, and that is the
function already identified as the field's layer setup -- it writes BG3CNT at
`0x0400000E` with `0x284`, matching the live register read from a match
(`docs/107`). `func_0203B7FC`, in the same field region, is another caller.

So the producer of the field's tile data is reached, and the parameters passed
to `func_0204E0E0` -- source, destination and length -- are where the extent of
the upload is decided.

**Next:** read `func_0204E0E0`'s signature and find where its length and
destination come from in the field's per-frame path. That extent is the field of
view; widening it is what puts real map in the extra columns, and nothing
host-side can substitute for it.

## The headroom, in tiles

Worth stating in tile units, because it sets the target precisely:

| thing | pixels | 8px tiles |
| --- | ---: | ---: |
| BG1 / BG2 tilemap (size bits = 1) | 512 x 256 | **64 x 32** |
| what the DS displays | 256 x 192 | 32 x 24 |
| what a 448-wide window needs | 448 x 192 | **56 x 24** |
| the map itself (`docs/data/maps.tsv`) | -- | 96 x 96 or 64 x 64 |

The map is far larger than the tilemap, so the game necessarily **streams**: it
keeps a sliding window of the 96x96 map inside a 64x32 tilemap and rewrites
columns as the camera crosses tile boundaries.

**56 of the 64 available columns is what widescreen needs, and the hardware
already provides 64.** Nothing has to grow. The columns beyond the displayed 32
are simply not kept current -- they hold the window from an earlier camera
position, which is exactly the mismatched, repeating terrain that appeared when
the compositor was told to read them.

So the change is not "make the tilemap bigger" but "keep more of it current":
the streamer must refresh ~56 columns around the camera instead of ~32 plus a
margin.

## Where that decision lives

`func_0204E0E0(a, b, c, d, e, src, g)` takes seven arguments -- four in
registers, three on the stack -- and `[sp+0x2c]` is the source pointer, passed
to `func_0204DCE0`, which is the setter that writes the job's `+0x18`.

Two callers sit in the field's own code: `func_02038890`, the layer setup, and
**`func_0203B7FC`** (1748 bytes, called from `func_020E34BC` in the UI region),
which is the better candidate for the per-frame path.

**Next:** find the loop that walks tile columns for the camera window, and the
constant that bounds it. That bound is the field of view.

## Measured: the game keeps 32 tile columns current, out of 64 it has

Sampling the tilemap watch across 30 windows of 20M cycles in a live match, then
mapping byte addresses to tile coordinates:

```
screen blocks touched: {1: 512}
tile columns touched: 32  -> [32 .. 63]
tile rows touched:    16  (10 .. 25)
```

**A 512-pixel-wide text BG is two 32x32 screen blocks, not one 64-wide grid.**
Entry index is `row*32 + (col%32)`, plus 1024 entries for columns 32..63. Reading
it as `row*64 + col` -- which I did first -- smears every write across both
blocks and reports "all 64 columns touched", the opposite of the truth. The
corrected mapping shows a single 32-column block being refreshed.

So the streamer keeps **32 columns** current -- exactly the displayed width --
inside a tilemap that has room for **64**. The other block holds the window from
an earlier camera position. That is precisely the mismatched, repeating terrain
that appeared when the compositor was told to decode past the viewport
(`docs/107`), and it explains it exactly: those columns are not uninitialised,
they are *stale*, which is why they looked like plausible terrain in the wrong
arrangement rather than noise.

**The target is now exact: refresh 56 columns instead of 32.** The destination
already exists, the transfer path already exists (`func_0204E0E0` and below),
and nothing needs to grow.

## Method note

The first two attempts at this measurement were both wrong and both looked
plausible:

* watching the tilemap's *destination* named an ITCM DMA routine, not the code
  that decided anything;
* mapping entry indices as `row*64 + col` reported all 64 columns refreshed,
  which would have meant the data out there was valid and the corruption
  unexplained.

Watching the **job object** rather than the memory it eventually writes, and
using the hardware's actual screen-block layout, is what produced an answer that
matches what is on screen.

## Negative result: the terrain upload was not caught in the act

`func_0203BDF8` -- the call whose source advances by `0xC0` (192 bytes = 96
entries, the map's tile width) and whose length is `count << 6` -- **did not
execute** in either test:

* warped into a match with a stationary camera, 3M rounds: not reached;
* after dragging across the field to try to scroll, 3M rounds: not reached.

Two readings fit and neither is established: the terrain is uploaded once at map
load (before the warp finishes) and only re-uploaded when the camera crosses a
tile boundary, or the drag did not scroll at all -- LEGO Battles may scroll from
the screen edge or the minimap, and a drag on the field is plausibly a box
selection.

The other call site, `func_0203BE8C`, has a **fixed** length of `0x680`. Its
source buffer at `0x021530A0` was read live and holds 832 entries with only
**two distinct values**, almost all `0x001F` -- a near-uniform overlay, not
terrain. So the 832 = 32x26 arithmetic, while it fits, describes that overlay
and **must not** be assumed to describe the terrain window.

What stands: the refresh span measurement (32 columns kept current out of 64)
and the traced transfer path. What does not: any claim about which constant
bounds the terrain upload. Finding that needs the camera actually moving, which
means finding how this game scrolls first.

## Measured with the camera moving: 32 columns, one screen block, block 1 never written

The earlier measurement was taken with a stationary camera, which left open
whether a scrolling camera refreshes more. It does not.

**The camera scrolls with the D-pad.** Dragging on the field does not scroll it
(that is presumably selection), which is why the earlier attempt to provoke the
streamer failed and why `func_0203BDF8` was never caught -- it is **not** the
terrain upload, and the guess that it was is withdrawn.

Holding Right through twelve 25M-cycle windows, with `LB_WATCH` on the BG map:

```
blocks touched:   {0: 6144}
distinct columns: 32   ->  [0 .. 31]
writer:           pc = 0x01FF8260  x6144   (the ITCM DMA kick)
```

**Every write lands in screen block 0. Block 1 -- columns 32..63 -- is never
written at all**, scrolling or not.

So the game configures a 512-pixel-wide BG but fills only one 32x32 block and
relies on the hardware's 256-pixel wrap for scrolling. The second block holds
whatever was there at load and is simply never used.

That is the sharpest available statement of the widescreen blocker:

* the extra columns a widened view needs come from **block 1**;
* block 1 contains **no map data at any time**, so no compositor change can
  reveal terrain there -- this is why extending it produced stale tiles
  (`docs/107`), and the same result would follow from any host-side approach;
* making the view genuinely wider therefore requires **filling block 1**, either
  by extending the guest's streamer to 56 columns across both blocks, or by the
  port generating those entries itself from the map.

The second option is newly plausible: `docs/data/maps.tsv` has every map's
dimensions and the terrain planes are already parsed by `tools/lb_map.py`, so the
port knows what tile belongs at each map coordinate. What it would still need is
the camera's map position each frame, which is the one piece not yet located.

## Hunting the camera by RAM diff: method, and a warning

With D-pad scrolling available, the camera can be searched for by differencing
main RAM across known inputs. Two passes, and the first one was a trap.

**Pass 1, monotonic increase (WRONG).** Snapshot, scroll Right, snapshot, scroll
Right, snapshot; keep values that rose both times. 50 candidates, with a tight
cluster at `0x021658B8`, `0x021658D4`, `0x02165B0C` moving `65536 -> 76800 ->
88576` in lockstep -- and `65536` is exactly `16.0` in 20.12, which made it look
like a camera starting at a round position.

**It is a clock.** Pressing Left, Down and Up each moved it by the *same*
`+11264`. A monotonic filter cannot distinguish a coordinate from a timer,
because a timer satisfies it perfectly. The discriminator has to be that
**opposite inputs produce opposite movement**.

**Pass 2, direction-reversing.** Right for a fixed duration, then Left for the
same, keeping values that moved one way and then back, symmetrically, returning
near their start. That gives 835 candidates unfiltered; constraining to plausible
map coordinates (a 96-tile map is 768 px, so under ~3.2M in 20.12) and requiring
the two deltas to match within a quarter leaves **32**.

Several reverse identically across different addresses, which is what a value
copied into per-subsystem state looks like:

```
0229439C  1486 -> 157 -> 1511      022AE830  1486 -> 157 -> 1511
021943F4  1486 -> 157 -> 1511      022B4DE4  1487 -> 156 -> 1512
0229804C  1486 -> 157 -> 1511      022B505C  1487 -> 156 -> 1512
```

**None is confirmed.** The magnitudes do not read as 20.12 pixels, and assigning
one on the strength of a shape is exactly what the clock above punished. The
next step is to watch one of these addresses with `LB_WATCH` and read the writer,
which names it instead of guessing -- the same move that worked for
`projectile+0x164` in `docs/95`.

## The candidate slot is scratch, but its writer is scroll bookkeeping

Watching `0x0229439C` -- one of the 32 direction-reversing candidates -- through a
40M-cycle Right press captured **103 writes from a single PC, `0x020286D8`**.

The slot itself is **not** a camera: its values alternate between two rising
series (`1422/81`, `1423/82`, `1424/83`), so it is a scratch location reused
within a frame. Naming it a coordinate on the strength of the RAM diff would
have been wrong, which is the third time this session that a watch has corrected
a diff-shaped guess.

The **writer** is the useful part. `0x020286D8` is inside **`func_02028590`**
(336 bytes, one caller `func_02028B3C`), which opens:

```c
func_02028590(obj /*r0=sl*/, pos /*r1*/) {
    if (obj[1] != 0) return;
    r5 = pos - obj[0x2C];        /* movement since the stored position */
    if (obj[0x34] == 0) return;
    r4 = obj[8];
    r6 = obj[0x34][0xC];
    ...
}
```

`pos - obj[0x2C]` is **how far the view has moved since the last update**, which
is exactly the quantity a column streamer needs. And `0x02028xxx` is the same
region as `func_02028890` (the field's layer setup, `docs/109` above) and
`func_02028B7C`, which is one of `func_0204E0E0`'s 19 callers -- the VRAM
transfer queue.

So the streamer is in this region, `obj+0x2C` holds the previous position and
`obj+0x34` a sub-object it walks. **`obj` itself is the layer/scroll state**, and
finding what is passed as `r0` from `func_02028B3C` names it.

That is the next read, and it is one function away from the camera position this
thread has been looking for.

### RETRACTED: `func_02028590` is animation timing, not scroll bookkeeping

The section above reads `pos - obj[0x2C]` as "how far the view has moved since
the last update" and calls `0x02028xxx` the streamer. **Both are wrong.**

`func_02028B3C`'s only caller supplies the argument:

```c
func_020E4D3C() {
    func_0200DC14();
    func_020E127C();
    g = 0x021581A4;
    func_02028B3C(g[0x18]);   /* the argument */
    g[0x18] += 1;             /* ...is then incremented by one */
    ...
}
```

`g[0x18]` is **a frame counter**. So `func_02028590(obj, frame)` computes
`frame - obj[0x2C]` = *frames elapsed since this object was last updated*, and
the list it walks is animated objects, not BG layers. The region does per-frame
animation timing.

The mistake was reading `a - b` as a spatial delta because a spatial delta was
what I was looking for. A subtraction has no units in the disassembly; the
caller supplies them, and I had not read the caller.

**What survives:** `0x0229439C` is scratch (the alternating values, independently
established); the tilemap is written only in block 0, 32 columns, by the ITCM DMA
(measured twice, with and without camera motion); the camera scrolls with the
D-pad.

**What does not:** any claim that `0x02028xxx` is the tile streamer, and any
progress toward the camera position. That hunt is back to the 32 candidates from
the RAM diff, minus this one.

### An honest assessment of this thread

Four techniques have now been spent on the camera: static scanning, monotonic RAM
diff (found a clock), direction-reversing RAM diff (32 unresolved candidates),
and watch-the-writer (found animation timing). Each was reasonable and each cost
an iteration to disprove.

The remaining approach with the best odds is to **watch the BG scroll registers**
themselves. `func_0202D1B4` writes `BGnHOFS/VOFS` from arguments (`docs/107`),
so breaking there during a D-pad scroll gives the scroll value the hardware
actually receives, and its caller chain is the camera by construction rather
than by inference. That is one experiment, and it is bounded.

## Harness lesson: `run_to_pc` misses intra-bank calls, so "not reached" is weak evidence

Watching `0x04000010`-`0x04000020` through a D-pad scroll captures the field's
scroll writes plainly:

```
pc = 0x0202D27C -> 0x04000018  x22      (BG2)
pc = 0x0202D250 -> 0x04000014  x22      (BG1)
pc = 0x0202D2A8 -> 0x0400001C  x2       (BG3)
pc = 0x01FF82E0 -> 0x04000010/14/18     (ITCM, once each)
```

Those three PCs are **inside `func_0202D1B4`** -- and `run_to_pc` on
`0x0202D1B4` in the same situation **never broke**.

The explanation is structural: the recompiler emits **direct C-to-C calls within
a bank**, so an intra-bank call never passes through `runtime_dispatch`, and a
break on the callee's entry PC is never tested. `run_to_pc` sees only dispatched
entries -- cross-bank calls, indirect branches, returns.

**This weakens several negative results recorded earlier**, which must not be
read as "this code did not run":

* `docs/98` #5 and #6 -- `func_02089DE4` "does not execute" during gameplay or on
  input;
* the finding above that `func_0203BDF8` "is not the terrain upload";
* the first attempt at `func_02089DE4` in this document.

Each of those is now only "not reached **through dispatch**", which is a much
weaker claim. A watch on the memory the function writes is the reliable test, and
is what produced every solid result in this document.

The retractions that came from *watches* -- block 1 never written, the animation
timing, the scratch slot -- are unaffected.

### RETRACTED: the D-pad probably moves a cursor, not the camera

"The camera scrolls with the D-pad" was concluded from the framebuffer hash
changing after each direction press. That is weak evidence: **anything** moving
on screen changes the hash.

Measured properly, through the scroll registers themselves:

```
BG1 (0x04000014):  508 -> 511   over 40M cycles of Right, then Left  (3 px)
BG2 (0x04000018):    1 -> 1     over 40M cycles of Left              (0 px)
```

Roughly 70 frames of held input moves the field by **single pixels or not at
all**. A camera would move hundreds. So the D-pad is moving something else --
most plausibly a cursor -- and the framebuffer changed because that something
else moved.

**What this costs:** the 32-column refresh measurement was taken "while
scrolling" and was not. It still stands as a measurement of the steady-state
refresh -- and it agrees with the earlier stationary measurement, which is why
both said 32 columns in block 0 -- but it is **not** evidence about what happens
when the view actually moves. Block 1 was never written in either case, which is
the load-bearing fact and is unaffected.

**What is still unknown:** how this game scrolls its view at all. Not the D-pad,
not a stylus drag on the field. The remaining candidates are the minimap on the
top screen, an edge-of-screen region on the touch screen, or a shoulder-button
modifier -- none tested.

Until the view can be made to move on demand, no measurement of "what the
streamer does while scrolling" is possible, and that is the measurement the
widescreen work needs.

## Systematic input sweep: nothing found scrolls the view

Watching the BG scroll registers in a live match and holding each input for ~25M
cycles (roughly 45 frames):

| input | BG1 / BG2 | verdict |
| --- | --- | --- |
| A, B, Select, Start, R, X, Y | 508 / 508 | no change |
| L | 508 / **0** | changed **once**, did not repeat -- a toggle, not a scroll |
| D-pad (all four) | 508 / 508 | no change (`docs/109` retraction above) |
| stylus drag, fast and slow | 508 / 508 | no change |
| held touch at each screen edge | 508 / 508 | no change |

`BG1` never moved from 508 under any input at all.

So **the view cannot currently be scrolled by any input this harness has tried**,
and every "while scrolling" measurement in this document is really a
steady-state measurement. The two that matter are unaffected by that, because
they agree with their stationary counterparts: 32 columns refreshed, and block 1
never written.

Possibilities not yet tested, in rough order of likelihood:

* **the top-screen minimap** -- LEGO Battles shows one, and dragging it is a
  common DS scroll idiom, but the top screen is not a touch surface, so the game
  would have to expose it another way;
* **the camera following a unit** given a move order far away, which needs a
  unit selected and a destination tapped -- several steps the harness can do but
  has not;
* **a game state where scrolling is disabled**, e.g. an opening camera lock, in
  which case waiting or advancing the match would release it.

Until one of these works, the widescreen thread's first prerequisite is unmet
and steps 2 to 4 cannot be measured, let alone built.
