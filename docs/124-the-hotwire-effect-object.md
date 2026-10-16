# The HotWireEffect object

`docs/105` retracted `func_020872DC`, which had been wired as
"HotWire_ResolveTarget" and is really `ResolveLiveEntity` with eighty callers.
That retraction left the hotwire cheat with **no located code at all**. This
finds the real object.

## The class

```
typeinfo  Battles::HotWireEffect   021434B8
vtable                             021434E8
constructor                        0201C100
factory                            0201C15C   (allocates 0x5C bytes, heap tag 0x91)
```

So a `HotWireEffect` is **92 bytes**. The constructor initialises two containers
(`func_020F27B8` at `+0x10` and `+0x28`), a third object at `+0x38`, stores the
caller's argument at `+0x0C`, and rewrites the vtable pointer twice -- the usual
base-then-derived construction.

## The tick is a five-state machine

`virtual_04` (`0201BF88`) switches on `[this+0x48]` through a five-entry jump
table. State 0 is the interesting one:

```asm
add   r0, r4, #0x41
bl    02087350              ; resolve the target handle at [this+0x41]
cmp   r0, #0
beq   0201C000              ; unresolved -> generic effect
ldr   r0, [r0, #0xE0]       ; the kind object
ldr   r0, [r0, #4]
ldrh  r0, [r0, #4]          ; a u16 type id
cmp   r0, #0xB1             ; <- the only type comparison in the effect
mov   r0, #0x1B  /  #0x1C   ; effect id: 0x1B if type 0xB1, else 0x1C
bl    02016BFC              ; spawn it        -> [this+0x50]
...
bl    02016AE4              ; second object   -> [this+0x54]
mov   r0, #0x64
str   r0, [r4, #0x58]       ; timer = 100 ticks
mov   r0, #1
str   r0, [r4, #0x48]       ; -> state 1
```

State 1 (`0201C05C`) decrements `[this+0x58]` and waits for zero. Two small
helpers at `0201C18C` and `0201C19C` set bits 4 and 2 of a flag byte at
`[this+0x40]`.

Field map so far:

```
+0x0C  ctor argument        +0x48  state
+0x40  flag byte            +0x50  spawned effect A
+0x41  target handle        +0x54  spawned effect B
+0x43  a second handle      +0x58  countdown, starts at 100
```

## What the 0xB1 test is, and what it is not

`cmp r0, #0xB1` is **presentation**, not permission. Both arms proceed; they
differ only in which effect id is spawned, `0x1B` or `0x1C`. It selects an
animation for the target's type -- plausibly building versus unit -- and nothing
in this object refuses a target.

**So the "can this be hotwired" predicate is not in `HotWireEffect`.** By the
time this object exists the decision has already been made, which means the test
lives in the caster or in the spell's targeting, and neither is located. The
factory at `0201C15C` has no `BL` callers, so it is reached from Thumb or through
a vtable, and the trail continues there.

## A correction adjacent to docs/105

`docs/105` was right that `func_020872DC` is not the hotwire target resolver.
The neighbourhood was right, though: **`func_02087350`** -- 116 bytes further on
-- *is* the resolver `HotWireEffect` uses on its target handle. The earlier
mistake was picking the wrong function out of the right region and then wiring
an override to it.

## Status of the setting

`lb_setting_hotwire_anything` still has **no consumer**, and this document does
not add one. What it adds is the object, its layout, its state machine, and the
knowledge that the predicate is upstream of it -- which is where the next look
goes. Guessing at `0xB1` as the restriction and inverting it would be wrong: it
is a presentation branch, and treating it as a gate is precisely the error
`docs/105` was written about.
