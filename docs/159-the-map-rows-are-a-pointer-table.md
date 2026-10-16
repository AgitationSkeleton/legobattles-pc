# The staging map is addressed through a row-pointer table

`docs/158` ended needing one thing: where the builder's destination comes from,
so a wider map has somewhere to live. It is better than a base pointer.

## The chain

The streamer is **`func_02039FB4`** -- confirmed by its second and third
instructions reading the camera at `[sl+0x5E]` and `[sl+0x60]`, the exact fields
`docs/112` identified:

```asm
02039FB4  push {r3..fp, lr}
02039FBC  mov  sl, r0
02039FC4  bl   0203D8FC          ; get the destination buffer
02039FCC  str  r0, [sp, #0x14]   ; keep it
02039FD4  ldrsh r5, [sl, #0x5e]  ; camera X
02039FD8  ldrsh r4, [sl, #0x60]  ; camera Y
...
0203A0B8  ldr  r3, [sp, #0x14]
0203A0C4  add  r6, r3, r5, lsl #1    ; r6 = base + index*2
0203A100  add  r2, r6, #0x40         ; the next row
```

`func_0203D8FC` is three instructions:

```asm
ldr r1, [pc, #4] ; add r0, r1, r0, lsl #1 ; bx lr
<literal 02152A20>
```

so it returns `base + index*2` from a literal at `0203D908`.

## And a second accessor, which is the useful one

Immediately after it:

```asm
0203D90C  ldr r2, [pc, #8]           ; a TABLE address
0203D910  ldr r1, [r2, r1, lsl #2]   ; table[bufferIndex]
0203D914  add r0, r1, r0, lsl #1     ; + entryIndex*2
```

The table is at **`02152090`**, and live it holds:

```
[0] 02152A20   [1] 02152A60   [2] 02152AA0
[3] 02152AE0   [4] 02152B20   [5] 02152B60
```

Spaced **`0x40`** apart -- 64 bytes, 32 entries, **exactly one map row**. These
are **row pointers**, and they are why the builder's stride constant is `0x40`.

## Why this matters

Relocating and widening the map does not need code patched at all. The rows are
reached through a **table of pointers in data**, so:

* rewriting those pointers with `0x80` spacing into a larger scratch area gives
  every row 64 entries instead of 32;
* the builder's `add r2, r6, #0x40` still has to become `#0x80`, and the bound
  `#0x0A` still has to rise -- but both are already proven patchable
  (`docs/158`);
* nothing has to be relocated by hand, because the table *is* the layout.

A data-driven layout is the friendliest thing this port could have found here,
and it is the route `docs/149` established as the one that actually works.

## Open

The table's first entries belong to the buffer at `02152A20`, which `docs/118`
matched to **block 0, BG2**. The terrain buffer -- `021523A0`, DMA'd to block 1
and therefore BG1 per `docs/156` -- is not among the six entries read. Either
the table is longer than six, or the terrain rows are reached through a second
table. That is the next read, and it is narrow: the same accessor pattern with a
different literal.
