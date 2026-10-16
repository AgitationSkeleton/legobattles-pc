# 374 — Widened 3D x lands on the DS's pixel, plus the band

Status: **x fixed; y left as it was.** The defect: sprites nudged at the
world border.

## Two causes, one already gone

The analysis rests on exact integer models of the port's own arithmetic
(vertex rounding, the border nudge and the bias fix).

* **What the cartridge does.** It places sprites exactly. Screen positions
  are whole pixels from one camera, and the ortho maps them exactly
  (X = 32768·x − 2^22, w = 2^22). It has no sub-pixel term and no border
  term.
* **Cause 1, the rounding.** The port's widened transform rounds three times:
  * `SubmitVertex` scales X by 256/RenderWidth (a C truncating divide);
  * it adds 2·w·shift/RenderWidth (another);
  * the viewport floors `(posX·RW)/den`.

  At RW 736, **67.6% of whole-pixel columns land 1 px left** of `x + band`;
  at 842, 960 and 1536 it is 55-69%, and 512 and 1024 are exact. The pattern
  depends on the column and on the band separately.
* **Cause 2, the band moving under a still camera.** While the band slid at a
  map edge and the camera moved with it, the ground stood still but the
  rounding pattern changed every frame. So a still sprite flipped by a pixel
  against a still ground on 18.4% of camera steps at 736. That is the
  observed nudge.

**docs/373 removed cause 2**: the camera now stops where the band would start
to slide, so it no longer moves through a sliding band. This doc removes
cause 1.

## The fix

A 1/256 px bias before the viewport's floor:

    s32 lb_px = (s32)((posX * viewport_width + lb_round) / den) ...
    lb_round = enhanced_full_viewport ? den / 256 : 0

The model checked every even RW from 258 to 1728 and every band shift. That is
1.32M whole-pixel columns, and 1.32M half-pixel columns (odd-width sprites,
where the DS floors x.5 down). 0 are wrong. The native 256-wide path does not
take it, and `LB_VTXROUND=0` turns it off (apply_hooks stanza 0zzzzz).

**Y is not changed.** There a bias leaves 27% of rows off (by the same integer model);
it needs the exact form: the viewport arithmetic on the unscaled clip Y, plus
the integer band. The y error is mostly constant, not flipping, so it does not
nudge.

## Verification

* **Counts:** exp-gate374 is identical to gate370b at all six checkpoints.
  This is a host-renderer change.
* **Vanilla** (exp-van374): 240/240 identical.
* **Frame rate:** 59.58 fps.
* **Layer maps** against gate-quiet (the old rounding), classified by
  `shiftcheck.py`:

| frame | differing px | what they are |
|---|---|---|
| 1285 | 0 | |
| 2570 | 72 | the right-edge column (24 rows) of three moved HUD elements: the bricks, Deselect and a list icon. Their left edge had rounded 1 px left and their right edge had not, so they drew one pixel **wider** than the DS draws them; now native width. |
| 3855 | 0 | |
| 5140 | 143 | all explained by 3D content moving exactly 1 px right: onto `x + band` |

Every difference is the intended correction, in the direction the model
predicts.
