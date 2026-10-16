# Staging works, and immediately found a defect in docs/132

> **The "defect" is WRONG, corrected by
> [docs/141](141-the-override-is-on-the-right-function.md).** func_0205E7F4 IS
> the hp setter: the write at 0205EC98 is 1188 bytes inside that same function,
> with no other prologue between them. The override is on the correct address.
> Everything else here stands -- lb_stage.py works, and damaging a unit does
> produce healing, which is what docs/139 denied.

## tools/lb_stage.py

Lists and edits the live entity pool through the debug server. In a warped Free
Play match:

```
i   entity    owner category   hp     handle
9   0230886C  1     Hero       1100   09.00
10  02308AB0  1     Builder     160   0A.00
11  02308CF4  1     Base       1500   0B.00
13  022DDAF4  0     Base       1500   0D.00
14  022DDDE8  0     Builder     150   0E.00
15  02309F6C  0     Hero       1000   0F.00

6 live entities, owners 0, 1
```

Two players, each with a Hero, a Builder and a Base. Pool slots 0-8 and 12 are
empty -- category **229** is an unused slot, not a unit, which is worth knowing
before anyone reads that number as a category.

Every field it decodes was established elsewhere in this project rather than
guessed: handle `+0xDC`/`+0xDD`, owner `+0xE4`, transport `+0x114`, blueprint
`+0x160` with category at `[+8]+0x5C`, hp `+0x1A0`.

## The harness CAN reach a healing situation

`docs/139` concluded the harness "can reach a match, but not a situation", and
listed three settings blocked on that. **That is wrong for healing.** Writing a
hero's hp down and letting the game run:

```
entity 15 hp: 1000 -> 400      (written)
...60M cycles...
entity 15 hp: 550              (the game healed it)
```

Damaging a unit produces healing on its own. No spell, no aura, no combat
needed. The situation was one write away the whole time.

## And that immediately found a defect

With `LB_TRACE_HEAL=1` and the setting on, the same run produced **zero** trace
lines. The healing does **not** pass through `func_0205E7F4`, the function
`docs/132`'s override hooks.

Watching the hero's hp word for writes only -- `LB_WATCH_RW=w`, the filter from
`docs/122` -- names the real writer:

```
pc=0205EC98  width=16  x12       <- the healer
pc=01FF8260  width=16  x4        <- the tilemap DMA, incidental
```

and `0205EC98` is:

```asm
ldr  r0, [r6, #0x160]      ; blueprint
ldr  r0, [r0, #8]          ; info block
ldrh r0, [r0, #0x62]       ; MAX hp  (docs/95's maxhp_62)
cmp  r5, r0 ; bne ...      ; if the new value is max, notify
add  r0, r6, #0x100
strh r5, [r0, #0xa0]       ; entity+0x1A0 = the new hp   <<<
ldrh r0, [r0, #0xa0]
cmp  r0, #0                ; if it is now zero, fall into the death path
```

That is the **generic hp setter** -- it knows max hp and it knows death.
`func_0205E7F4` is not it; whatever `docs/95` saw there, this path does not use
it.

## Consequence for docs/132

**The Fix Hotwired Healing override hooks a function the healing does not call.**
It is not merely unverified, as `docs/133` concluded from silence -- it is
attached to the wrong place, and the silence was the evidence, misread as
"the scenario never runs".

The fix is to hook the setter this document identifies instead, keeping the
bracket design: `docs/132`'s reasoning about owner comparison and containment
stands, only the address is wrong. That needs the setter's own entry point,
which is the next read: the write at `0205EC98` sits inside a larger function
whose prologue is above `0205EB80` and was not located this iteration.

## Method note

`docs/139` asserted a limitation -- "cannot reach a situation" -- and planned
work around it. One `write_mem` disproved it. The tool built to work around the
limitation is what showed the limitation was not real, and then found a shipped
defect in the first five minutes of use.
