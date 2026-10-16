# Block 1 is BG1's map: the layers are packed, and there is no spare tilemap

This retracts `docs/115` and `docs/116`. Both rest on "block 1 is never
written", and that was a **measurement artifact**.

## The artifact

`docs/115` sampled the tilemap watch in fourteen windows and reported 6656
writes to block 0, 512 to block 2, and none to block 1. The totals sum to
**7168 = 14 x 512**: every window returned exactly the 512-event cap, so the
ring was **saturated in every sample**, and only the most recent 512 writes of
each window survived. Block 1's writes happened earlier in each window and were
discarded before they could be read.

`count == max` is the tell, and it was visible in the numbers as recorded.

## What is actually there

Reading the blocks directly:

```
block0 06000000:  960/1024 non-zero,  30 distinct  first row: 0064 0064 0064 ...
block1 06000800:  854/1024 non-zero, 176 distinct  first row: 0022 0023 0027 0028 0029 0000 01E0 ...
block2 06001000:  960/1024 non-zero,  68 distinct  first row: 001F 001F 001F ...

entries equal between block0 and block1: 64 / 1024
after scrolling:  block0 changed = True,  block1 changed = True
```

Block 1 holds **854 non-zero entries across 176 distinct values** -- richer and
more varied than block 0 -- shares only 64 entries with it, and is **rewritten
when the view scrolls**. It is live map data, not stale memory.

The DMA register capture shows it being filled explicitly, in a matched pair:

```
src=02152A20  dst=06000000  cnt=80000340   832 entries   x23
src=021523A0  dst=06000800  cnt=80000340   832 entries   x23
```

832 entries is 26 rows of 32 columns. Two layers, one transfer each, same size,
sources 0x680 apart.

## The real layout: packed, not spare

BG2 is 512x256 at screen base 0, so it spans blocks 0 and 1. BG1 is 512x256 at
screen base 1, so it spans blocks 1 and 2. The maps are **packed back to back**,
each layer using only its first 32 columns, which works because the field never
scrolls past column 31 -- the scroll registers sit at a constant 4.

So block 1 is simultaneously BG2's unused second half and **BG1's live first
half**, and the second interpretation is the operative one.

## Consequences

* **`docs/116` is retracted.** Its 60.6% pixel change was not BG1 revealing a
  hidden overlap; it was **BG1's map being corrupted** by overwriting it with
  BG2's tiles. The "spurious trees and ground texture" were BG1's real terrain
  replaced by the wrong tiles. Its conclusion that BG1 is "an enabled but empty
  layer" is exactly backwards: BG1 is a populated, actively-streamed terrain
  layer.
* **`docs/115`'s plan is void.** There is no free 2 KB block to fill and nothing
  to repoint BG1 *away* from -- block 1 is where BG1 belongs.
* **`docs/107`'s garbage is explained.** Decoding BG2 past column 31 reads
  BG1's map through BG2's tile base and palette. Not stale memory, not a
  partially-filled map: another layer's data, which is why it looked like
  structured repeating tiles rather than noise.

## What widening would actually require

The field has **no spare tilemap space at all**. Every 2 KB block in
`06000000..06001FFF` is a live map for one of the three text layers. A genuinely
wider BG2 needs a real 64-column map, which means relocating BG1's and BG3's
maps to free blocks elsewhere in VRAM_E/F, then extending the streamer to fill
BG2's columns 32..63 from the same source it already builds.

That is a substantially larger guest-side change than anything proposed in this
thread so far, and it is the first version of the plan that is consistent with
what the hardware is actually doing.

## Method note

This thread has now produced two wrong conclusions from instruments rather than
from the game: scroll registers that are constant by design, and a watch ring
read past its capacity. Both were detectable at the time -- the first by
capturing a screenshot, the second by noticing the event count equalled the cap.
Check the instrument's own saturation and semantics before drawing a negative
from it.
