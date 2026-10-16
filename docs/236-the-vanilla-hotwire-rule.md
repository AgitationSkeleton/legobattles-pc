# 236 — The vanilla hotwire rule, located

**Goal within the project:** the cheat settings include a
**let-anything-hotwire option**. That option has been in the panel for a long
time as a combo entry whose accessor **no code called** — a selectable choice
that did nothing, which the project does not allow: no ImGui setting or button
may remain unwired or incomplete.

It was unwired for a stated reason, and the reason was wrong.

## The retraction

This panel, and `docs/125`, `docs/134`, `docs/143` and `docs/148`, all recorded
some version of:

> the vanilla "specials only" rule is in the targeting layer, **not yet
> located**

It is located. It was found by asking the question from the data side rather
than the code side: the port computes an entity's category as
`[[[e+0x160]+8]+0x5C]`, so a scan of the whole ARM9 image for **`LDRB Rd,
[Rn,#0x5C]` followed within six instructions by `CMP Rd, #imm`** finds every
site in the game that tests a category against a constant. There are 200, of
which 20 compare against 6 — and one of those, `0x0207B7A8`, sits in the same
compilation unit as the spell factory.

## What it actually says

`func_0207B73C(?, caster, order, target)` returns 0 to refuse a cast:

```asm
0207B790  ldrb  r1, [r7]           ; the order's effect id
0207B798  cmp   r1, #0x20          ; 32 == HotWire
0207B79C  bne   0207B7FC           ; every other spell: not our business
0207B7A0  ldr   r0, [sb, #0x160]   ; target->definition
0207B7A4  ldr   r0, [r0, #8]       ;         ->info
0207B7A8  ldrb  r0, [r0, #0x5c]    ;         ->category          (docs/72)
0207B7AC  cmp   r0, #6             ; Siege/Special
0207B7B0  bne   0207B7FC           ; ANYTHING ELSE FALLS THROUGH -- ALLOWED
0207B7B4  blx   func_02045AE8      ; a mode gate: [r0+0xC] must be non-zero
0207B7C8  bl    func_020875D4      ; the caster's player, from caster+0xE4
0207B7DC  bl    func_020859F4      ; sb = category-6 units this player owns
0207B7E8  bl    func_02085DB0      ; r0 = that player's farm allowance
0207B7EC  cmp   r0, sb
0207B7F0  addle sp, sp, #4         ; allowance <= owned  ->  REFUSE
0207B7F4  movle r0, #0
0207B7F8  pople {r3-r8, sb, sl, pc}
```

**32 is not a guess.** `func_0207BC0C` is the spell-effect factory, a 35-arm
jump table on the same id byte; arm 32 is the one that allocates 0x64 bytes and
calls `func_02078C18`, and `func_02078C18` is the only function in the image
that stores vtable `0x0214A920`, which `docs/105` identified as
`HotWireSpell`.

`func_020859F4(player, category, live)` and `func_02085DB0` are not guesses
either — `docs/58` named both while working out what a Farm does.

## So there is no whitelist

> **Corrected by docs/392: there is one, in the stylus tap.** The armed-spell
> tap handler `func_020A9350` filters a hotwire's targets with tag 0x2B
> (enemy) and tag 0x3B, which `func_020A9B7C` gives only to categories 5 and
> 6. What made "everything falls through" look true is the glitch in
> `func_020A9570`: it ORs the tags of every entity under the stylus and keeps
> the last one's handle, so a transport or special under the stylus lets the
> steal land on whatever was scanned after it. The validator below adds only
> the farm cap. "Hotwire anything" now also turns the 0x3B tag into 0x2B
> (0x020A945A). The paragraphs below are kept as written.

**Vanilla keeps no list of hotwirable categories.** It restricts exactly one
thing: stealing an enemy **Special** when your farms will not support another
one. Everything else — melee, ranged, mounted, transports, heroes — falls
through the `bne` at `0207B7B0` and is allowed as shipped.

That is why five documents failed to find a category filter. There is none to
find. The searches were looking for the wrong shape of rule.

## What the cheat therefore is

"Hotwire anything" now means the only thing it can mean against this code: **do
not let the farm allowance refuse the steal.** The three LE-conditional words at
`0207B7F0` are the entire refusal, they are reached only after `cmp r1, #0x20`
has established this is a hotwire, and falling through lands on `0207B7FC` —
the ordinary path a non-special target already takes. Replacing the three with
`NOP` is the smallest change that expresses it, and it cannot affect any other
spell.

It is applied **and removed** with the combo, so the toggle is live rather than
needing a restart, and it verifies all three stock words before writing so a
different build is left alone instead of corrupted.

## "Specials only" is a different mechanism, and stays one

Mode 1 is *additive*: it refuses ownership changes for non-specials inside
`Entity::SetOwner` while `HotWireSpell`'s tick is on the stack. That is
`docs/134`'s bracket and it is unaffected by any of this. The two modes are not
two halves of one rule — vanilla restricts by supply, mode 1 restricts by
category, and mode 2 removes the supply restriction.

## The method worth keeping

The rule had been hunted from the spell object outward — vtable, tick, slot 5,
constructor, factory — and every one of those is filter-free, which is what
made "the targeting layer" sound like a place that must exist somewhere else.
Scanning the image for *the field access itself* found it in one pass, because
whatever else a category rule does, it must read the category byte.

## Measured, both ways, on one binary

`LB_HOTWIRE_MODE=<0|1|2>` forces the combo without the UI — the same idiom as
`LB_FORCE_CAP`, `LB_PIXEL_SCALE` and `LB_HUDSCALE` — so the cheat can be checked
headlessly without writing anybody's `settings.json`. A Free Play battle, the
same build, the same route, only the environment differing:

```
$ lb_exp.py hwon  --route free --env LB_HOTWIRE_MODE=2 --peek 0207B7F0:C
  0207B7F0  E1A00000 E1A00000 E1A00000        three NOPs

$ lb_exp.py hwoff --route free --env LB_HOTWIRE_MODE=0 --peek 0207B7F0:C
  0207B7F0  D28DD004 D3A00000 D8BD87F8        stock: addle / movle / pople
```

**State exactly what that proves and no more.** It proves the refusal
instructions are gone when the cheat is on and present when it is off, live, in
a running battle, from one binary. It does not by itself show a hotwire cast
succeeding at the farm cap — that needs a scripted battle that reaches the cap
and casts, which is a harder rig than this. What it does remove is the only
thing that was ever wrong with this control: it is no longer a menu entry with
no code behind it.
