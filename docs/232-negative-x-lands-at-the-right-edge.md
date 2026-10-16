# 232 — A negative x does not vanish; it lands at the right edge

**Goal within the project:** interaction in the widened field (adaptive screen
size must include clicking outside the DS's own columns). docs/231
established, with a controlled experiment, that the game refuses the *value*
rather than the delivery. This narrows what "refuses" means, and it turns out
not to be a refusal at all.

## The call chain, walked with the link register

docs/231 added LR to watch events. Walking it upward, one level per run, from
the write of the selection count:

```
020EC8F0  writes the count            lr -> 020E2796 / 020E296A
020E2408  the click handler           lr -> 020E3537
020E3518  a three-call wrapper        lr -> 020DD3C9
020DD3B4  a state-machine guard       lr -> 020AA8E1
020AA8D4  a delegate invoker          lr -> 020DBD01 / 020DBC43
020DBC14  the input router
```

Each of these was measured twice, once for an in-band click and once for a band
click, and the answer is the same all the way down until the last step:

| function | in band | band |
|---|---|---|
| `020DBC14` → `020AA8D4` (delegate invoker) | runs, 38 times | runs, 50 times |
| `020DD3B4` (state guard) | runs | **never** |
| `020E3518` | runs | **never** |
| `020E2408` (the click handler) | runs | **never** |

So the click handler is never even reached for a band click. The router runs;
the field-click delegate it would invoke does not.

`020DBC14` calls `FUN_020a8794`, whose field-selection branch is gated on
`FUN_020af040` returning zero — the gesture recogniser. And that routine stores
the touch coordinate as a **`char`** and compares it as an **unsigned byte**:

```c
*(char *)(param_1 + 6) = (char)*(undefined2 *)(param_3 + 0x1c);   // x
...
iVar2 = FUN_0211acdc((uint)*(byte *)(param_1 + 8) - (uint)*(byte *)(param_1 + 6));
if (iVar2 < 0xd) ...
```

That is the first place in the whole chain where the coordinate stops being
sixteen signed bits.

## What the coordinate actually becomes

Rather than argue from the decompilation, ask the game. `--bandwrap N` parks
the player's base at screen x = N and clicks at a chosen *other* x, using the
selection count at `0x0215802C` as the detector and reading back the selected
handle so "it selected something" and "it selected the target" are different
answers.

| base parked at | clicked at | selected | count | handle |
|---|---|---|---|---|
| 60..190 (its own position) | at the base | 2 of 4 | 1 | 000D |
| −112..−16 (off the panel) | at the base | **0 of 4** | — | — |
| 234..246 | −16 | 2 of 4 | 1 | 000D |
| 234..246 | −116 | 4 of 4 | 1 | 000D |
| 210..222 | −40 | 2 of 4 | 1 | 000D |
| 210..222 | −80 | 3–4 of 4 | 1 | 000D |
| **122..134** | **−128** | **0 of 4** | — | — |
| **122..134** | **−228** | **0 of 4** | — | — |

Read the last two rows against the middle four. A negative click selects the
base when the base is **near the right edge** of the panel, at 216 or 240, and
selects nothing when the base is in the **middle** at 128 — whatever negative
value is clicked.

**A negative x is not discarded. It is delivered to the pick as a position near
the right edge of the native panel.** Which is why nothing about it ever looked
like a clean bounds test: the click really does happen, it just happens
somewhere else, and the only reason it usually appears to do nothing is that
the right edge of the screen usually has nothing on it.

That also explains, retrospectively, the "cutoff exactly at screen x 0" of
docs/231: at x ≥ 0 the click lands where it was aimed, and at x < 0 it lands at
the far right, so the target stops being hit the instant x goes negative.

## Two hypotheses tested and refuted, recorded so they are not retried

**A drag rectangle.** The unsigned-byte delta above is a drag threshold, so a
negative x gives a huge delta and would be classified as a drag rather than a
tap — and a drag sweeps a rectangle, which would explain a click far from the
base still selecting it. **Refuted by the count**: every selection in the table
is exactly **one** unit. A drag rectangle wide enough to reach the base from
100 px away would have caught the builder and the hero too, and the count is
capped at eight, not one.

**A plain 8-bit wrap.** `(char)` truncation predicts native −16 → 240 and −116
→ 140. The first hits a base at 240 and the second should miss it by a hundred
pixels. **It does not miss**: −116 selected the base at 240 four times out of
four. So the truncation alone is not the whole story.

## The discriminating experiment, run

Two mechanisms fitted every row above: the coordinate is **clamped** to the
panel's right edge, or it **wraps** and the base's clickable footprint is much
wider than assumed. Both predict the same thing for every click the default
window allows, because its band is only 128 px and every reachable negative x
wraps into the right half of the panel — which is also where a clamp puts it.

They come apart at a wider band, so `--bandsize` sets the window first. At
1920×1080 the band is 345 px:

```
band 345 px each side
in band     target at x  60..190  -> SELECTED 2 of 4     handle 000D
left band   target at x -329..-16 -> SELECTED 0 of 4
wrap 60<--196   target at x 54..66 -> SELECTED 0 of 4
ctrl 60<--296   target at x 54..66 -> SELECTED 0 of 4
```

A click at native **−196** wraps to exactly **60**, which is where the base is
parked, and the control at the same moment shows the base is selectable there.
It selects **nothing**.

**The wrap is refuted. The coordinate is clamped**, to somewhere near the
panel's right edge — consistent with a negative signed value being read as
unsigned 16 bits (−16 → 65520) and then saturated to the panel width, which is
the same class of bug as the five `LDRH` → `LDRSH` sites already in
`kTouchPatches` and one more of them.

## What is left

The clamp's exact instruction. A value-filtered watch for writes of 255 across
main RAM is too noisy to use — 255 is written about 1,200 times per trial by
unrelated code, and the ring is dominated by it.

Two better approaches for the next run, both cheap:

* ~~Bracket the landing point.~~ **Done, and it brackets cleanly.** Parking the
  base at a series of positions and clicking the *same* native −150 at each:

  ```
  land @140  0 of 3       land @200  0 of 3
  land @160  0 of 3       land @220  2 of 3
  land @180  0 of 3       land @240  1 of 3
  ```

  Hits begin between 200 and 220 and continue to 240. Writing L for the landing
  point and w for the base's clickable half-width, that says `L − 220 ≤ w` and
  `L − 200 > w`. **L = 255 fits with w between 35 and 55 px**, which is the
  right size for a three-metatile building (72 px wide, half 36). So the click
  lands at or within a few pixels of **255 — the last column of the DS panel**,
  whatever negative value was clicked.

  That is the signature of an unsigned read plus a saturate-to-width, not of a
  truncation and not of a bounds rejection.
* **Watch the event object.** `FUN_020af040` reads the touch x from
  `event + 0x1c`, and the event is heap-allocated, so the address is not known
  statically — but the allocation is visible: `020ECADC` was seen writing a heap
  pointer during selection, and the same LR trick that walked the call chain
  gives the allocation site.


## Why this matters for the fix

Both mechanisms are fixable in the same place and neither needs the guest's
picking logic to be understood further: the coordinate has to survive as
sixteen signed bits into `FUN_020af040`'s state, exactly as the port already
widened five other reads from `LDRH` to `LDRSH` (the `kTouchPatches` table).
What this document adds is that those five were not enough, and where the sixth
one is.
