# 279 — The settings crash audit

Goal: no imgui setting (music, hotwire changes, rebalance and the rest) can
crash the game, shown with evidence backed by the decompiled code.

## The one that crashes, and why

`fix_hotwired_healing`. Bisected against a real run rather than read off the
code — the first attribution, made from log adjacency, blamed `music_mode` and
was wrong (the music line simply printed immediately before the halt).

The evidence is a single-variable A/B. The exact settings of the crashing
configuration, changing only this one:

| run | `fix_hotwired_healing` | `music_mode` | result |
|-----|------------------------|--------------|--------|
| crash2 | **true** | 2 | frozen, 1 dispatch miss at `0x020872DC` |
| bisectA | **false** | 2 | 57.4 fps, **0 misses** |

So faction-aligned music is innocent and the healing fix is the crasher.

The mechanism, from the code: `heal_tick_override` (`lb_patches.cpp`) is gated on
`lb_setting_fix_hotwired_healing()` and calls the guest directly —

```c
if (lb_setting_fix_hotwired_healing() && spell) {
    g_cpu.R[0] = spell + 0x18u;
    runtime_call_push_return(kResolveLiveEntity);   // 0x020872DC
    g_cpu.R[14] = kResolveLiveEntity;
    g_cpu.R[15] = kResolveLiveEntity;
    runtime_dispatch(kResolveLiveEntity);
```

— and `runtime_dispatch_miss()` ends in `nds_halt("dispatch miss")`, so a miss is
fatal by design rather than degraded.

**Why the dispatch misses is NOT established.** What has been ruled out:

* The entry point exists. `0x020872DC` is in `lb_arm9.toml` with `size = 0x2C`,
  `mode = "arm"`, `name = "HotWire_ResolveTarget"`, and it is in the emitted
  table (`lb_arm9_dispatch.c`).
* The lifted bytes are right. Its `NdsStaticValidation` record holds 44 bytes
  which match `section_static_02000000.bin` at that address **exactly**.
* Stale bank objects. `build.sh` passes `-DNDS_TITLE_BANK_DIR` and the 32-minute
  rebuild after the re-lift recompiled them.
* The port's own music writes. `kMusicState` (`0x0214B41C`) and `kMusicRt`
  (`0x02155494`) are not in that page.

**RETRACTED (docs/280, docs/281): the validation gate cannot be the mechanism.**
This document previously reasoned that `lookup_in()` skips an entry whose live
bytes differ from the lifted ones, so the live RAM must differ at dispatch time.
Reading the runner settles it: a skipped entry is recorded as
`inactive_candidate`, `lookup_static_cached` returns `nullptr`, and a null
lookup is the **Tier-3 interpreter** path, not a halt -- its own comment reads
"Copied RAM code has no static entry and is handled by Tier 3." That is how the
eight `kTouchPatches` code patches already work. A validation mismatch degrades
to interpretation; it does not miss.

What `runtime_dispatch_miss` actually is: the `default:` arm of the **resume
switch** every lifted function carries (`recompiler/src/main.cpp`,
`emit_resume_switch` and the `emit_entry_switch` block). It fires when control is
already inside a lifted function and `R[15]` matches none of its interior
instruction addresses. So the question to ask is not "why was the entry
rejected" but "which function was executing, and why was R[15] outside it".

The Thumb-bit candidate raised here is unexercised for a different reason than
stated: the filter is real, but it too results in a skip, and a skip means
Tier 3.

## Everything else, cleared as a group

One run with every other enhancement on at once — the point being that if the
group is clean, no member of it crashes on this path, and if it is not, the
bisection starts from a known set:

```
enhanced=1  hotwire_mode=2  hotwire_transport=1  attack_boost_ranged=1
same_builders=1  balance_patch=1  unlock_units=1  unlock_extras=1
music_mode=2    fix_hotwired_healing=0
```

Result: **59.8 fps, 0 dispatch misses, 28 patches applied.**

## What this audit does NOT cover, stated plainly

The route reaches a Free Play match and idles. **It never reaches combat**, and
the reported crash happened once combat began — which is precisely when a heal
spell can tick. So:

* The group above is clear for menu, load, and idle play.
* It is **not** clear for combat, for any of them.
* The one setting whose consumer only runs in combat is the one already known to
  crash, which is not a coincidence worth ignoring: a setting whose code path
  never executes cannot be shown safe by a run that never executes it.

Closing this item properly needs a route that fights — units in contact, spells
cast — with each setting group on. Until then this document is a partial
result, and the honest claim is "no setting except `fix_hotwired_healing` has
been observed to crash on the paths tested", not "no setting can crash".

## RESOLVED — the cause was the synthesised guest call, and it was never needed

`fix_hotwired_healing` no longer crashes. The recorded combat demo now replays
end to end with the setting ON.

| build | result |
|---|---|
| synthesised guest call into `kResolveLiveEntity` | RUN INCOMPLETE, frozen, 1 dispatch miss |
| three interior entry points removed, clean re-lift | **identical** -- same pc, same lr, generation 40431 |
| the lookup done in C | **262 s complete, 57.9 fps, 0 dispatch misses** |

### What the crash actually was

Not a bad branch target. The log's `lr` was the tell:

```
[dispatch-miss] cpu=arm9 pc=0x02193E14 arm (lr=0x0207AA90)
  [ram@0x02193E14] 00000009 0213B68C 00000000 00000000
```

`0x0207AA90` is the instruction after `bl func_0207AAB8` at `0x0207AA8C`, and
`func_0207AAB8` is three instructions (`mov r1,#1 / strb r1,[r0,#0x10] /
bx lr`) with no indirect branch -- so that call returned normally. The very next
thing the containing function does is `pop {r4, pc}` at `0x0207AA94`, which
loaded `0x02193E14` -- a heap pointer, not code -- off the guest stack. **A
smashed return address**, in `Battles::Sim::SpellBase::virtual_03` (`0x0207AA00`),
the spell tick itself.

### The hypothesis that was REFUTED

docs/280's interior entry points sat on this exact path: `kResolveLiveEntity`
calls `func_02087350`, which they had split in two, leaving its `push {r4,lr}`
in one emitted function and its `pop {r4,pc}` in another. Removing all three and
re-lifting clean produced a **bit-for-bit identical crash**. So the truncated
containers were not the cause. (Removing them was still right -- the validation
records went from 16/20/44 bytes back to 64/72/144 -- but it is not this bug.)

### The fix

The override entered the guest to resolve the spell's source handle. Everything
that produced was ONE BYTE, the owner at `source+0xE4`, and the resolution
behind it is a pure function of memory:

```
func_0205C9B4   table = *(u32*)0x02155028 ; slot = u8 handle[0]
                bounds-check, index, compare u8 obj[0xDD] to u8 handle[1]
func_02087350   the above + a null-handle write-back on failure
func_020872DC   the above + reject a dying entity
```

No calls, no stack, no side effect the override depends on. Replicated in C as
`resolve_live_entity_c()`, so the guest is never entered: there is no return
address to smash, no call-return-stack entry to strand, and no re-entrancy
window. The write-back is deliberately NOT reproduced -- the guest performs it
itself at `0x0207AA40`, two instructions later, and duplicating it would mean
the port writing simulation state the cartridge already maintains.

### What this closes, and what it does not

**Closes:** this document's headline finding. Every enhancement setting,
including `fix_hotwired_healing`, now survives the recorded combat route.

**Does NOT close:** the general claim. The route is one battle. The honest
statement remains "no setting has been observed to crash on the paths tested",
and the paths tested are now much better than when this document was written --
they include combat, which is where the only known crasher lived.

**A lesson worth carrying:** a synthesised guest call is the port's most
dangerous construct, and this one bought a single byte. Before writing another,
check whether the value wanted is a pure function of memory -- if it is, read it
in C and never enter the guest.

## The fix is verified ON THE PATH, not merely by a run that finished

The objection that makes this necessary: clamping the camera at the world edge
(a good change in itself) desyncs the recorded demo from the play it captured,
so touch-screen inputs on units no longer land the same way. The AI opponent
still sends troops to raid the base, so combat happens regardless.

That matters. The demo is frame-indexed INPUT replay, not
state replay, so moving the camera bounds moves where a touch lands in the
world, and unit selection diverges from there. "It did not crash" is weak
evidence if the heal path never executed.

Checked rather than assumed, with `LB_TRACE_HEAL=1`:

```
[lb_heal] tick #0 source owner 1
[lb_heal] tick #64 source owner 1
...
[lb_heal] tick #1024 source owner 1
```

151 trace lines. That line is gated on `g_in_heal_tick`, which is set only when
the resolver RETURNS A LIVE ENTITY, so it proves three things at once: the spell
tick ran, `resolve_live_entity_c()` resolved successfully, and it returned a
sane owner. Run totals: 262 s, 58.1 fps, 0 dispatch misses.

**Two consequences for how these runs are described.** First, the A/B remains
valid: all three healing runs had the camera bounds active, so the desync is a
constant across them and only the resolver changed. Second, this demo no longer
reproduces the play it recorded and must stop being described that way -- it is
a deterministic combat route that reaches combat because the AI raids
regardless. Re-establishing a route that matches the original play needs a fresh
recording on the current build.

## A logging defect found in the same output

The trace lines ran together:

```
[lb_heal] SetHp reached: entity 02317944 hp<-1500 in_tick=0
[lb_heal] ...
```

Those are a LITERAL backslash and 'n', not a newline. Four format strings in
`lb_patches.cpp` contained `\n` where they meant `
` -- the heredoc
escape-mangling trap (C source is written with an editor, never through bash
heredocs, for this reason). A byte-exact search for `5C 5C 6E` across
`port/runner/src/*.cpp` found exactly those four and nothing else, which matches
the symptom: only the heal lines ran together. Fixed.

Worth noting how nearly it was missed: a regex search for the doubled escape
over-matched 72 lines in the same file, because a truncated display hid where
the match actually was. Had that been believed, the "fix" would have rewritten
68 correct format strings. The byte-sequence search is the one to trust.
