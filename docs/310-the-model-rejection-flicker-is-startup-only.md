# 310 — The model-rejection flicker happens only before the first good frame

Item 7 — the screen rendering flickers semi-frequently, causing a temporarily
transparent shroud, a yellow flash, or other graphical flickering — has one documented cause fixed (docs/267, a west-edge tile for the first 24 s)
and the rest open. This tests the other mechanism the port itself names.

`lb_widemap_tile` describes it:

> `choose_camera` demands 0 of 48 sampled cells disagree, with no tolerance.
> During a pan the guest's tilemap genuinely IS a mix of two cameras — the
> streamer rewrites columns as the camera crosses a tile — so no candidate
> matches all 48 and the model is rejected for that frame. Blanking then paints
> the whole band tile 0 for one frame: transparent on the fog layer (the
> terrain shows straight through) and a flat solid tile on terrain (the flash).

`g_model_bad_frames` had been counted for a long time and read 73–74 in every
run on disk — so rejections were real and regular. What was never recorded is
**how badly** a rejected frame disagrees, or **when** it happens. Both are one
increment on a path taken ~74 times in a whole run, so neither can cost
anything.

## How badly: not near-misses, total disagreements

```
badmax=48   bad-of-48 hist  1:0 2:0 3:0 4:0 5:0 6:0 7:0 8:0  9+:74
```

Every rejection disagrees on nine or more of the 48 sampled cells, and at least
one disagrees on **all 48**. The hypothesis that these are mid-pan near-misses
wanting a small tolerance is dead — and `choose_camera`'s own comment, which
argues against a tolerance because failures "disagree on hundreds of cells out
of 768", is confirmed rather than overturned. A tolerance would be wrong.

## When: never after the model has once worked

```
model_bad_frames=74 (after-first-ok 0)
```

**Zero** rejections occur after the model has been good even once. All 74 are
before the first good frame — the map not built yet — which is exactly the
harmless case.

This run was `--pan`, the very condition the comment above blames. It produced
no post-startup rejection at all, so on this build the premise that panning
rejects the model does not hold. The HOLD-the-last-good-table behaviour that
replaced blanking is still the right default; it simply is not being exercised
mid-play.

## The other flicker indicators in the same run

```
[lb_bares] lines            0
bare=0                      (cells drawn clear where the record says hidden)
mine vs guest BG1           0/768 differ
fallbacks=0                 (band-geometry fallbacks)
59.52 fps
```

Nothing disagreeing anywhere: the port's terrain model matches the cartridge's
own BG1 exactly on all 768 cells, no cell is drawn bare, and the band geometry
never takes its fallback.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** every model rejection disagrees on 9+ of 48 cells, max 48 —
genuine disagreements, not near-misses, so no tolerance is warranted.

**ESTABLISHED:** all 74 rejections precede the first good model. None occur
afterwards, including through a full `--pan`.

**Therefore the model-rejection path cannot be causing mid-play flicker**, and
that candidate cause for item 7 is eliminated.

**NOT ESTABLISHED:** that item 7 is fixed. One mechanism is eliminated and one
was already fixed (docs/267); the defect has several symptoms and this
addresses the band-blanking one only. A yellow flash or transparent shroud
arising from something else — a sprite layer, a palette upload, a frame the
3D engine misses — is untouched by this.

**One run, one map, one pan pattern.** `after-first-ok 0` is a strong signal
because the mechanism is deterministic — the map gets built and then the model
stays good — but it is not a proof over every map.
