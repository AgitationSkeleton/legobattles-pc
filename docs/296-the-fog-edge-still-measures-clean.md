# 296 — The fog edge still measures clean, against a control in the same frame

A screenshot from play showed broken shroud edges at the fog/terrain frontier.

docs/270 fixed a flat-edged shroud and verified it with a comparison worth
keeping: the cartridge's own 256 columns sit inside the same composited surface
as the bands, drawn by the same renderer in the same frame, so the guest region
is a **known-good sample of what this game's fog edge looks like**. Band figures
are read against it, never against an absolute.

That measurement was made by hand in docs/270 and by hand again afterwards.
Twice is enough: it is now `tools/lb_fogedge.py`.

## The metric

`LB_LAYERDUMP` records which layer won each pixel, as `(order + 1) * 40` — BG1
terrain is 120, BG2 fog is 160. For each row the tool finds the boundary column
between them and reports:

* **mean run** — consecutive rows sharing one boundary column. An organic
  diagonal coastline steps often and scores LOW; a flat horizontal edge holds
  one column for many rows and scores HIGH. docs/270's defect read **28.81**
  against the cartridge's **2.04**.
* **steps** — the histogram of column change between adjacent rows. The
  cartridge's signature is 1–2 px steps dominating.

A region that is entirely fog or entirely terrain has no frontier, and the tool
says "no boundary here" rather than scoring a mean from too few rows. That
distinction is what stops an empty region looking like a clean one.

## The result

786x376 surface, guest columns at x 265..521:

```
CARTRIDGE (guest)  rows= 32  mean run 1.33  longest 4  steps {0:8,  1:12,  2:8,  4:2}
LEFT band          rows=287  mean run 1.59  longest 4  steps {0:106,1:123, 2:45, 4:5}
RIGHT band         rows=303  mean run 1.18  longest 4  steps {0:47, 1:162, 2:47, 3:34}
```

On a second run's frames, the same shape: cartridge 1.54, left band 1.25.

**All three regions are in one range, 1.18 to 1.59, with the same longest run of
4**, and all three are dominated by 1–2 px steps. The bands' fog frontier has
the cartridge's own signature. docs/270's fix is holding, and nothing here
resembles the 28.81 the defect produced.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** on these captures, the fog/terrain boundary in both bands is as
organic as the guest's own, measured against that guest region in the same
frame.

**NOT ESTABLISHED:** that this is what the screenshot shows. It is one
moment in a play session on a Mars-tileset map; these dumps are a Castle map
on the harness route. A defect confined to a tileset, a map edge, or a state
this route never enters would not appear here.

What this does do is narrow it: "the band's fog edge is flat where the guest's
is organic" — the docs/270 defect, and the most obvious reading of "broken
shroud edges" — is measured absent. If it is seen again, the useful thing
is which part of the window, because `lb_fogedge.py` can then be pointed at that
region specifically rather than at the whole frontier.

## Addendum — the Mars tileset, and why the metric cannot depend on one

docs/296 closed with "a defect confined to a tileset would not appear here",
since the dumps were a Castle map and the screenshot is Mars. That was
tested, on `--map-index 1` (mp02, MarsTileset), verified loaded by sampling the
field centre of a capture: 3859 red pixels against 385 green.

```
              CARTRIDGE   LEFT band   RIGHT band
Castle map      1.33        1.59        1.18
Mars map        1.41        1.60        1.18
```

Nearly identical, and the step histograms are too — left band `{0:105, 1:122,
2:42, ...}` against `{0:106, 1:123, 2:45, ...}`.

That similarity is not a coincidence or a failed map switch. **The metric reads
layer identity, not colour.** `LB_LAYERDUMP` records *which layer won* each
pixel; the fog/terrain boundary is therefore decided entirely by the visibility
record — which cells are revealed — and a tileset only changes what gets painted
inside each region. Two runs whose camera path and revealed area have the same
shape produce the same boundary whatever the art.

So the caveat this addendum set out to close was partly misplaced: a
tileset-specific fault cannot show up in this measurement **and cannot hide in
it either**. If the reported edge problem is a tileset artefact, it is a colour
or tile-selection fault inside a region, not a boundary fault — which is a
different measurement, and `[lb_shroud]`'s `bare` counter (docs/288) is the one
that already watches for it, reading 0 so far.

What remains genuinely untested is a state this route never enters: a map edge,
a particular reveal pattern, or a moment in a long session.
