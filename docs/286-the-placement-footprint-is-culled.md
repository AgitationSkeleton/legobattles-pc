# 286 — The placement footprint is culled at 32×24, and the stride is a table

The building-placement UI could become dishevelled when clicking around to
place a building, and building has to work anywhere in the widened view.

This traces the footprint end to end and finds the change set is **much smaller
than expected**. The earlier assumption — that the port would have to draw the
footprint itself, and therefore reimplement the placement predicate — is wrong.

## The chain

`func_020370B4(placement_state)` is the painter.

* It clears `0x021530A0` to `0x1F` for `0x600` bytes and sets `fp = 0x4F`.
* Origin tile is `[r4,#0x04]`/`[r4,#0x05]`, footprint size is
  `[r4,#0x20]`/`[r4,#0x21]` — two u8 pairs, each fed to `Vector2s16`.
  **Backwards (docs/392):** the loop runs from +0x20 to +0x20 + +0x04, so
  +0x20/+0x21 is the origin in map metatiles and +0x04/+0x05 the size. A
  bridge's origin walks with the cursor at size 1x2 (run t392i).
* It then runs a **nested loop** (`sl` over rows to `[sp,#0xa2]`, `r7` over
  columns to `[sp,#0xa0]`) and inside that loop recomputes `fp` as `0x4F` or
  `0x3F` — so the verdict is **per tile**, not per site. Seven separate
  `mov{cc} fp` sites feed it.
* Each cell goes to `func_0203cbb8(view, &Vector2u8(col,row), fp)`.
* `[sp,#0x1c]` starts at 1 and is zeroed whenever a cell comes out `0x3F`; it is
  the function's return value, i.e. the whole-site verdict.

`func_0203cbb8` converts to screen space: camera from `[r4,#0x5e]`/`[r4,#0x60]`
in 12.12 fixed point, metatile `0x18000`/`0x10000` (24×16 px).

## Where it is lost: a cull, not a clamp

```asm
0203CD30  ldrsh   r2, [sp, #0x3c]     ; min.x
0203CD34  cmp     r2, #0
0203CD38  ldrshge r0, [sp, #0x3e]     ; min.y
0203CD3C  cmpge   r0, #0
0203CD40  addlt   sp, sp, #0x9c       ; min < 0      -> RETURN, cell dropped
0203CD44  poplt   {r3, r4, r5, r6, pc}
0203CD48  ldrsh   r1, [sp, #0x44]     ; max.x
0203CD4C  cmp     r1, #0x1f           ; 31
0203CD50  ldrshle r0, [sp, #0x46]     ; max.y
0203CD54  cmple   r0, #0x17           ; 23
0203CD58  addgt   sp, sp, #0x9c       ; max > 31/23  -> RETURN, cell dropped
```

32×24 eight-pixel cells is exactly the native 256×192. A site in the widened
columns is **dropped**, which is the same shape as docs/283's command cursor —
culled rather than misplaced. Partially-visible cells are then clamped at
`#0x20`/`#0x18` (`0203CD80`, `0203CD94`), so the two behaviours are distinct and
both are present.

## The finding that makes this cheap: the stride is a row table

`func_0203d8e8` is the whole of the cell addressing, five instructions:

```asm
0203D8E8  ldr r2, [pc, #8]          = 0x021520F8      ; the row table
0203D8EC  ldr r1, [r2, r1, lsl #2]  ; row base = table[y]
0203D8F0  add r0, r1, r0, lsl #1    ; + x halfwords
0203D8F4  bx  lr
```

**No stride is hardcoded here.** The geometry lives entirely in the table, which
`func_02038018` builds in two identical loops:

```asm
020380A8  add r2, r1, ip, lsl #1     ; r1 = 0x021530A0
020380AC  str r2, [r0, r3, lsl #2]   ; r0 = 0x021520F8
020380B0  add r3, r3, #1
020380B4  cmp r3, #0x1a              ; 26 rows
020380B8  add ip, ip, #0x20          ; 32 halfword cells per row
020380BC  blo #0x20380A8
```

So the footprint's whole geometry is **four immediates and one base pointer**:

| what | where | value |
|---|---|---|
| cull max x | `0203CD4C` | `0x1F` |
| cull max y | `0203CD54` | `0x17` |
| clamp x | `0203CD7C`/`0203CD80` | `0x20` |
| clamp y | `0203CD90`/`0203CD94` | `0x18` |
| row count | `020380B4`, `020380DC` | `0x1A` |
| row stride | `020380B8`, `020380E0` | `0x20` |
| buffer base | literal at `func_02038018` | `0x021530A0` |

Point the table at a larger port-owned buffer, widen those, and **the guest
computes every per-tile verdict itself, across the full surface**. Nothing about
the placement predicate has to be replicated — which matters, because
`func_0205C034` has eleven callers and is the kind of thing this project
never guesses at.

## What is NOT established, and is the next thing to measure

**Whether repointing the row table is safe for the buffer's other consumers.**
`0x021530A0` has **nine** literal-pool references — `func_020370B4`,
`func_02037B9C`, `func_02038018`, `func_02038890`, `func_020396E0`,
`func_0203B7B8`, `func_0203B7FC`, `func_0203C78C`, `func_0203D8D8` — while the
row table has only **two** (`func_02038018` and `func_0203D8E8`). Those two
counts differing is the whole risk: anything that reaches the buffer by its own
literal rather than through the table will still address it as 32×26 and would
write into the wrong rows of a widened buffer.

The marquee (`0203BE8C`), the selection-marker pass (`0203C9AC`) and
`func_02038890` (`0203897C`) all push `0x021530A0` for `0x680` bytes through
`func_0204E0E0`, and `func_020396E0` uploads it via `func_0210CBE0`. A widened
buffer must keep handing those the native sub-rect, or the port must take over
drawing them — and it already takes over the marquee (`marquee_hide_guest`
defaults on).

**Also not established:** that widening the cull does not break the guest's own
upload. The cull exists partly to keep writes inside the buffer; widening it
without widening the table first would write past row ends. The two changes are
one change and must land together.

## MEASURED: the table matches the arithmetic

Read at runtime, in gameplay, before and after an 8 s linger (identical both
times, so this is the steady state and not a mid-build snapshot):

```
021520F8  021530A0 021530E0 02153120 02153160
02152108  021531A0 021531E0 02153220 02153260
02152118  021532A0 021532E0 02153320 02153360
02152128  021533A0 021533E0 02153420 02153460
```

`0x021530E0 − 0x021530A0 = 0x40` — 32 halfwords per row, from base
`0x021530A0`, exactly what `func_02038018` computes. The same read confirms
`*(0x02152030+4) = 0x0224B448`, which is the object `func_020370B4` hands
`func_0203cbb8` as `r4` and therefore the one whose `+0x5E`/`+0x60` is the
camera — the same object the rest of this project already calls the camera.

## AND THE CONSTRAINT THAT KILLS THE OBVIOUS FIX

Repointing the row table alone **does not work**, and the reason is the
nine-references-versus-two gap named above, made concrete:

* `func_020370B4` clears the buffer through **its own literal** (`0x021530A0`,
  `0x600` bytes). Repoint the table and the new buffer is never cleared, so
  cells from previous frames persist.
* `func_020396E0` uploads through **its own literal** too. Repoint the table and
  the guest uploads a buffer nothing writes any more — so the footprint would
  disappear from the **native window as well**, turning a missing-in-the-bands
  bug into a missing-everywhere bug.

So the row table is not the single choke point it first appears to be. It is the
choke point for *addressing*, while clearing and uploading each bypass it.

### Three candidate designs, with the cost that rules each in or out

1. **Repoint the table and take over drawing entirely.** The port would have to
   render tiles `0x4F`/`0x3F` itself from the overlay tileset. Substituting a
   drawn box for the game's own tile art is exactly the kind of substitute the
   project rules out (no guessing, substitutes or shortcuts), so this is only
   acceptable if the port draws the *real* tiles.

2. **Route only out-of-range cells to a side buffer.** Preserves the native path
   byte for byte — clear, upload and the other seven consumers all untouched.
   Needs a range check inside `func_0203d8e8`, which is five instructions with
   no room, and an override will not fire there because lifted code reaches it
   by a direct `bl` (docs/238's structural finding).

3. **Re-run the painter with a shifted camera and harvest the buffer.** The
   guest computes every verdict; the port only collects. But `func_020370B4` has
   side effects beyond painting — it returns the site verdict, writes `1` to the
   view's `+0x68`, and calls a `BitArray2D` virtual — so invoking it extra times
   per frame risks double-applying them. Not safe without establishing what
   those side effects are.

## Status

Open, fully traced, **not implemented, and deliberately so**. The trace is
sound and the runtime read confirms it, but none of the three designs above is
currently justified: (1) needs the port to render the guest's own tiles, (2) is
blocked by the override limitation, and (3) needs `func_020370B4`'s side effects
established first. Implementing any of them on the current evidence would be
guessing, which the project does not do.

The cheapest next measurement is for design (3): determine what
`view+0x68` and the `BitArray2D` virtual actually do, because if they are
idempotent then (3) becomes safe and needs no guest patching at all.

## RESOLVED — design (3)'s blocking question is answered

The "cheapest next measurement" named above was taken. Both of
`func_020370B4`'s side effects are benign:

* `view+0x68` gets the **constant 1** (`mov r2,#1; strb r2,[r1,#0x68]`), and
  `func_020396E0` clears it to 0 once it has uploaded. It is a dirty flag, and
  setting it twice is setting it once.
* The `BitArray2D` call takes `sp+0x124` — a **stack local**, constructed at
  entry by `func_020eea60` (which only zeroes) and destroyed at exit by
  `func_020eebb8` (which frees only if non-null). Balanced and function-local.

**Design (3) is safe and is the one to build.** Also established: the footprint
layer is **BG3 engine A** (BG3CNT at `0x0400000E`, via `func_0210BD78`), which
the port currently holds to be uniformly inert — a measurement that is true
only while no placement is in progress, and the second cause of this item on
top of the cull traced above.

See docs/306.
