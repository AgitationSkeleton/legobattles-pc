# 313 — The selected SET is now readable, not just the count

The suspected defect that drag-selection may not select the right units has
been OPEN and unverified throughout. It is a claim about a **set**, and until now
nothing in this project could read one: every selection probe uses the COUNT at
`0x0215802C`, which answers "did anything get selected" and is silent on
"were they the right ones".

## Closing the address docs/226 left open

docs/226 found the shape from the per-frame consumer — `param_1 + 0x54` is the
selection list and `param_1 + 0x58` its count — and ended:

> That is the next address to watch, and it now needs only one thing that is
> not yet in hand: the address of `param_1`.

The count's known address closes it. `0x0215802C = param_1 + 0x58` puts
`param_1` at `0x02157FD4` and the list pointer at `0x02158028` — which is also
what `--bandselect`'s `sel_handle()` reads, so the two agree independently.

`[lb_sel]` prints the set whenever it changes:

```
[lb_sel] none
[lb_sel] count=2 handles: 000F 000E
[lb_sel] none
```

## Two mistakes on the way, both of which produced a confident nothing

**The entries are two-byte handles, not pointers.** Read as words the list
yields garbage, and the first version printed nothing usable.

**Sampling once a second misses the event.** The first version sat inside the
entity tick, which returns early on 59 frames of every 60 and again if the
camera or field rect is unavailable. It reported `[lb_sel] none` for an entire
run in which drags plainly selected — a clean, wrong answer. The read now runs
**every frame**, before those early returns.

Neither mistake announced itself: both produced a quiet, plausible "nothing
selected". That is the failure mode worth remembering here.

## Cost

One `bus_read_u32` per frame plus a compare, and printing only when the set
changes — a held selection is silent. The whole block is behind `LB_ENTLOG`.

## What it already shows

A drag aimed at the hero selected **two** entities, `000F` and `000E`. Whether
the second belongs in the box is exactly the wrong-unit question, and it is now
askable.

## Next

Map handle to identity. The entity pool at `0x02155028` gives an array and a
count, and the handles (14, 15 here) are plausibly indices into it — but that
is a guess and this file has just spent two mistakes on guesses. `lb_field`
already exposes `lb_field_entity_ptr(i, &ptr)`, so the check is cheap: print
the handle beside the pool index whose entity matches, and see whether they
agree across several selections.

With that, the wrong-unit question becomes a set comparison: entities inside the drag box
versus entities actually selected.

## First real reading, and a miscount of my own

Eighteen drags in one run, with the set logged on every change:

```
11 x  count=1 handles: 000F
 6 x  count=2 handles: 000F 000E
36 [lb_sel] lines total -- 17 selections and 18 deselections
```

**The same probe, the same target, two different outcomes.** A box nominally
around the hero picks up `000F` alone eleven times and `000F` plus `000E` six
times. That is the shape of the suspected defect, stated in entity handles rather than in
impressions, and it is the first time this project has been able to say it.

It is **not yet a defect**: the box moves between trials because the target
drifts (`sx` 18, 32, 41 within one run), so `000E` may be legitimately inside
the box on some trials and outside on others. Deciding that needs the handles
mapped to positions — the next step above.

**A miscount worth recording.** Reading the log through `head -14` I saw one
`count=` line among the first few and concluded "only one drag in the whole run
actually selected anything", which would have made the hudquads detector look
badly broken and invalidated docs/312's boundary result. The full file has 36
lines. The two detectors in fact agree closely — 18 drags, 14 called SELECTED
by hudquads, 17 real selections — and **docs/312 stands**. Truncating a log and
generalising from the visible part is the same error as sampling once a second,
one tool further up.

## A separate finding: these runs end outside gameplay

`--drag-boundary` runs leave `NOT-GAMEPLAY.png` behind and exit 1; the
pause-cycle runs do not. The last trials show `hud 1 -> 64`, and 64 quads is a
menu, not a selection. So something late in the drag sequence opens a menu and
the run finishes there.

It did not corrupt the boundary measurement — the `[lb_sel]` counts confirm
real selections throughout — but it means the *tail* of a drag run is not in
gameplay, and any end-of-run counter read from one of those runs should be
treated accordingly.

## Handles resolved, and the wrong-unit question answered

The handle is a **raw index into the entity pool** at `0x02155028` (`+8` array,
`+0xC` count). That is not assumed — it is confirmed by pointer identity
against an independent feed:

```
[lb_sel] count=2 handles: 000F 000E
         [000F ptr=023076A4 cat=0 map=235,239]
         [000E ptr=02307460 cat=1 map=211,207]

[lb_hero]    sx=18 sy=95 owner=0 ptr=023076A4     <- same pointer as 000F
[lb_builder] sx=-6 sy=63 owner=0 ptr=02307460     <- same pointer as 000E
```

`000F` is the player's hero (cat 0), `000E` the builder (cat 1). The camera
arithmetic agrees both ways: hero map 235 minus `sx` 18 gives `cam_x` 217, and
builder map 211 minus 217 gives `sx` -6, which is what the entity feed
independently reports.

### The test

The builder sits at surface `(-6, 63)` and every drag box is `y 55..135`, so
the builder is inside a box exactly when the box's x range contains -6.
Predicting the selection count from that alone, and comparing with what the
game did:

```
rep  arm     box x        builder in  predict  observed
0    cross   -22..58      YES         2        2   ok
0    native  0..80        no          1        1   ok
1    native  0..80        no          1        1   ok
1    cross   -22..58      YES         2        2   ok
...
8    native  1..81        no          1        1   ok

prediction from box geometry alone: 14 correct, 0 wrong, of 14
```

**Fourteen of fourteen.** The game selects exactly the units the box contains.

### No wrong-unit selection

The "two different outcomes" recorded in the previous section were correct
behaviour throughout: the box really did move between trials as the target
drifted, and the selected set tracked it precisely. Nothing here supports the
suspicion that drag-selection may not select the right units.

### And it strengthens selection in the band

The crossing boxes are the ones that reach the builder, which stands at
surface `x = -6` — **in the band**. They selected it every time. So a drag box
that extends past the native edge does pick up a unit standing out there, which
is a stronger statement than docs/312's "no difference in whether something got
selected".

**Scope:** one map, two units, one box height, n=14. The prediction is exact
rather than statistical, which is what makes fourteen trials worth stating, but
it is not every case — in particular no trial had the target itself in the band
with the box entirely native.
