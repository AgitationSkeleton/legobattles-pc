# 306 — The placement footprint: one design, justified, and the layer it lives on

docs/286 traced defect item 4 end to end, listed three candidate designs, and
deliberately implemented none of them — each needed something established
first. It named the cheapest next measurement:

> determine what `view+0x68` and the `BitArray2D` virtual actually do, because
> if they are idempotent then (3) becomes safe and needs no guest patching at
> all.

Both are measured here, and both are benign. Design (3) is unblocked.

## `view+0x68` is a dirty flag, and the store is a constant

The painter's tail:

```asm
02037B5C  ldr  r1, [pc, #0x24]     = 0x02152030
02037B64  ldr  r1, [r1, #4]        ; the view
02037B68  mov  r2, #1
02037B6C  strb r2, [r1, #0x68]     ; <- the "side effect"
```

It stores the **constant 1**. Writing 1 twice is writing 1 once, so it is
idempotent by inspection. And the uploader confirms what the flag means — it is
the last thing `func_020396E0` does:

```asm
02039894  mov  r0, #0
02039898  strb r0, [r6, #0x68]     ; cleared once the buffer has been uploaded
```

`+0x68` is "the footprint buffer needs uploading". Setting it again is at worst
a redundant upload, never a wrong one.

## The `BitArray2D` call is a stack local's destructor

The call that looked like a global side effect takes `sp + 0x124`:

```asm
020370C0  add r0, sp, #0x124
020370C4  bl  #0x20EEA60          ; constructor, at entry
...
02037B60  add r0, sp, #0x124
02037B70  bl  #0x20EEAA8          ; Battles::BitArray2D::virtual_00, at exit
```

The same stack slot, constructed on the way in and destroyed on the way out.
Neither touches anything outside the object:

```asm
func_020EEA60   str vtable,[r0] ; str #0,[r0,#0x10] ; str #0,[r0,#0x14]   no allocation

func_020EEBB8   ldr r0,[r4,#0x10] ; cmp #0 ; bne -> free ; str #0
                ldr r0,[r4,#0x14] ; cmp #0 ; bne -> free ; str #0
```

The constructor only zeroes; the destructor frees **only if non-null** and
nulls after. So the pair is balanced and null-guarded: a call that allocates
nothing frees nothing.

**All three of the painter's "side effects beyond painting" are therefore safe
to repeat**: the return value (which an extra call simply discards), an
idempotent flag store, and a function-local constructor/destructor pair.

## The layer: BG3, and the port already believes it is inert

Tracing the upload settles where the footprint actually goes:

```
func_020396E0 -> func_0210CBE0(0x021530A0, 0, 0x680)
                 -> func_0210BD78 -> ldrh r3,[0x04000000 + 0xE]   BG3CNT
```

`0x0400000E` is **BG3CNT**, and the screen-base block is extracted from it. The
footprint is **BG3 on engine A**.

That matters, because the port's own gpu2d hook says this about BG3:

> *"BG3 DRAWS NOTHING OUTSIDE THE GUEST'S OWN ROWS, because that is what it does
> INSIDE them. Measured: the cartridge writes 001F into every one of BG3's own
> 32 columns, on every row, and draws nothing."*

That measurement is correct **and it was taken with no placement in progress**.
`func_020370B4` clears the buffer to `0x1F` and then writes `0x4F` or `0x3F`
per footprint cell, so BG3 is inert exactly until a building is being placed —
at which point it carries the entire footprint overlay.

So item 4's defect in the widened view has two independent causes stacked on
each other:

1. the guest **culls** any footprint cell outside 32x24 screen cells
   (`0203CD4C` / `0203CD54`, docs/286);
2. the port serves the band columns of BG3 a **clear tile** —
   `lb_widemap_clear_tile(3, ...)` — because it holds BG3 to be uniformly
   inert.

Either alone would hide the footprint outside the native 256 px. Both are
present.

## Why this makes the job much easier than terrain

BG3 carries **nothing but the footprint**. There is no camera model to infer,
no shroud record to trust, none of the machinery `lb_widemap_tile` needs for
terrain and fog — where `choose_camera` demands 48 of 48 sampled cells agree.
For BG3 the default is one constant, `0x001F`, and the only content is whatever
the painter wrote.

## The design, now fully specified

1. Save the camera at `view+0x5E` / `view+0x60` (12.12 fixed, docs/286's
   runtime read confirms the object is `*(0x02152030+4)`).
2. Shift it by whole metatiles so that a band region falls inside the guest's
   own 32x24 cull window.
3. Call `func_020370B4`. The guest computes **every per-tile verdict itself**,
   with its own seven `mov{cc} fp` sites — nothing about the placement
   predicate is replicated, which is the whole point, because `func_0205C034`
   has eleven callers and the project never guesses at a predicate it can run.
4. Harvest `0x021530A0` (26 rows x 32 halfwords, row table at `0x021520F8`).
5. Restore the camera and let the guest's own final pass paint and upload the
   native window exactly as it does today, so the 256 px picture is unchanged
   byte for byte.
6. Serve the harvested cells for band columns of BG3 instead of the clear tile.

## The machinery already exists

`call_guest(addr)` is in `lb_patches.cpp` — push a return marker, point LR and
PC at the target, `runtime_dispatch`, and check `runtime_unwinding()`. It is
already used for a five-call chain.

Its documented failure mode is the one constraint worth carrying into the
implementation: *"a chain of guest calls losing its return to a cooperative
slice yield, so the length of the chain IS the problem and every call removed
is a real improvement"* (docs/241). Design (3) needs **one** call per shift,
which is the cheapest shape this machinery has been asked for.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** `view+0x68` is an idempotent dirty flag, cleared by the
uploader; the `BitArray2D` call is a balanced, null-guarded stack-local
constructor/destructor pair. Design (3)'s blocking question is answered.

**ESTABLISHED:** the footprint layer is BG3 engine A, via BG3CNT in
`func_0210BD78`.

**ESTABLISHED:** the port's "BG3 is uniformly inert" measurement is true only
while no placement is active, and the band columns are served a clear tile,
which is the second cause of item 4 on top of the guest's own cull.

**NOT IMPLEMENTED.** This tick establishes the design and locates the
machinery; it writes no code. Designs (1) and (2) from docs/286 stay rejected —
(1) would have the port draw substitute art, (2) is blocked by docs/238's
override limitation.

**NOT ESTABLISHED:** the cost. One extra `func_020370B4` per shift per frame
during placement only, over a footprint-sized loop rather than a screen-sized
one, should be negligible — but "should be" is not a measurement, and the
`fps.txt` budget is the thing that decides it.
