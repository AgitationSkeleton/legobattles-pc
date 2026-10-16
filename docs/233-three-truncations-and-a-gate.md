# 233 — Three truncations and a gate

**Goal within the project:** the adaptive window's interaction requirement — a click in
the widened band selecting a unit. docs/232 established that an out-of-range
touch x is not refused but *relocated*, landing at column 251. This document
finds why, in four places, and fixes three of them outright.

The method throughout is the same and is worth stating once: **follow the
value.** Click at native +300 — a distinctive number, and out of range in the
same way a negative is — and watch main RAM for writes of 300. Every fix pushes
the value one step further along the chain, and the trace says exactly where it
stops. No step of this was inferred from the disassembly alone.

## Where it stopped, four times

| after | the value reaches | stopped at |
|---|---|---|
| nothing | `0203DD1C` writes `0215376A` | the touch struct only |
| the clamp removed | `0200AA2C` (arg spill), `0200AA60` writes the event | the event object |
| the hit test fixed | `020E3EB0`, `020E3F68`, `020E14C4` | the per-widget containment test |
| the gate removed | `020A87FC`, `020A8800`, `020A880E`, `020A8812` | the selection rectangle |

Each row is a measurement that changed, not a claim.

## 1. The touch pump posts its event twice, and one post was unpatched

`func_0203DCB8` builds a touch event and posts it in two places, with
byte-identical instruction pairs:

```
0203DE40  BL 020097F8            get the event queue
0203DE4C  LDRH r1, [r2, #0x0A]   X          <- patched earlier
0203DE50  LDRH r2, [r2, #0x0C]   Y
0203DE58  BL 0200A350            build the event

0203DF4C  BL 020097F8
0203DF58  LDRH r1, [r2, #0x0A]   X          <- NOT patched
0203DF5C  LDRH r2, [r2, #0x0C]   Y          <- NOT patched
0203DF64  BL 0200A350
```

`r2` at the second post comes from the literal at `0203DF88`, which is
`0x02153760` — `kTouchBase` itself — so there is no ambiguity about what is
being read. The second post carries a constant event kind of 3, the held/move
event.

Both posts' X and Y are sign-extended now. This alone changed nothing
measurable, which is worth recording: it was necessary but not sufficient.

## 2. The clamp: `4 <= x <= 251`, with an unsigned compare

`FUN_0200A350` builds the touch event. Its x argument sits in r5:

```
0200A3B8  cmp   r5, #4      E3550004
0200A3BC  movcc r5, #4      33A05004
0200A3C0  cmp   r5, #0xFB   E35500FB      251
0200A3C4  movhi r5, #0xFB   83A050FB
```

`movhi` is **unsigned** higher. It does not merely bound large positives — a
negative x is a very large unsigned number and takes the same branch. So every
out-of-range click, in either direction, arrived at the pick as 251.

That is precisely the measured behaviour of docs/232: clicking native −150 and
clicking native +300 both selected a base parked at 240, neither selected one at
200 or below, and the landing point bracketed to a little under 255. 251 fits
the bracket exactly.

Only the upper `mov` is replaced with a no-op. The lower one fires on an
unsigned compare against 4, which a negative x never satisfies, so it is vanilla
behaviour for a real stylus. And the upper bound is not replaced with a wider
constant because the right bound depends on the window — the port already bounds
this coordinate itself, since the only thing that writes the slot is the
enhanced touch mapping, which cannot produce an x outside the surface it drew.

**Verified by A/B:** clicking native +300 selected a base parked at 240 two
times in four before the patch, and zero in four after.

## 3. The hit test truncated to eight bits

With the clamp gone, the next thing to touch the coordinate is `FUN_020E489C`,
which hands it to the widget hit test:

```
020E498E  8BB8  ldrh r0, [r7, #0x1C]    event x
020E4990  8BF9  ldrh r1, [r7, #0x1E]    event y
020E4994  0600  lsl  r0, r0, #24
020E4996  0609  lsl  r1, r1, #24
020E4998  0E00  lsr  r0, r0, #24        zero-extend the low byte
020E499A  0E09  lsr  r1, r1, #24
020E499E  bl    020E3EAC
```

`lsl #24` then `lsr #24` is `& 0xFF`, so native −150 was offered to the hit test
as 106 and +300 as 44 — both plausible in-panel positions, so a band click could
spuriously hit a HUD button nowhere near it.

The fix keeps the same two instruction slots and changes the shift: `lsl #16`
then `asr #16` is a 16-bit sign extension. **ARMv5TE Thumb has no `SXTH`** —
that is an ARMv6 instruction — so the shift pair is how this is written on this
core, not a workaround.

Safe for out-of-range values: `FUN_020E3EAC` walks a linked list of widgets and
calls a containment test on each, indexing nothing by the coordinate, so a
negative x simply fails to be inside any rectangle. That is the correct answer
for a click out in the band.

## 4. The gate: a test that the x fits in a byte

`FUN_020A8794` enters the field-selection path only when four conditions hold,
and the fourth is:

```
020A87D8  7F61  ldrb r1, [r4, #0x1D]
020A87DA  2900  cmp  r1, #0
020A87DC  D120  bne  <skip the whole field branch>
```

**The event's x is a sixteen-bit field at +0x1C.** That is not inferred from the
decompiler: `FUN_0200AA2C` stores it with `strh r5, [r7, #0x1C]`, and a
value-filtered watch that also reports the ADDRESS shows that store landing at
`0230BD2C`, `0230B0B8` and friends — heap event objects at base + 0x1C. So
+0x1D is that x's **high byte**, and this branch is a test that the coordinate
fits in 0..255.

In vanilla it always does, so the branch is never taken and the test reads as
dead code. With the widened field it is taken for every click outside the native
panel, in either direction — which is exactly the symmetry docs/232 measured
without being able to explain it.

**Measured, not deduced.** A read watch on the two loads in the guard shows both
firing for in-band *and* band clicks, so execution reaches the test in both
cases; the reads inside the branch — which fetch the touch coordinate to build
the selection rectangle — fired only for in-band clicks. Nothing sits between
them but this branch. After the patch those inner reads fire eight times for a
band trial where they fired zero before.

## What is still wrong

**The band still selects 0 of 4.** The click now enters the field-selection path
out in the band and the rectangle is built from the true coordinate, but the
click handler at `020E2408` is still not reached.

The most likely next gate is named rather than guessed. `FUN_020A9898`, called
from inside the field branch to build the event's classification flags, tests
the same `+0x1D` byte **thirteen times** — at `020A98B4`, `020A98D8`,
`020A98F2`, `020A990C`, `020A992A`, `020A9956`, `020A9962`, `020A998C`,
`020A99AC`, `020A99CC`, `020A99F0`, `020A9A14` and `020A9A38`. If the flags come
out wrong, `FUN_020AA0A4`'s dispatch matches a different handler and the
field-click delegate is never invoked.

There was a cheaper configuration to test first, and it inverted the fix. The
event's x is used for two different things: the classification flags and the
widget hit test read it, while the selection **rectangle** is built from the
touch struct at `0215376A` instead. So keeping the event's x in range — letting
the clamp stand — while the rectangle carries the true value might have been the
correct shape of the whole fix, with the clamp removal above being the wrong
instinct.

`LB_TOUCHSKIP=<hex address>,...` leaves named sites at stock, so that A/B runs
in one binary rather than across two builds. A/B-ing by rebuilding twice is how
this project has already answered a new question with a stale binary.

**Tested, and refuted.** With `LB_TOUCHSKIP=200A3C4` the event's x is clamped to
251 again, so `+0x1D` is zero and every one of `FUN_020A9898`'s thirteen tests
passes — and the band still selects 0 of 4, with `020E2408` still not reached.
So the flags are not the gate either, at least not through that byte, and the
inverted configuration is no better than the direct one.

That is a useful exclusion: it means the remaining divergence is **not** carried
by the event's x at all. Both configurations deliver identical event x values
for in-band and band clicks in every field the classification reads, and the two
still route to different delegates.

## The dispatch, and the flag word that turned out to be identical

`FUN_020AA0A4` walks a list of handlers and matches each one's mask at
`node+0x0C` against a flag word built on the stack:

```
020AA0AC  ldr r4,[r0,#0x10]     the handler list head
020AA0B2  mov r0,r4 ; add r0,#0x0C
020AA0B8  bl  020AA004          mask == (flags & mask) ?
020AA0BE  beq -> skip
020AA0C4  bl  020AA0FC          matched: arm this delegate
020AA0CA  ldr r4,[r4,#4]        next
```

Watching the entry pushes of the deep links says exactly where a band click
stops:

| function | in band | band |
|---|---|---|
| `020AA0FC` (a handler matched) | runs | **never** |
| `020DD3B4` | runs | never |
| `020E3518` | runs | never |

So no handler matches for a band click. The obvious next question is which bit
of the flag word differs — and the answer is **none of them**.

`FUN_020A9F80` is the bit setter (`str r1, [r0, r2]` at `020A9FA0`), so watching
that one PC shows the flag word being built, with its address and its running
value. For the field path — the calls whose LR is inside `FUN_020A9898` — it
reads identically in both cases:

```
in band:  08000000 -> 08000010 -> 08010010 -> 08030010   at 027E35BC
band:     08000000 -> 08000010 -> 08010010 -> 08030010   at 027E35BC
```

Same address, same four writes, same call sites, same final `0x08030010`. The
classification is not what diverges.

## An instrument limit that nearly produced a false answer

The next step was to compare the handler MASKS tried in each case, by watching
the other load in the compare loop (`020AA010`). That run reported the two cases
walking **completely disjoint** lists — nodes at `02245370..02245970` in band and
`02245AF0..022460F0` out of it — which reads like a smoking gun: two different
sets of registered handlers, and the band case simply lacking the one whose mask
is `0x00000010` (the only mask in either list that is a subset of `0x08030010`).

It is not a smoking gun. **The watch ring holds 512 events**, and that watch
records 40 handlers × 16 passes = 640 reads per drain. Each drain returns the
last 512, so the two "lists" are two different *windows* of the same walk, and
their disjointness is an artefact of where each window happened to start.

Recorded rather than acted on, because this is exactly the shape of the four
instrument faults in docs/228 — a plausible reading of output that the
instrument could not have produced correctly. The chain measurement above is
unaffected: it filters on four rarely-hit PCs, so its ring never fills.

## The ring was raised, and the answer was one bit

`kWatchSize` is 16384 now (was 512), and the debug server's single-reply cap is
8192. With that, the mask comparison is trustworthy: **the two cases walk the
same 40 handler nodes**, all shared, so the disjoint-list reading was purely the
artefact described above.

Then the load that only executes on a match — `ldr r1,[r4,#8]` at `020AA0C0` —
names the matching handler directly. It fires twice for an in-band trial and
**never** for a band one, and the node it reads has mask **`0x00000040`**.

Watching the bit setter (`str r1,[r0,r2]` at `020A9FA0`) shows where that bit
does and does not get set:

```
in band  ... 04000400 (lr 020A9BC9)   00004040 (lr 020A9C09)   04040400
             04060400   06060400   then 000040C0, 000041C0
band     ... 04008000 (lr 020A9727)                    then 00004080, 00004180
```

`0x41C0` has bit `0x40`; `0x4180` does not. **One bit, and it decides the whole
dispatch.** The in-band click takes an arm of the classifier that the band click
never reaches.

## A wrong fix, caught by its own A/B

The obvious reading was that those arms are guarded by the same
`event->x_hi == 0` test as the gate, written out thirteen times by the compiler
between `020A98B4` and `020A9A38`. Patching all thirteen looked safe by the same
argument that justified the gate: the byte is a 16-bit x's high half, so in
vanilla it is always zero and the test always passes.

**It is not safe, and the A/B said so immediately.** With all thirteen patched
every trial reported "0 of 0 clean attempts" — the deselect stopped working, so
nothing was measured at all. Skipping exactly those thirteen through
`LB_TOUCHSKIP`, in the same binary, restored the control to 2 of 4.

The reason is the caution recorded above and then ignored: **115 sites in the
ARM9 test `[rX, #0x1D]` against zero**, because 0x1D is a common struct offset.
Some of the thirteen are testing a different object's field, and neutralising
them changed behaviour for ordinary in-range clicks. They are removed.

## Where the search actually goes now

Disassembling the arm that sets bit `0x40` shows it is not guarded by a byte
test at all:

```
020A9B7C  push {r3-r7,lr}            a helper taking an entity handle
020A9B86  bl 0208727E                validate the handle
020A9B8A  cmp r0,#0 ; bne            invalid -> return 0
020A9B9A  bl 020875D6                the local player
020A9BA8  bl 02087390                the entity's owner
020A9BB0  bl 02085D58                is it mine?
   ...
020A9C04  bl 020A9F80  r1=0x46       set bit 0x46 -> word 2, bit 6 = 0x40
```

`0x46 >> 5 = 2` and `0x46 & 0x1F = 6`, so bit `0x46` is bit `0x40` of word 2 —
which is `flags + 8`, exactly the `027E356C` the trace watched.

So the arm runs when **an entity is under the cursor and owned by the player**,
and it does not run for a band click because *no entity handle is found there*.
The remaining defect is in the coordinate-to-entity lookup that supplies that
handle, not in any flag, byte or branch on the way to it.

That is a better place to be than a byte test, and it is consistent with
everything above: the coordinate now arrives correctly at every stage, and the
first thing that actually *uses* it geometrically still cannot find what is
standing there.


## A caution about offset 0x1D

A scan of the whole ARM9 finds **115** sites testing `[rX, #0x1D]` against zero.
It is a common struct offset, not a touch-specific one, so "it is the x high
byte" is true for *this* event object and must not be assumed anywhere else. The
evidence for this one is the `strh` at +0x1C and the watched address of that
store, both specific to the touch event.
