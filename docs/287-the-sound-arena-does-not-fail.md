# 287 — The sound arena: unchecked, but measured succeeding

Carried on the open list as *"sound preload into a 686,080-byte heap with
silently-swallowed failures — needs live measurement"*. This is that
measurement, and the answer is **no defect on the measured configuration**. The
unchecked returns are real and are a latent hazard; they are not currently
firing.

Recorded because a hazard that is not firing is worth knowing about precisely,
and because "needs live measurement" items otherwise stay open forever.

## What is unchecked

`func_020894A4` builds the arena, at the very top of the function:

```asm
020894AE  ldr  r0, [pc,#0x2c8]   = 0x000A7800        ; 686,080 bytes
020894B0  ldr  r1, [pc,#0x2c8]   = 0x0214B434        ; "Sound/sound_..."
020894B2  movs r2, #0x86                             ; tag
020894B4  blx  func_0204C6E4                         ; allocate
020894B8  ldr  r1, [pc,#0x2c4]   = 0x02155494
020894BA  str  r0, [r1, #0x1c]                       ; store -- NO null test
020894BC  ldr  r1, [pc,#0x2b8]   = 0x000A7800
020894BE  blx  func_02106CE0                         ; build the arena over it
020894C2  ldr  r1, [pc,#0x2bc]   = 0x02155494
020894C4  str  r0, [r1, #0x24]                       ; store -- NO null test
```

`0xA7800` appears exactly **once** in the ARM9 image, as the literal at
`0x02089778`, reached by both of those loads — so this is the only place the
number lives and there is no second arena.

Neither return is tested. The recogniser's allocations in the same binary do
`cmp r0,#0 / beq`, so this is not the house style; it is an omission.

The per-bank loads in `func_02088590` have the same shape:

```asm
020885A6  ldr r0, [r5, #0xc]     ; cached handle
020885A8  cmp r0, #0
020885AA  bne <skip>             ; non-zero -> already loaded
020885B8  blx func_02088398      ; load
020885BC  str r0, [r5, #0xc]     ; store -- NO null test
```

A failed load leaves the field zero. Because the guard is "load if zero", the
next call simply retries — so a failure is not a crash, it is **an inaudible
sound and a retry, forever, with no diagnostic**. That is what "silently
swallowed" means here, and it is the reason this could not be settled by
reading code alone: success and failure produce the same control flow.

## The measurement

`r5` is `func_02088590`'s first argument, and it is `0x02155494`: the init at
`020894D8` loads that address and zeroes `+0x10` and `+0x0C`, which are the
same two offsets the preload fills. That identification is an inference from
matching offsets and the init/fill pairing, not from a traced call — stated so
rather than asserted.

Read at runtime, in gameplay, before and after an 8 s linger:

```
                       before            after
02155494  +0x00        00000000          00000001
          +0x0C        00000001          00000002     bank handle
          +0x10        00000000          00000001     bank handle
          +0x1C        02193EAC          02193EAC     the allocated block
          +0x24        02193EAC          02193EAC     the arena handle
0214B434               "Sound/sound_..."              the allocator's tag
```

* **The allocation succeeds.** `+0x1C` and `+0x24` are both `0x02193EAC`, so the
  686,080 bytes are live at `0x02193EAC..0x0223B6AC`. The two unchecked stores
  are storing a real pointer.
* **Both banks load.** `+0x0C` and `+0x10` are non-zero after the linger. `+0x10`
  reading `0` on the first sample and `1` on the second is the load happening
  between them, which is the expected sequence rather than a failure.
* The `"Sound/sound_..."` string at the allocator's name argument confirms the
  block being measured is the sound arena and not a same-shaped neighbour.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** on this route and this map, the arena is allocated
successfully and both sound banks load. The unchecked returns do not fire.

**NOT ESTABLISHED:** that this holds on every map. The arena is a fixed
686,080 bytes allocated once, so a map whose banks are larger, or a state that
loads more of them at once, could exhaust it — and by the mechanism above that
would be inaudible rather than loud. One route, one map (the default Free Play
"The Pond"), one moment.

**NOT ESTABLISHED:** that `+0x0C`/`+0x10` are the only bank handles. They are
the two the preload's first block fills; the function continues past
`0x020885D6` and may fill more.

## What would be worth doing, and what would not

Adding a null check to the guest is **not** worth doing: it would change
behaviour the cartridge does not have, on a path measured not to fail, and the
project's rule is that port changes rest on a strict reading of the decomp.

What would be worth doing, if sound ever goes missing in play, is a port-side
one-shot log on `+0x0C`/`+0x10` still reading zero after the preload returns —
cheap, silent when healthy, and it turns an inaudible failure into a visible
one. Not added now, because nothing has been observed failing and the project's
constraint is that logging must not cost frame time.
