# 226 — The gather rect was never widened

**Goal within the project:** adaptive screen size in gameplay that fully works,
including unit rendering, effects and particles, and touch input across the
widened view. This is one bug that was quietly costing both.

## The gate stopped being the gate

`apply_cull_width` widens the rectangle the cartridge gathers entities into.
Without it the game never puts an entity outside its native 256×192 into its
list at all, so nothing out there can be drawn and nothing out there can be
hit-tested.

Its first line was:

```cpp
if (!std::getenv("LB_WIDEMAP")) return;    // same switch as the widening
```

The comment was true when it was written. It stopped being true when
`LB_WIDEMAP` became **default-on**: `lb_widemap.cpp`'s `stage()` returns 1 when
the variable is unset, so the widening runs while the variable is absent — and
this function, reading the variable directly, took its early return.

So in every ordinary session, and in every probe run before this fix,
the game ran with its stock gather rect while the port drew a widened picture
around it.

This is the same shape of fault as the one recorded in this file's own history:
a guard that declines in silence is indistinguishable from a guard that fired.
Nothing printed, because the printing is inside the part that was skipped.

**The gate now matches what actually decides the widening** — `lb_setting_enhanced()`,
with `LB_WIDEMAP=0` still able to force it off.

## Two more faults in the same function

**The margins described a surface the port no longer uses.** They were `352` and
five metatile columns, both derived from a fixed 448-wide surface with 96 px
either side of the guest's 256. The surface has been adaptive since docs/215 — a
1600×900 window gives 392×212, which is 68 px a side, and a different window
gives something else again. The margins are now computed from the live surface:

```
[lb_cull] gather rect right edge camx+256 -> camx+384 px (band 128)
[lb_cull] gather rect left edge camx-24 -> camx-168 px (1 -> 7 metatile columns)
[lb_cull] gather rect right edge camx+256 -> camx+324 px (band 68)
[lb_cull] gather rect left edge camx-24 -> camx-96 px (1 -> 4 metatile columns)
```

That is one run resizing its window: the rect follows it and then goes quiet.

**It patched once.** The overlay carrying this code is reloaded between battles,
which puts the stock encoding back, after which a one-shot patch is silently
absent for the rest of the session. It now compares the code against the
encoding it wants every frame — two reads — and rewrites when they differ, so it
is self-healing and prints only when it actually changes something.

## The same stale constant, one file over

`[lb_ent]`'s `kExt = 96` decided which entities counted as "in a widened
column". On a 68-px band that counts entities which are off the drawn surface
entirely — the smaller version of the mistake its own comment describes. It now
comes from the live surface too.

This one matters more than a log line: **the harness reads that report to decide
where to aim**, and a trial aimed at native −80 on a 68-px band is aimed at
nothing. docs/225's right-band trials failed to park for a related reason.

## Why this was so hard to see

Buildings still appeared in the widened columns, because the building stamp is a
different path from the entity gather. So the band was never obviously empty —
the player's base sat out there looking correct while units did not, and a
capture of a base in the band reads as "units render in the band" unless you
know to tell them apart.

And every measurement of the touch path was correct and irrelevant: the
coordinate reaches the touch struct as −38 (docs/224), every instruction that
reads it sign-extends (docs/225), and the rectangle builder is clean. All true,
and none of it could matter while the unit being aimed at was not in the list
the hit test walks.

## Status of the fix

The patch is confirmed to be **applying** where it previously did nothing, and
to track the window. Whether it makes an out-of-band click select is measured
separately, by the three-band comparison in `--click-watch`, and is recorded
with that measurement rather than assumed here — this port has published three
conclusions that were reasonable and wrong, and "the obvious cause has been
fixed so the symptom must be gone" is exactly the shape of the fourth.

---

## Measured: it does NOT fix out-of-band clicking

Recorded here because the section above deliberately refused to claim it. The
`LB_CULLWIDE=0` switch exists so this patch can be A/B'd against itself with the
same surface, the same calibrated aim, and ten attempts a trial:

| | in band | left band | clamp control |
|---|---|---|---|
| gather rect **on** | 4 / 10 | **0 / 10** | 0 / 10, clean |
| gather rect **off** | 7 / 10 | **0 / 10** | 1 / 10 |

Two conclusions, and the second is the one that matters.

**The fix does not break anything.** In-band selection works either way; 4 of 10
against 7 of 10 is well inside the noise of a click that lands about half the
time.

**The fix does not make an out-of-band click select.** Zero of ten on both
sides, against an in-band rate of 4 to 7 in 10. At the worse of those rates a
run of ten failures happens 0.6% of the time by chance, so this is a real
negative rather than a quiet one.

So the gather rect was genuinely broken and is genuinely worth fixing — units
and effects outside the native columns were never being gathered, which is a
defect in its own right — but it is not what stops a click out there from
selecting. The hunt continues, with a better instrument than it had: a
calibrated aim, a clamp control, and enough attempts for a zero to mean
something.

**What this rules out.** Not "the entity is missing from the list" — it is in
the list now, and the click still does nothing. Combined with docs/224 and
docs/225, which showed the coordinate arriving intact and every reader
sign-extending, the remaining candidates are all downstream of both: something
that tests the point itself against the view.

## Also ruled out: the game's own view width

`LB_VIEW_WIDTH=392` widens the cartridge's own view through its own
`Set_View_Dimensions` — 32 tiles to 49, with the setter reallocating the
view-space fog mask to match. If anything in the hit test were bounded by the
view's width, or by a structure sized from it, this would lift that bound.

```
[lb_fov] view width 32 -> 49 tiles (392 px); the setter reallocates the fog mask to 168 bytes
click-watch: inband   builder at (123, 63)  SELECTED 3 of 10
click-watch: left     builder at (-42, 63)  SELECTED 0 of 10
click-watch: left     clamp point (0,63)   SELECTED 0 of 10   (clean)
```

Still zero. So the view width is not the bound either.

**The list of things measured and excluded**, each by an experiment rather than
by reading:

1. the coordinate never arriving — it reaches the touch struct as −38 (docs/224)
2. a reader zero-extending it — every reader enumerated from a read watch, and
   the only `LDRH` feeds a `STRH` that truncates to the same bits (docs/225)
3. the selection rectangle being wrong — `func_020AA8E4` is `LDRSH` throughout
4. the entity being absent from the gathered list — the `LB_CULLWIDE` A/B above
5. the game's own view width bounding it — this section

Whatever decides is downstream of all five, and the next step stops guessing at
it: `--sel-trace` points a watch at the entity struct itself and records what an
in-band click WRITES there, which names the routine that marks a unit selected.
The same watch on an out-of-band click then says whether that routine ran.

Two small pieces of plumbing were needed for that, and both are worth having
anyway. `lb_field_entity_ptr` exposes an entity's address, which nothing static
names — it is reached through a pool pointer and an array, so it exists only
once a battle is running. And a `watch_range` debug-server command sets the
memory watch while the game runs, because `LB_WATCH` is parsed once at startup
and cannot express "the entity the harness just found"; without it the only way
to watch a runtime address was to run the whole route twice.

## A fifth caller of the gather, in the overlay

`LBXrefs.java` on the gather and its rect:

```
FUN_020882bc  (gather entities within a metatile rect)   5 references
   from 02001a84  in FUN_02001938
   from 020391a4  in FUN_02039148
   from 0203d41c  in FUN_0203d3d8      <- entity draw pass A
   from 0203d4cc  in FUN_0203d488      <- entity draw pass B
   from 020e3400  (no function)        <- ???

FUN_02038f28  (the rect itself)                          4 references
   from 02039188  in FUN_02039148
   from 0203d400  in FUN_0203d3d8
   from 0203d4b0  in FUN_0203d488
   from 020e33ee  (no function)
```

The interesting one is the last of each. Ghidra reports "no function" because
that address is in an overlay it never analysed, and the overlay matters: the
callers of `FUN_020a8794` -- the touch reader at the head of this whole
investigation -- are also out there, at `020dbc14` and `020dfdd8`. Earlier watch
traces recorded live PCs at `020E9C64`, `020E0E6A`, `020E1284` and `020ABB18`,
so this code is resident and executing rather than dead weight.

That fifth caller asks `FUN_02038f28` for a rect and hands it to the same
gather, sixteen bytes apart, which is the same shape as both draw passes. It is
a good candidate for the selection's own gather.

It also means the widening this document fixed should already reach it, since
the patch is on the immediate INSIDE `FUN_02038f28` and every caller shares it.
That is consistent with the A/B finding no change, and it moves the suspicion
past "which rect" to what the overlay does with the list once it has it.

## Where the trail stands, and the next address to watch

The entity trace worked once the watch was drained close enough to the click.
Two runs of the same click, one that selected and one that did not, differ by 21
(offset, pc) pairs — and four of them are in the `0x020Dxxxx` overlay:

```
ONLY when it SELECTED
   +1EC  pc 020D3B12        +160  pc 020D3E82
   +1A0  pc 020D48E6        +160  pc 020D48EA
```

`020D3B12` sits in `FUN_020d3acc`, which reads `+0x1EC` as a list LENGTH — it
builds the selected unit's action list. A consequence of selection, not the
decision.

Its only caller is at `020D4906`, which Ghidra had no function for. Scanning
back for a Thumb `push` finds the entry at `020D48CC`, and the other two
overlay PCs fall inside it. Forced through the decompiler (`LBForce.java`, new,
which asserts a boundary Ghidra would not guess), it opens:

```c
if ((*(int *)(param_1 + 0x58) == 1) &&
    (iVar2 = FUN_020872dc(*(int *)(param_1 + 0x54)), iVar2 != 0)) {
    ...
    FUN_020d3acc(param_1, iVar2);
}
```

**So `param_1 + 0x54` is the selection list and `param_1 + 0x58` is the count**,
and this function is the per-frame consumer of both. Whatever WRITES those two
fields is the decision this whole investigation has been looking for.

That is the next address to watch, and it now needs only one thing that is not
yet in hand: the address of `param_1`. `watch_range` can point the watch at it
the moment it is known, exactly as it was pointed at the entity here.

**Not being claimed:** that this is the last link. It is the next one.
