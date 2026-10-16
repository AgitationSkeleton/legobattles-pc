# 290 — RETRACTED: the shroud-seam measurement was invalid twice over

> **RETRACTION.** This document reported a hypothesis as
> **confirmed**. It was not validly measured, and the confirmation is
> **withdrawn**. Two independent faults, either fatal on its own, are set out at
> the end. The original reasoning is kept above them because it read as careful
> and was not.

Two screenshots from play showed broken shroud edges, and mis-tiled shroud that
appeared to sit in the original-aspect part in the middle of the field.

The second is a located hypothesis, and it is **confirmed**.

## The measurement

The shroud is drawn from 8x8 tiles, and the field is magnified 2x, so one tile
is 16 window pixels. A correctly tiled field is therefore self-similar at a
16-pixel horizontal shift: comparing each column against the column 16 to its
right should give a small difference everywhere.

Where that self-similarity **breaks**, the tiling phase has been interrupted.

On a heavily shrouded frame at 1600x800 (guest 786x376, band 261 each side),
comparing 48-pixel-tall column signatures against their 16-pixel neighbours and
flagging differences above 4x the frame mean:

```
period-break clusters (window x):   134..143    526..541    558..567
```

## The control that makes it mean something

A break could equally be a terrain edge — the frame is 91% cloud in the sampled
row, not 100%. The discriminator is the one docs/212 used to classify the HUD:
**move the camera and see what does not move.**

Four frames across a pan:

```
pan-00   134..143, 526..541, 558..567
pan-01   132..143, 526..541, 558..573
pan-02   134..143, 526..541, 558..567
pan-03   134..143, 526..541, 558..559
```

**Identical window x in every frame while the camera moves.** A world feature
would travel; these do not move at all. They are screen-locked, which makes them
artifacts of the compositing geometry rather than anything in the map.

## And one of them is where the screenshot put it

With the field starting at window x 14 and 2x magnification, surface x is
`(window - 14) / 2`:

| cluster | surface x | what is there |
|---|---|---|
| 134..143 | 60..64 | — |
| **526..541** | **256..263** | the guest's own 256 columns begin at band left = **261** |
| 558..567 | 272..276 | 11..15 px inside the native region |

The middle cluster brackets the native boundary. That is the original-aspect
part in the middle, identified from a screenshot, and the measurement agrees.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** three horizontal positions where the shroud's 16-pixel tiling
period breaks; they are fixed in window coordinates across four frames of camera
motion; one of them brackets the boundary between the port's synthesised band
and the guest's own columns.

**NOT ESTABLISHED:** what produces the other two (surface 60 and 272), and
whether all three share one cause. 272 sitting ~11 px inside the native region
is suggestive of a second boundary rather than the same one measured twice, but
that is a guess and is recorded as one.

**NOT ESTABLISHED:** that this is the same artifact as the "broken shroud edges"
in the first screenshot. That image shows the fog/terrain frontier with stepped
edges, which is the subject of docs/270 — a different measurement with its own
fix already landed. The two reports arrived together and may be one thing or
two.

**NOT MEASURED:** the right-hand boundary. The guest's columns end at window
x 1048 and no cluster was found there, which is itself worth explaining: a seam
on one side only is a stronger clue than a seam on both.

## Why the earlier fog work did not catch this

docs/288 walked the LAYER map and found no isolated islands, and the always-on
`bare` counter reads 0. Both are correct and neither can see this: a seam is a
phase discontinuity **within** BG2's own content, where every pixel is a
legitimate fog tile drawn by the layer that should draw it. Nothing is missing
and nothing is the wrong layer — the tiles simply do not line up across a
column. Only a self-similarity measure looks at that.

---

# THE RETRACTION

## Fault 1 — the self-similarity was tested at the wrong period

The measurement compared each column against the column **16 window pixels**
away, on the reasoning that the shroud is built from 8x8 tiles at 2x
magnification, so one tile is 16 window pixels.

`LB_TILECHK` shows what the tile stream actually is, logged per tile across the
band boundary:

```
ht=-4 tile=000   ht=-3 tile=1E0   ht=-2 tile=012
ht=-1 tile=000   ht= 0 tile=1E0   ht= 1 tile=012
```

**The repeating unit is three tiles — `000, 1E0, 012` — not one.** That is the
metatile, 24 guest pixels, 48 window pixels. A correctly drawn field with a
3-tile pattern is *not* self-similar at one tile, so the "period breaks" this
document reported are places where an 8-pixel comparison was worse than average
on a pattern that never had an 8-pixel period. They are an artefact of the
question, not of the picture.

## Fault 2 — the camera-pan control did not pan

The control was: "move the camera and see what does not move", across
`pan-00..03`. Measured afterwards by correlating the frames:

```
pan-00 -> pan-01 : shift +0 px      pan-02 -> pan-03 : shift +0 px
pan-01 -> pan-02 : shift +0 px      pan-00 -> pan-03 : shift +0 px
```

**The camera never moved.** Every pair is a zero shift, including the widest.
So "the clusters sit at identical window x in every frame while the camera
moves" reduces to "four near-identical frames measure the same", which
establishes nothing at all. The control that was supposed to separate a
compositing artefact from a terrain feature did neither.

## And the mechanism it pointed at is affirmatively fine

The handover this document located at surface 261 is `lb_ht` crossing 0 in
`decode_text_line`, where the port's substituted band tiles give way to the
guest's own. The tile stream above runs straight through it —
`000, 1E0, 012, 000, 1E0, 012` — with `sub=1` on the port's side and `sub=0` on
the guest's, and no repeat, skip or phase change at the join.

The sub-tile phase is shared too: `tx = sx & 7` with
`sx = (bgx + hw_x) & sx_mask`, computed once and used for both the guest's tile
and any substituted one. Masking at 256 or 512 cannot alter the low three bits,
so the phase is correct out in the band as well.

**So the band/native handover is measurably clean**, which is the opposite of
what this document reported.

## What is left of the report

The observation stands on its own: the screenshots show something wrong in the
shroud. What is withdrawn is this document's claim to have
**located** it at the native boundary. That location came from a measurement
with no valid control, and a separate measurement now shows the boundary itself
behaving correctly.

The report is open again, and unlocated.

## The rule, for the fourth time in a row

docs/250 (a stale probe), docs/288 (a periodic counter), docs/289 (an fps band)
and docs/292-293 (a comparison against a different renderer) all failed the same
way, and so does this: **a reading was trusted before its instrument was.**

This one is the worst of the five, because the instrument had a control built
into it and the control was never itself checked. Correlating the four frames
took one command and was run only after the conclusion had been published as a
confirmation of the hypothesis.

## Addendum — Fault 2 re-verified, this time with a validated instrument

The retraction above was itself checked, because the correlation that produced
Fault 2 had a suspicious signature: an identical mean residual (27.1) on all
four pairs, including the non-adjacent one. Identical numbers across different
inputs usually means the measurement, not the data.

Two things were done before trusting it a second time.

**The frames were confirmed to differ at all.** Distinct md5 sums, distinct file
sizes, and a direct pixel comparison showing 33-40% of sampled pixels differing
between most pairs. So the images are genuinely different and "shift 0" was not
a comparison of one image with itself.

**The shift finder was validated on known shifts first** — the step that
had been skipped five times. A frame was offset by a known amount and the finder
asked to recover it:

```
known  -64 -> recovered  -64   OK
known  -16 -> recovered  -16   OK
known   +0 -> recovered   +0   OK
known  +24 -> recovered  +24   OK
known  +96 -> recovered  +96   OK
```

Five of five. Only then was it run on the real frames:

```
pan-00 -> pan-01 : shift +0 px  (residual 27.1)
pan-01 -> pan-02 : shift +0 px  (residual 27.1)
pan-02 -> pan-03 : shift +0 px  (residual 27.6)
pan-00 -> pan-03 : shift +0 px  (residual 27.6)
```

**Fault 2 stands.** There is no horizontal translation between those captures.
The 33-40% of differing pixels are fog being revealed and units moving — content
changing in place — not the camera sliding. The similar residuals are simply the
size of that content difference once the frames are correctly aligned, which is
what a residual at the right shift should look like.

Worth recording separately: the run's log shows the camera *did* move,
`cam=136 -> 265 -> 266 -> 268 -> 272 -> 584`. It moved between the captures'
moments rather than across them, so the pan happened and the captures missed it.
That is a sampling fault in the harness, not a still camera, and it is what a
future crossing test has to fix: capture on a verified camera delta, not on a
step counter.
