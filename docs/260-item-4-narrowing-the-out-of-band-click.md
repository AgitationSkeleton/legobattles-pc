# 260 — Item #4: three blockers eliminated, and the previous suspect cleared

The defect: clicks and drags outside the original DS aspect landed at the nearest point inside it, so a unit ordered to move outside the aspect moved to its edge instead.

## Where this already stood

A chain of three earlier documents, each correcting the last:

- **docs/217** — out-of-band clicks do not select. *Retracted in part.*
- **docs/218** — they do (4/12 on the base). *Retracted:* the base's hit area
  covers native x 0, and a clamped click lands at x 0, so it was hit whether or
  not the extended coordinate did anything.
- **docs/224** — redone against the **builder**, which stands still and whose
  hit area a clamp cannot reach. In band `hud 0 -> 6 SELECTED`; out of band
  `hud 0 -> 0 nothing`, with the delivered slot holding **-38**.

So the settled position is: **the coordinate arrives and the cartridge does not
act on it.** docs/224 left a specific next step, and this document follows it.

## The previous suspect is cleared

docs/224 ended:

> `func_020a8794` reads the touch x twice — `020A8800` is `ldrsh` and feeds the
> rectangle builder, `020A8812` is `ldrh`, zero-extends, and stashes at
> `[obj+0x20]`. A negative x survives the first and becomes 65498 through the
> second.

Disassembled as Thumb (the function is Thumb; decoding it as ARM produces
garbage, which is worth knowing before anyone tries again):

```
020a87fc  ldrsh  r2, [r1, r0]
020a8800  ldrsh  r1, [r1, r0]     <- signed, feeds the rect builder
020a8808  bl     #0x20aa8e4
020a880e  ldrh   r1, [r0]
020a8812  ldrh   r0, [r0]         <- the suspect
020a8814  strh   r0, [r5, #0x20]  <- ...immediately truncated to 16 bits
020a8816  strh   r1, [r5, #0x22]
```

**The zero-extension is harmless.** `strh` stores the low halfword, so -38 and
65498 write the identical `0xFFDA`. `ldrh` and `ldrsh` cannot be distinguished
by what ends up at `[obj+0x20]`. That suspect is eliminated, and the remaining
half of docs/224's note — "find what reads `[obj+0x20]` back" — is the part that
still matters, because the sign only reappears at the read.

## Two more eliminated

**The rectangle builder is clean.** `func_020aa8e4`, which receives the
*signed* x from `020A8800`:

```
020aa8e4  cmp   r1, r3
020aa8e6  ble   #0x20aa8ee
020aa8e8  strh  r1, [r0, #0x18]     ; max x
020aa8ea  strh  r3, [r0, #0x10]     ; min x
          ...same again for y at +0x12 / +0x1a
```

A pure min/max normaliser on **signed** compares, with no bounds test and no
clamp. A negative coordinate is stored correctly as the rect's minimum.

**The entity gather rect is widened, on both edges.** `apply_cull_width`
rewrites the immediate at `0x02038FD0` for the right edge and replaces the
three-instruction left-margin block with a clamped `col - 5`, because editing
the literal alone would underflow a camera in the first four columns. So
entities outside the native 256 are in the list and can be hit-tested.

## What that leaves

The coordinate is **delivered** (slot = -38), **stored** correctly as a signed
halfword, **normalised** correctly into the selection rect at `obj+0x10..0x1a`,
and the entities out there **are gathered**. The failure is downstream of all
four.

The next thing to find is what consumes that rect — a reader of `[obj+0x10]`
and `[obj+0x18]` — and whether it re-reads with `ldrh` somewhere, which is
where a stored `0xFFDA` would finally become 65498 and put the rect's minimum
far to the right of its maximum. That is a single-value search rather than the
open-ended hunt it looked like at the start of this document.

## The consumer is a callback, which is why the call graph keeps dead-ending

Following the object rather than guessing at it. In `func_020dbc14`, one of the
two callers of the touch handler, the object is not an entity at all:

```
020dbc2a  bl   #0x20aa73c        ; construct, on   add r0, sp, #0x40
020dbc36  bl   #0x20a8794        ; fill the rect
020dbc3e  bl   #0x20aa8d4        ; <- consume it
020dbc44  bl   #0x20aa774        ; destruct
```

It is a **stack local at `sp+0x40`**, constructed and destroyed around the
click. And the consumer is dynamic dispatch:

```
020aa8d4  ldr  r2, [r0, #4]      ; a function pointer in the object
020aa8d8  cmp  r2, #0
020aa8da  beq  #0x20aa8e0
020aa8dc  ldr  r1, [r0, #8]      ; its bound argument
020aa8de  blx  r2
```

So "find what reads `[obj+0x10]`" has no static answer: the reader is whatever
`[obj+4]` happens to hold, set by whoever configured the object. That is the
same wall docs/253 hit on the fog edges ("the static call graph exhausted"),
and it is worth naming as a pattern in this codebase rather than rediscovering
it a third time.

**Next step, and it is a runtime one:** read `[obj+4]` at the moment of an
out-of-band click — the object's address is `sp+0x40` inside `func_020dbc14`,
so a watch on the `blx r2` at `0x020AA8DE` gives the target directly. The port
already has the machinery (`deep_trace`, the write-watch ring). With that
address the reader is a normal disassembly job, and the `ldrh`-vs-`ldrsh`
question can finally be asked of the right instruction.

## Correction: this chain is input BINDING, not unit picking

Followed it to the end statically, and it does not do what the previous section
assumed. `func_020aa7a4` — called both by the constructor and at the **start**
of `func_020a8794` — *clears* the callback:

```
020aa7a4  movs r1, #0
020aa7a6  str  r1, [r0, #4]
020aa7a8  str  r1, [r0, #8]
```

So the handler starts with no action and installs one only if the click
resolves. It installs it via `func_020aa0a4` (called at `020a881c`), which walks
a linked list and, for each node, runs `func_020aa004(node+0xc, query)` as the
test — installing the callback from a table only on a hit.

`func_020aa004` is **not** a geometric test:

```
020aa00a  ldr  r5, [r2]      ; word count
020aa010  ldr  r3, [r0]      ; candidate's required mask
020aa012  ldr  r2, [r1]      ; query's mask
020aa014  ands r2, r3
020aa016  cmp  r3, r2
020aa018  beq  ...           ; every required bit must be present, else 0
```

A bitmask subset test over N words. It compares **input/gesture flags**, and
never looks at a coordinate. So this chain maps an input event to a handler;
the unit picking happens inside whatever handler is then invoked.

**That is a wrong turn, recorded so it is not taken again.** The rect at
`obj+0x10..0x1a` is built here and carried, but nothing on this path
hit-tests it — which also explains why no `ldrh`/`ldrsh` asymmetry turned up
anywhere along it.

The static route genuinely ends here: the picker is behind the `blx r2` at
`0x020AA8DE`, and its identity is data, not code. The runtime observation is
required after all — watch that branch during an out-of-band click, take the
target, and start again from there.
