# Block 1, measured on the right engine

> **WRONG, retracted by [docs/117](117-block-1-is-bg1s-map-and-the-layers-are-packed.md).**
> The "block 1 receives no writes" result is a ring-saturation artifact: the
> write totals sum to exactly 14 x 512, the sample cap, so only the most recent
> writes of each window survived. Block 1 is BG1's live map. Do not act on the
> plan below.

`docs/109` concluded that widening the field means filling the tilemap's second
screen block, and `docs/114` established that its measurements were on engine A
after all -- the field. This is that measurement repeated deliberately, with the
engine known rather than assumed.

## Method

Watch `06000000..06002000` -- engine A's BG map region -- while **holding Left**,
so the tile streamer is actively working, sampling in fourteen short windows to
keep the 512-entry ring from saturating and hiding the distribution.

## Result

```
writes per 2KB screen block:
   block 0  (06000000):  6656 writes
   block 1  (06000800):     0 writes
   block 2  (06001000):   512 writes

single writer PC: 01FF8260  (the ITCM DMA routine)
```

**Block 1 receives no writes at all**, while the field is scrolling and the
streamer is demonstrably busy. This is `docs/109`'s finding, now on the engine
that actually draws the field.

## Which layer owns which block

Screen base is in 2 KB units, and a 512x256 text layer occupies **two**
consecutive blocks:

| layer | prio | size | blocks | written? |
| --- | --- | --- | --- | --- |
| BG0 | 2 | (3D layer) | -- | n/a |
| BG1 | 3 | 512x256, base 1 | 1, 2 | block 1 no, block 2 yes |
| BG2 | 1 | 512x256, base 0 | 0, 1 | block 0 yes, block 1 no |
| BG3 | 0 | 256x256, base 2 | 2 | yes |

So **block 1 is shared**: it is BG2's second half and BG1's first half. The 512
writes to block 2 are consistent with BG3, the front-most layer.

The terrain is **BG2** -- priority 1, behind the 3D units at priority 2 in the
DS's ordering, in front of BG1 -- and its columns 32..63 live in block 1, which
nothing ever writes. That is the exact 2 KB that a wider view needs filled, and
its address is `06000800`.

## The complication this measurement exposes

Block 1 is not spare memory. Filling it makes those tiles appear **twice**:

* as BG2 columns 32..63, which is the intent; and
* as BG1 columns 0..31, at priority 3, behind everything.

BG1 is currently empty everywhere it is displayed, which is why nobody has
noticed the overlap. Whether the second appearance is harmless depends on
whether BG2 has transparent pixels in the visible span, since BG1 sits directly
behind it and would show through.

This has not been tested and is not assumed either way. It is the first thing to
check before writing anything into block 1: fill it with a recognisable pattern
and look at whether the pattern appears behind the existing field.

## Not yet answered

What bounds the streamer to 32 columns. The writes all come from one ITCM DMA
routine at `01FF8260`, so the column count is decided by whoever builds that
DMA's source and length, not by the copy itself. That producer is the next thing
to read.
