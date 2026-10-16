# 317 — Item 8: drag-time camera drift is not boundary-specific

Item 8 was *"clicking/dragging past the aspect causes auto-pan"*. docs/277
retracted half of it by measuring a **hold** at nine points and finding the
camera still, and docs/298 recorded the rest plainly:

> Drags do move the camera and that half is unexplained.

`--edgescroll` tests holds, so it cannot speak to drags. The `--drag-boundary`
probe already performs paired drags that differ *only* in whether the box
crosses the native edge, so recording the camera either side of each one turns
it into the missing test at no cost.

## Result

```
rep0 cross  box -22..58 CROSSES x=0   cam still
rep0 native box   0..80 one side      cam still
rep2 cross  box -22..58 CROSSES x=0   cam -48,+0
rep3 native box   0..80 one side      cam -48,+0
rep3 cross  box -22..58 CROSSES x=0   cam +48,+0
rep6 native box   1..81 one side      cam +48,+0

CAMERA DRIFT (item 8)  crossing 2/5 drags moved the camera,
                       entirely-native 2/8
```

**Drift happens on both arms at comparable rates.** A drag whose box lies
entirely inside the native columns moves the camera just as a crossing one
does, so whatever causes it is not the native edge and not the widened view.

## The quantum is exactly two metatiles

Every drift is `±48` px and never anything else. A metatile is 24 px wide
(docs/286), so the camera moves in **two-metatile steps** — which is a
game-side scroll granularity, not an arbitrary nudge. The camera values seen
elsewhere in these runs (0, 88, 136, 217, 273) also differ by multiples that
include 48.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** drag-time camera movement is not specific to dragging past the
aspect. It occurs at a similar rate for drags entirely within the native
columns, in a paired test where the two arms differ only in that.

**ESTABLISHED:** when it moves, it moves by exactly ±48 px, two metatiles.

**NOT ESTABLISHED:** whether the drift is correct behaviour at all. The game
may scroll on its own — following a unit, or edge-scrolling — and this probe
cannot tell a game-side scroll from a port-side one. It only shows the band is
not implicated.

**NOT ESTABLISHED, and the honest limit:** n is 5 and 8. That is enough to say
the two arms are not obviously different; it is not enough to bound a small
difference.

## What would settle the rest

A drag with no unit near it, on a still camera, repeated — if the camera still
steps by 48 px with nothing to follow, the scroll is unconditional and belongs
to the game; if it only steps when a unit is selected or walking, it is the
camera following, which is also the game. Either answer removes item 8's
surviving half from the port's defect list.

## It is not the camera following the unit

The remaining alternative was that the camera follows a selected or walking
unit, which would make the drift game behaviour rather than anything to do with
drags. Pairing every camera sample with the hero's **world** position in the
same run:

```
paired samples: 110
camera moved between samples: 3
hero   moved between samples: 2

  both moved        : 0
  CAMERA ONLY moved : 3
  hero only moved   : 2
  neither           : 104
```

**Zero samples where both moved.** The camera moved three times with the hero
stationary, and the hero moved twice without the camera following. So the
camera is not tracking the unit; it scrolls on its own.

## The Vanilla control could not be run

The plan was to settle this the way docs/311 settled item 10 — run the same
probe with `enhanced: false` and see whether a native drag scrolls the camera
there too. **It did not run.** Vanilla emits no `[lb_geom]` line, and the
drag delivery needs it to map window coordinates to native ones, so the probe
stopped at `drag-unit: geom=None` and performed no drags at all.

The run's camera steps are therefore route movement, not drag response, and
comparing them to the Enhanced run's would be comparing a run that dragged
against one that did not:

```
Enhanced  418 samples, 70 changes:  -48,+0 x33   +48,+0 x30   (+56, +136, -32 once each)
Vanilla   158 samples, 25 changes:  -600,-144 x9  +600,+144 x9  +88,+144 x3
```

Recorded rather than quietly dropped, because the previous section of this page
states the comparison as the thing that would settle it.

## Where item 8 stands

**ESTABLISHED:** the drift is not boundary-specific (both arms, similar rates),
not camera-follow (zero co-moving samples), and quantised to exactly two
metatiles.

**NOT ESTABLISHED:** that it is the cartridge's own behaviour. Everything
measured points that way and nothing points at the port, but the one comparison
that would prove it has not been run.

**What it needs:** a native-only drag control that does not depend on
`[lb_geom]`. A drag entirely inside 0..255 can be delivered by raw touch, which
works without the geometry line — so a Vanilla arm is buildable, just not with
today's `click6` delivery.

## The control, run at last: Vanilla drags scroll too

`--drag-native` exists to be the comparison. It confines the drag to the native
256x192 and delivers it with **raw `touch`**, which takes guest coordinates and
needs no `[lb_geom]` — so the identical drag can be made in either mode, which
`click6` delivery could not.

It also carries its own control: one path hugging the left edge, where an
edge-scroll would fire, and one across the middle, where it should not.

```
Enhanced   edge 2/6 moved the camera   middle 2/6
Vanilla    edge 3/6 moved the camera   middle 0/6
```

**Vanilla scrolls too.** A drag entirely inside the native window, on a build
with no widening at all, moves the camera in three of six trials. Whatever
does this is in the cartridge, not in this port's extended view.

One honest caveat on the shape of it: Enhanced moved on middle drags and
Vanilla did not, which would be interesting if six trials per cell could
support it. They cannot. The magnitudes also differ (Enhanced ±48; Vanilla
±128,±144 and one −456), which is more likely the two runs sitting at different
camera positions than a difference in kind.

## Item 8: off the port's defect list

The original defect: clicking or dragging past the aspect boundary causes
auto-pan. Taken apart:

* **clicking** — docs/277 measured a hold at nine points scrolling nothing.
* **past the aspect** — the crossing and non-crossing arms drift at the same
  rate, so the aspect boundary is not involved.
* **auto-pan itself** — real, quantised to two metatiles, not the camera
  following a unit, and **present at native width in Vanilla**.

Nothing in it requires the widened view, and the one comparison that could have
implicated the port instead cleared it.

**What is still not known** is what the cartridge scrolls *for* — a drag
gesture, a touch near an edge, or something else entirely. That is a question
about the game rather than about this port.
