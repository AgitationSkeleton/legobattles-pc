# 316 — The HUD decline that matters, counted rather than sampled

The defect, as `lb_hud.cpp` records it: the open/collapse HUD animations for
the brick (red/blue/yellow) actions sometimes still display at their original
aspect positions.

docs/298 narrowed it to "not in `lb_hud.cpp`", on the grounds that every
classification exit there is clear cumulatively. That narrowing was reasoning
about the wrong counters.

> **2026-09-26, docs/385: found, and not by this counter.** It IS in
> `lb_hud.cpp`, and no decline counter could see it. A list's items slide
> along their row while it opens or closes, and each item in flight is an
> element of its own, placed by its own position: `+0,-111` mid-screen (so
> drawn mid-field) or `+251,-111` near the right edge. Those move vertically,
> so none is a decline, edge or span. LB_HUDTRACE showed it frame by frame;
> the pieces now take the brick column's move.

## The evidence was already on disk

`LB_HUDLOG` prints a `SKIPPED` list: elements classified as HUD whose computed
displacement came out zero, so they keep their native position. Most of those
are correct — a centred icon touches no edge and must not be dragged to one.
The ones that matter are those **hugging an edge**, which should have moved.

Surveying every run on disk, 1,060 SKIPPED entries:

```
centred (native x 25..231)              941   declining these is correct
TOUCHING AN EDGE (x<=24 or x1>=232)     119
   native x 227..251 y 168..192   x69
   native x   4..12  y  16..40    x7
   native x  12..19  y  16..40    x7
   native x   4..12  y  40..64    x7
   native x  12..19  y  40..64    x7
```

Those four rects are **exactly the buttons in the defect** — the build-flow
probe reads the same in-field HUD as `(4,16,12,40) (12,16,20,40) (4,40,12,64)
(12,40,20,64)`, the brick actions in the top-left corner. The symptom is in the
logs, and has been all along.

105 of the 119 come from frames reading `surface=448 moved=0` — the port
moving nothing at all — and every one is in a run predating this work.

## Why the SKIPPED list could not settle it

It is rebuilt every frame and printed on every **120th**. The file says so
about its sibling counter:

> a decline lasting a few frames is overwritten before it is ever reported ...
> 124 samples across ~15,000 frames cannot see a three-frame event

So "none in the current runs" from that list is worth little. Across 32 runs
and 1,533 sampled HUD frames there were indeed zero edge-touching skips — which
is encouraging and is not evidence, for exactly the reason quoted.

## The counter

`g_edge_declines`, incremented whenever a declined element has `nx0 <= 24` or
`nx1 >= 232`, cumulative and never reset, with the first eight printed in full.
Same construction and same justification as `g_span_declines` beside it. It
appears in the summary as `edgedecl=`.

Cost: an int compare on a path that already runs once per declined group.

First run with it, driving the HUD open and closed through the build list:

```
spandecl=0 edgedecl=0
```

Zero — and this time over **every frame**, not every hundred and twentieth.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** the symptom is real and is recorded in this port's own
logs — 119 edge-hugging elements declined, whose rects are the brick action
buttons and the bottom-right corner element.

**ESTABLISHED:** all of those occurrences are in runs predating this work.

**ESTABLISHED:** one current run, with the HUD opened and collapsed, produces
`edgedecl=0` counted on every frame rather than sampled.

**NOT ESTABLISHED:** that it is fixed. One run is one run, and `lb_hud_scan`
only runs while a HUD is present. What has changed is that the next occurrence
cannot hide: a single one anywhere in a run now survives to the summary
line and prints its rect.

**NOT ESTABLISHED:** which change fixed it, if it is fixed. The 105-strong
cluster sits at `moved=0`, the port moving nothing at all, which is a different
failure from declining one element — and the same work repaired the
`kTouchPatches` collision that had disabled thirty patch sites, so a common
cause is plausible and unproven.

## What the first run actually covered

Worth stating, because "one run" is vague and the run was cut short by a
`taskkill` clearing a deploy lock — which cost its exit code, not its data.

It selected the builder and drove the HUD through several states before that:

```
after tapping the builder, hudquads=6
6 HUD quad(s): (4,16,12,40) (12,16,20,40) (4,40,12,64) (12,40,20,64) (4,168,28,192)
tapped (8,28)   hudquads 6 -> 13   OPENED
tapped (16,28)  hudquads 13 -> 7
tapped (8,52)   hudquads 7 -> 18   OPENED
```

and the distinct `hudquads` values logged across the run were
**0, 6, 7, 13, 16, 17, 18**.

The 16 and 17 matter: they are neither of the settled states, so they are
frames captured **mid-transition** — the open/collapse animation the defect names.
The elements listed are the brick action buttons themselves.

So the `edgedecl=0` covers several HUD transitions including intermediate
frames, on the exact elements in question. Still one run, but a more pointed
one than the bare number suggests.
