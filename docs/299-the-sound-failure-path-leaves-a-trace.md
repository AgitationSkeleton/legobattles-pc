# 299 — The sound failure path is checked, silent, and leaves a trace

The defect: unit sounds do not play, except for a couple of buildings or units
-- the Astro builder's plays, but not the Space Criminal Leader's or the
Islander's.

docs/287 cleared the *heap*: the 686,080-byte arena allocates, and both banks
`func_02088590` loads first report non-zero handles. That is not the same as
"the sounds an individual unit needs are present", and this goes further in.

## The preload checks its sub-allocations, unlike its arena

Past the first block, `func_02088590` repeats this shape:

```asm
bl   func_02089BDC
movs r0, #N
bl   func_02089B0C        ; or func_02089B28
cmp  r0, #0
bne  <skip>
movs r0, #3
blx  func_02024E9C        ; only on zero
```

and `func_02089B0C` is five lines:

```asm
ldr r1, [pc, #0x14]       ; the object at 0x02155494
ldr r1, [r1, #0x24]       ; +0x24 -- the SOUND ARENA handle docs/287 measured
blx func_02106F6C         ; (N, arena)
cmp r0, #0 -> return 1 or 0
```

So these are **sub-allocations out of the sound arena, and they are checked** —
unlike the two unchecked stores docs/287 found at the arena's own creation.

## The handler is a stripped varargs logger

`func_02024E9C` **ignores its argument**:

```asm
push {r0, r1, r2, r3}     ; varargs prologue
bl   func_0204C608
str  r0, [0x02143E38 + 4]
bl   func_0204C664
str  r0, [0x02143E38]
add  sp, sp, #0x10        ; varargs epilogue
```

The `#3` never reaches anything. In the retail ROM this is a printf-shaped
function whose body has been stripped, leaving only two words written to a
global pair. **So a failed sound allocation is genuinely swallowed** — nothing
is shown, nothing is retried, and the unit is simply silent.

**But it is not invisible.** Those two stores are the only thing that touches
`0x02143E38` on this path, so a write there is a signature for "a sound
allocation failed".

## The arena is NOT exhausted

The obvious hypothesis — early units load, the arena fills, later units fail —
was tested by sampling across the arena at `0x02193EAC .. 0x0223B6AC` in
gameplay:

```
+55%  0x021F0000   non-zero      +80%  0x02219EAC   all zero
+60%  0x021F852C   non-zero      +90%  0x0222AC2C   all zero
+70%  0x0220932C   non-zero      tail  0x0223B000   all zero
```

Filled to roughly three-quarters, with 150–200 KB of contiguous zero at the end.
An allocator failing for want of space in that arena is hard to credit, so that
hypothesis is weakened rather than confirmed.

## The instrument, and why this run did not get one

A write watch on `0x02143E38` answers it directly. `LB_WATCH=2143E38:2143E40`
with `LB_WATCH_RW=w` is the right form — `%x:%x`, no `0x`, and it arms silently,
so the absence of a confirmation line is normal rather than a failure.

The run is what failed: **fps 15.5 and zero `[lb_shroud]` lines, so it never
reached a battle.** The watch tests every bus access, and the route cannot
survive that cost. Reporting "0 hits" from it would have been meaningless.

**Next step, specified:** arm the watch *after* the route reaches gameplay
rather than at startup, so the expensive part covers only the window where
sounds are being allocated. `LB_WATCH` is read once on first use, so this needs
either a debug-server command to set the range live, or a route that starts the
process with the watch already armed and a much larger time budget.

## Status

Item 2 is **open**, and better understood: the failure path exists, is checked,
is silent, and writes a detectable signature. Whether it fires has not been
measured, and the arena being three-quarters full argues it may not — in which
case the silence is downstream of loading, in triggering or mixing, and that is
a different search.

## MEASURED: the failure handler never runs

The watchpoint was the wrong instrument and unnecessary. `func_02024E9C` WRITES
to `0x02143E38`, and the ARM9 image holds `FF FF FF FF` there, so its initial
RAM value is `0xFFFFFFFF`. Reading it before and after a match costs nothing and
answers the same question: if the pair has changed, the handler ran.

Two matches, two tilesets, each with a 22–24 second linger in gameplay:

```
Castle (default route)   02143E38  FFFFFFFF FFFFFFFF   before and after
Mars   (--map-index 1)   02143E38  FFFFFFFF FFFFFFFF   before and after
```

**Unchanged.** No sound sub-allocation failed in either match.

### The control, which the first attempt failed

The first run of this reported the same "unchanged" and it meant nothing: `peak
0/2048 polygons` and zero `[lb_shroud]` lines, so **no battle was ever played**.
The cause was a **stray `nds_runner.exe`** holding the debug port — the failure
mode the project's notes already record, where a leftover instance answers the
probe and fakes a broken read. fps 15.4 against a healthy 52.9 was the tell.

The runs above were confirmed first: 33 `[lb_shroud]` lines and `peak 74/2048
polygons` in each, so a real battle with real geometry.

## Where this leaves item 2

**CLOSED, that branch.** The allocation hypothesis is dead on two counts, both
measured: the arena is only about three-quarters used, and the handler that
fires on a failed sub-allocation never fires. Sounds are not missing because
they could not be allocated.

**So the silence is downstream of loading** — in what triggers a sound for a
unit, or in what mixes it — and that is a different search with different
instruments.

**Still not established:** that this route requests the same sounds the silent
units do. The handler firing zero times proves no *requested* allocation failed;
a sound that is never requested at all would look identical from here, and
"never requested" is a live hypothesis for units whose blueprint sound id the
port may not be resolving.
