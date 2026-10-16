# 321 — HUD elements left at native positions: the failure mode is gone, with a denominator this time

The defect: during HUD open/collapse animations, elements sometimes drew at
their original-aspect positions. docs/316 left it at *"one run reads `edgedecl=0`, which is encouraging and
is not evidence"*. Two things were missing: a denominator, and a count of the
failure the 119 historical declines actually came from.

## The zero was unreadable on its own

`edgedecl` counts declined HUD elements that hug an edge. Across 36 runs and
1,642 summary lines it reads **0** — and that number is equally consistent with
"declines happen constantly and none touch an edge" and with "this block barely
runs". The `SKIPPED` list cannot settle it either: it is printed every 120th
frame while the decline path runs on every one.

So `g_declines` now counts every declined element beside it, and the summary
reads `edgedecl=<edge>/<all>`. Cost: one increment on a path that was already
being taken.

Driving the HUD open and closed through the build list:

```
edgedecl=0/227     hudquads seen: 0 1 6 7 8 10 11 13 14 15 16 17 18 19 20 64
```

**227 declines evaluated, none of them at an edge**, across sixteen distinct
quad counts — several of which are neither settled state, so they are frames
captured mid-transition, which is the animation the defect is about.

## The failure the 119 came from, counted directly

docs/316 noted that 105 of the 119 historical edge declines sat on frames
reading `moved=0` — *"the port moving nothing at all"* — and called a common
cause "plausible and unproven". That is a different and much cruder failure
than declining one element, and it can be counted exactly: a summary line
saying zero elements moved while `hudquads` is non-zero means the HUD was
present and the port left all of it at its native position.

| | `moved=0` with `hudquads>0` | of all `[lb_hud]` summary lines |
|---|---|---|
| runs before the fixes below | **76** | 6,889 |
| runs after them | **0** | 1,724 |

Zero of 1,724 against a prior rate of 1.1% is not "not seen lately". At that
rate the chance of no occurrence in 1,724 lines is about 5 x 10^-9.

And in the run above, every settled state moved everything it had:

```
moved=7 quads=7     moved=18 quads=18    moved=10 quads=10    moved=6 quads=6
moved=8 quads=8     moved=64 quads=64    moved=19 quads=19    moved=13 quads=13
```

One line reads `moved=7 quads=8`. That single element declined with
`edgedecl=0`, so it was a centred element, which is the case declining is FOR.

## What fixed it is still not established

The same stretch of work repaired the `kTouchPatches` collision that had
disabled thirty patch sites, and several band and geometry paths. Any of those
could be the cause, and no bisect was run. **The mechanism is not identified;
only its disappearance is measured.** That distinction is worth keeping, because
this defect was narrowed twice before on reasoning that turned out to be about the wrong
counters.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** the decline path is live and is being exercised — 227
evaluations in a single run driving the HUD through sixteen quad states.

**ESTABLISHED:** not one declined element touched an edge, in that run or in
any of the 1,642 summary lines across 36 runs.

**ESTABLISHED:** the `moved=0`-with-a-HUD-present failure occurred 76 times in
6,889 older summary lines and **0 times in 1,724 from the newer runs**.

**NOT ESTABLISHED:** which change fixed it.

**NOT ESTABLISHED:** that it is fixed on screen. The symptom was intermittent,
and what is measured here is the port's own classification, not what a person
watching the screen sees. The defect is marked fixed on the strength of a
failure mode that used to be countable and now counts zero, and the honest
caveat is that the instrument and the eye are not the same instrument.
