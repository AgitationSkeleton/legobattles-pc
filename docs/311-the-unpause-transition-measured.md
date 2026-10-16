# 311 — The unpause transition, measured: a ~300 ms guest fade, not a band fault

Item 10, fog and shroud misbehaving briefly when unpausing, was recorded in
docs/298 as **OPEN — not investigated**. It is investigated here.

## The instrument

`lb_exp.py --pause-cycle`: read the port's counters, press START, linger,
press START again, shoot a burst across the first second, and read the counters
beside every shot. The counters are cumulative, so the delta across the cycle
is what the pause caused and no baseline run is needed.

The detector already existed. docs/310 measured `model_bad_frames`'
*after-first-ok* term at **0 across a whole `--pan` run**, so any non-zero
value here is the pause and nothing else. That is what makes this readable at
all.

## The timeline, twice

```
                model_bad  after-ok  terrain_differ  shrouded
before              73         0           0           768
paused             152        79         745           767
+100ms             272       199         745           767
+200ms             272       199         745           767
+350ms             307       234           0           768     <- recovered
+600ms             307       234           0           768
+1000ms            307       234           0           768
```

A second run agrees within one or two counts (74 / 80 / 236), so this is
reproducible rather than a one-off.

**While the menu is up, `terrain_differ` is 745 of 768.** That is expected and
not a fault: the port's terrain model describes the *field*, and BG1 is holding
the pause menu. The model check correctly rejects, and the band holds its last
good table rather than painting nonsense — the paused capture shows the menu
spanning the full widened width with no stale terrain beside it.

**Recovery completes between +200 ms and +350 ms.** `terrain_differ` returns to
0, `shrouded` to 768, and the rejections stop.

## What is on screen in that window, and the control that explains it

At +100 ms and +200 ms the window is **fully black** — and at +350 ms the field
is back and pixel-consistent with the pre-pause frame.

The obvious reading is that the port's band logic is failing for ~300 ms. The
in-frame control says otherwise: **the top screen is equally black.** The port
renders the top screen 1:1 and never extends it — docs/308 measured that at
exactly `scale x 1.0000, scale y 1.0000`, pixel-identical to the guest's own
output. A layer the port does not touch cannot be blacked out by the port's
band logic.

So the blackness is the **cartridge's own fade** through the unpause
transition, and the port is reproducing it. The counters agree: the model is
rejected during the fade precisely because there is no field being drawn to
model.

## A first reading that was wrong, and how the control caught it

The first `--pause-cycle` run took its "after" capture three seconds post-
unpause. Before and after came out essentially identical — same terrain, same
green patch — which says nothing at all about the moment in between, and I had
already started writing up `terrain_differ = 744/768` as the defect.

It is not a defect, for the reason above, and three seconds was the wrong
place to look for something described as brief. The burst exists
because of that mistake.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** pausing drives the port's terrain model to disagree on 745 of
768 cells, which is correct behaviour — BG1 holds the menu, not the field.

**ESTABLISHED:** recovery completes between +200 ms and +350 ms after unpause,
reproducibly, with `terrain_differ` back to 0 and `shrouded` back to 768.

**ESTABLISHED:** the black interval is the guest's own fade, because the top
screen — which the port renders 1:1 and does not extend — is black at the same
moment.

**ESTABLISHED:** nothing persists. The settled frame is consistent with the
pre-pause frame, and `bare` stays 0 and `band_fallbacks` 0 throughout.

**NOT ESTABLISHED:** that the fade's *duration* matches the cartridge's. ~300 ms
is plausible for a DS fade but it has not been compared against native-width
output, and a fade that runs long would look exactly like this.

**NOT REPRODUCED:** any fog or shroud artefact at unpause. Two runs, and the
only visible event is a fade that a layer the port does not touch shares.

## Next

The one open question is cheap: run the same cycle in Vanilla and compare the
frame at +100 ms and +200 ms. If Vanilla is black at the same offsets, the
transition is the cartridge's entirely and item 10 is not reproducible from
this route; if Vanilla has already recovered, the port is holding the fade
longer and that is a real defect with a known window to hunt in.

## The control, run: Vanilla fades identically

docs/311 left one question — whether the ~300 ms black interval is the
cartridge's fade or the port holding it longer. Run the same cycle with
`enhanced: false` and measure mean luminance of each burst frame:

```
shot        enhanced   vanilla
before        139.6      32.3
paused         18.3      21.2
+100ms          2.2      16.2
+200ms          2.2      16.2
+350ms        139.9      32.3     <- both fully recovered
+600ms        139.3      32.3
+1000ms       140.4      32.3
settled       139.3      32.3
```

**Both modes darken across the unpause and both return to their own baseline
at exactly +350 ms.** The port's widening neither lengthens the fade nor leaves
anything behind.

The absolute numbers differ only because Vanilla is pillarboxed — most of its
frame is permanent black border, so its baseline is 32.3 against Enhanced's
139.6. The shape and the recovery frame are what compare, and they match.

(Vanilla also emits none of the counters: `[lb_wideR]` and the terrain audit
belong to the band code, which does not run. The probe printing `{}` there is
correct, though its "may not have reached gameplay" wording is misleading for
this case — the run held 59.72 fps and its captures are good.)

## Item 10: closed as not reproducible from this route

Every visible event at unpause is the cartridge's own, and the port reproduces
it identically at native width. No fog or shroud artefact appears at any offset
from +100 ms to settled, in either mode, across three runs.

What would reopen it: an artefact that can be timed or captured, or one that
needs a longer session than this route runs — the pause here happens about
twenty seconds into a match, with 768 of 768 cells still shrouded, so a defect
that needs explored terrain either side of the pause is outside what this
measured.
