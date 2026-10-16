# 319 — Item 4: the band harvest is written, and it is OFF, because it crashes

docs/318 measured item 4 end to end and named the next step as implementation
rather than another measurement. This is that implementation, what it taught,
and why it does not ship enabled.

## What landed

* `fp_paint_override` on **`func_020370B4`**, the placement painter — one
  caller (`func_02037EC0`) and nothing else, so the override is the whole of
  the painting path.
* `lb_fp_band_*` in `lb_widemap.cpp`: the camera accessors, a shift computed in
  whole metatiles from the latched band width, a harvest of the 26x32 buffer
  through the guest's own row table, and a per-column supplier.
* `lb_widemap_extends(3)` returns 1 **only while a harvest is live**, and the
  gpu2d BG3 chain gained two arms: serve a harvested footprint cell, else draw
  nothing at all. Never `0x001F` — that tile is opaque and is exactly the band
  docs/205 recorded the last time this layer was extended.
* `[lb_fp]` gained the guest's own UI record (`phase done kind org cur hover`)
  and a `painter override:` line that states `active` and both shifts.

## What it taught, which is the durable part

**A recompiled body does not return.** The first version called `original()`
three times — band, band, native. It harvested nothing, ever.

`lb_patches.cpp` already states the rule, in `set_owner_override`:

> A recompiled body is resumable -- it can return mid-function on a scheduler
> yield and be re-entered at a label (docs/108) -- so nothing may follow it.

The measured consequence is stronger than "resumable". A lifted function's
epilogue sets `R[15]` from LR and dispatches onward, so `original()` runs the
painter, then its caller, then *its* caller, and comes back only when the whole
slice unwinds. Everything after it runs once, at slice end, with `R[15]`
somewhere else entirely — and a second `original()` therefore lands on the
body's own entry guard:

```c
if (g_cpu.R[15] != 0x020370B4u) { switch (g_cpu.R[15]) { ... default:
    runtime_dispatch_miss(g_cpu.R[15]); return; } }
```

The run that proved it printed the override's diagnostic exactly **once**, with
`active=1 shiftL=-240 shiftR=+240`, and produced no harvest line at any of the
five drag positions. `marquee_observer` gets away with work after `original()`
only because its work is one idempotent counter at slice end.

**The camera shift was visible in the probe.** `[lb_fp]` reported `cam=25,144`
against an unshifted 265 — exactly 265 - 240. That is the diagnostic working:
the shift was real, the paint after it was not.

## Why it is off

The second version did it the way this codebase already does a nested guest
call — `runtime_call_push_return`, LR and PC at the target,
`runtime_dispatch`, argument registers preserved — the same shape as
`call_guest` and `issue_waiting_to_die`.

That run reached placement, printed `painter override: active=1 shiftL=-240
shiftR=+240`, wrote one more `[lb_geom]` line, and then **stopped producing
output and exited**. The screenshot that followed timed out after 90 s against
a process that was no longer there.

So `LB_FPBANDS` now defaults to **0**, and everything behind it is inert:
`lb_fp_placement_active()` returns 0, the override is a pure tail call to the
cartridge's own painter, no harvest runs, `lb_fp_bands_active()` stays 0 and
`lb_widemap_extends(3)` keeps returning 0. The render path is what it was.

One crash is not a diagnosis, and this page does not offer one. What it rules
out is the shape of the call, not the design: docs/306's argument that the
painter's three side effects are safe to repeat is untouched by a crash in the
machinery that repeats it.

## Candidates for the crash, in the order worth testing

1. **Stack.** `func_020370B4` reserves a large frame — the BitArray2D local is
   at `sp + 0x124` — and the nested call runs on the guest's live stack at
   whatever `R[13]` the outer painter had already moved it to. `call_guest`'s
   own users move `R[13]` down to scratch below the live frame first
   (`issue_waiting_to_die` sets `g_cpu.R[13] = scratch - 0x20`), and this does
   not.
2. **Re-entrancy of the caller.** The override fires on every painter call, and
   `func_02037EC0` has six callers — including `func_020AA9F8`, the placement
   tick itself. A nested paint issued from inside the tick is a different
   context from one issued from the view tick.
3. **The return marker, across a chunked body.** `runtime_call_push_return`
   with `LR == kFpPaint` is the pattern `call_guest` uses, but its targets are
   leaf-ish helpers. The painter is **0xAE8 bytes** by the corpus, returning
   with `pop {r4-fp, pc}` at `0x02037B7C` — and the lifted
   `lb_arm9_afunc_020370B4` covers only `0x020370B4..0x020372B4`, ending on
   `runtime_dispatch_literal_fallthrough(0x020372B4u)`.

   That chunking is not a defect — an ARM function with inline literal pools
   lifts this way — but it means the body spans several dispatch entries joined
   by fallthrough, and the marker has to survive that whole chain before the
   `pop {pc}` that finally consults `runtime_call_should_return`. A marker that
   is never matched turns the return into
   `runtime_dispatch_with_exchange(0x020370B4)`, which re-enters the painter:
   an unbounded loop that renders nothing, which is exactly what a window that
   stops responding with no crash message looks like.

(1) is the cheapest; (3) is the one that matches the observed symptom. Both are
cheap to separate with a counter on entries against returns, which is what the
next pass adds.

## Second pass: instrumented, capped, and DIAGNOSED

`fp_call_painter` now counts its entries and its returns, prints the first six
with `sp`, `lr` and `r0`, and **refuses after 400 calls**. The instrumented run
answers the question outright:

```
[lb_fp] nested painter call #1 ENTER sp=027E3548 lr=02037F48 r0=0224B4D4
[lb_fp] nested painter call #1 LEAVE ok=0 pc=021104D0
[lb_fp] nested painter call #2 LEAVE ok=0 pc=021104B8
[lb_fp] nested painter call #3 LEAVE ok=0 pc=021104D0
...
[lb_fp] nested painter calls CAPPED at 400 (0 returned)
```

**Zero of four hundred returned**, and every one stopped at the same handful of
addresses. `0x021104B4` is a 0x28-byte halfword fill:

```asm
021104b4  mov  r3, #0
021104b8  cmp  r3, r2
021104c4  strh r0, [r1, r3]
021104d0  add  r3, r3, #2
021104d4  blt  #0x21104b8     <- the backward branch
021104d8  bx   lr
```

That is the painter's own buffer clear — the `0x600` bytes of `0x001F`
docs/286 describes — and `0x021104D4` is a **backward branch**, which is
exactly where `runtime_slice_yield` is checked. Its comment says what happens
next:

> When it fires it arms `g_unwinding` so the BL/BLX return-checks PRESERVE
> their pending returns as the host stack unwinds — the call-return stack is
> saved/restored per-CPU by the scheduler, so a spin at any call depth can be
> preempted and cleanly resumed.

Preserving the pending return is right for the guest's own calls and **wrong
for this one**: the host C frame it would resume into is `fp_call_painter`, and
that frame is gone. So the marker stays on `g_crs` for ever, one per painter
call, and the stack fills until `nds_halt("call-return overflow")` stops the
emulator.

**That is the crash.** Not memory, not the stack pointer, not re-entrancy —
a leaked return marker, at one per frame of placement. Candidates (1) and (3)
above are both wrong and are left standing only as the record of what was
guessed before it was measured.

Two things follow:

* `runtime_call_cancel_return(kFpPaint)` now runs on every failed call, so the
  leak is gone whatever else happens.
* **A nested synchronous guest call cannot survive a slice yield**, and the
  painter reliably yields because its first act is a ~3,800-instruction fill
  loop. docs/241 already said this in the abstract — *"a chain of guest calls
  losing its return to a cooperative slice yield, so the length of the chain
  IS the problem"* — and the measurement here is that the chain does not have
  to be long. One call to a function with a big loop is enough.

## Third pass: the painter busy-waits on hardware, so it cannot be called at all

Lifting the slice cap for the one call is the obvious answer to a slice yield,
and it is wrong. With `g_cycle_cap` saved, set to 0 for the call and restored,
the fill loop stopped being where it stopped — and every call stopped at
exactly one new address instead:

```
[lb_fp] nested painter call #1 LEAVE ok=0 pc=0210A760
[lb_fp] nested painter call #2 LEAVE ok=0 pc=0210A760      (6 of 6 identical)
```

```asm
0210a75c  ldr  r1, [pc, #0x28]   = 0x04000280   ; DIVCNT
0210a760  ldrh r0, [r1]
0210a764  tst  r0, #0x8000                      ; the divider's BUSY bit
0210a768  bne  #0x210a760                       ; SPIN
0210a76c  ldr  r0, [pc, #0x1c]   = 0x040002A0   ; DIV_RESULT
```

`0x04000280` is the **DS hardware divider**. The painter busy-waits on it —
world-to-screen needs a division — and a busy-wait ends only when the
*scheduler* advances emulated time and clears that bit. Inside a nested
synchronous call the scheduler does not run. So:

* with the cap in place, the slice yield breaks the spin and the call never
  returns — 400 of 400;
* with the cap lifted, the spin never ends at all. The seventh call hung the
  game, and the screenshot after it timed out against a live but frozen
  process.

**`func_020370B4` cannot be called synchronously from the host by any
arrangement of markers, caps or stack.** That is a property of the function,
not of the machinery around it. The cap lift is reverted; the marker cancel and
the "already unwinding" guard stay, because both are correct regardless.

It is worth saying plainly that the previous section's confident "**That is the
crash**" was half an answer. The marker leak was real and was one failure mode;
it was not the reason the call could not complete. Two measurements, two
different walls.

## What that rules out, and what is left

Design (3) is not dead, but **calling the painter synchronously is** — by
anything, not just by this code. The remaining shape is to let the guest call
the painter itself, once per frame, with the camera shifted before the call and
the buffer put back afterwards:

* the override sets the camera for the band this frame belongs to and
  tail-calls `original()` — no nested call, nothing to yield, and the guest's
  own painter is free to spin on the divider for as long as it likes because
  the scheduler is still running;
* a second hook harvests the buffer into the band table, writes the saved
  NATIVE buffer back over it, and restores the camera — before anything that
  draws with the camera runs.

The cost is that the native footprint is at most two frames stale while a
building is being dragged, which at 57 fps is not a thing anyone can see.

**Where the second hook goes is now readable rather than open.** The painter has
one call site, `0x02037F44` inside `func_02037EC0`, and the tick around it is
short:

```asm
func_02037EC0:  ... bl func_020370b4 ; strb r0,[r4,#0x24]
                ... bl func_02036fbc ; bl func_0208727c ; blx func_020a8750
func_020398B8:  ldrb r0,[r4,#0x78] ; bl func_02037ec0
                blx  func_020a186c
                blx  func_020d999c          <- the last call of the view tick
```

The first of those four is the answer, and it also rules out the later ones.
**`func_02036FBC` does use the camera** — it calls `func_0203CA10(&out, view,
&in)` and then adjusts the result — so the camera must be back before it runs,
not after. And it is ideal for the job:

* it has **exactly one caller**, `func_02037EC0`, the placement tick;
* it is the **very next call after the painter**, at `0x02037F60` against the
  painter's `0x02037F44`;
* it is in the dispatch table as ARM, so it takes an override like any other.

So the shape is two overrides and no nested call at all:

| frame | painter override, BEFORE `original()` | `func_02036FBC` override, BEFORE `original()` |
|---|---|---|
| n % 3 == 0 | nothing | save the buffer as the NATIVE copy |
| n % 3 == 1 | camera -= band | harvest to band[0]; write the native copy back; restore camera |
| n % 3 == 2 | camera += band | harvest to band[1]; write the native copy back; restore camera |

The guest calls its own painter, so it may spin on the divider for as long as
it likes — the scheduler is still running. The camera is shifted across exactly
one function, the painter, which is the only one that should see the shift. The
upload takes the restored native copy, so the 256-px picture is at most two
frames stale during a drag. And `view+0x68` is set by the painter either way,
so the upload still happens (docs/306: it is an idempotent dirty flag).

Nothing here calls guest code out of turn, which is what every failure on this
page has in common.

## Status

**IMPLEMENTED, NOT ENABLED, ROOT CAUSE KNOWN.** Item 4's measurement stands
and has now reproduced three times identically (docs/318). The shipped build is
unchanged: with `LB_FPBANDS` unset the override is a pure tail call, and a run
with placement driven and the ghost dragged across both bands finishes clean at
57 fps.

## Status

**(superseded by the Status section at the end of this page.)**
(docs/318); its fix does not. Nothing about the shipped build changed: a run
with the flag off is the same run it was before this page.
