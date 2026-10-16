# The viewport dimensions, and who rewrites them

`docs/162` concluded that every one of the outstanding requirements -- no culling
of units, buildings, fog or particles; a minimap box matching the window;
edge-anchored UI -- reduces to the game's own view rectangle. This finds it.

## The fields

Scanning the view object at `0224B42C` for viewport-sized values finds exactly
two:

```
+0x16C = 32     the viewport width
+0x170 = 24     the viewport height
```

32 x 24 tiles is 256 x 192 pixels -- the DS screen. Nothing else in `0x200`
bytes of that object matches a viewport dimension.

## Writing them works, and does not last

Setting `+0x16C` to 56 (448 pixels) takes: it reads back as 56 and survives
several million cycles. **A single nudge reverts it to 32.**

Watching the field for writes names the writer:

```
pc=020EEB30  w=32  value=32  x8
```

which sits in a small setter:

```asm
020EEB1C  push {r4,r5,r6,lr}
020EEB20  mov  r4, r0        ; the target object
020EEB24  mov  r6, r1        ; width
020EEB28  mov  r5, r2        ; height
020EEB30  str  r6, [r4, #8]  ; -> +0x16C when r4 is view+0x164
020EEB34  add  r0, r6, #7
020EEB38  lsr  r1, r0, #3    ; (width + 7) / 8
020EEB3C  str  r5, [r4, #0xc]
020EEB40  str  r1, [r4, #4]  ; the ROW STRIDE IN BYTES
```

`(width + 7) / 8` stored as a stride means this object is a **bitmask, one bit
per tile**: 32 bits wide, 4 bytes per row, 24 rows, 96 bytes. A per-tile bitmask
sized exactly to the viewport is what fog of war, shroud or a dirty-tile map
looks like, which is consistent with `docs/162`'s expectation that fog is culled
by the same rectangle as everything else.

## The chain above it

Three callers. The one that targets this view is `0203A5BC`:

```asm
0203A5A8  cmp sb, #0x18        ; a loop over 24 rows
0203A5B0  ldr r1, [r6, #8]     ; width  -- copied from ANOTHER object
0203A5B4  ldr r2, [r6, #0xc]   ; height
0203A5B8  add r0, sl, #0x164   ; the destination: view + 0x164
0203A5BC  bl  020EEB1C
```

So `32` and `24` are **copied from a further object `r6`**, not immediates here.
The constants live further up the chain, and that is where a durable change has
to be made -- patching this site would only be overwritten by whatever sets `r6`.

## Where this leaves the requirements

The rectangle is found and it is writable; what is missing is its **source**.
That matters because per-frame rewriting from the vblank hook is not reliable
here: the game writes 32 every frame too, and whichever write lands last wins.
Patching the constant at its source is the durable form, and `docs/158` already
established that guest code in RAM accepts patches.

One iteration of following `r6` should reach it.
