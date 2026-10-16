# 377 — Sprite depth follows the feet across the bands

Status: **fixed.** Defect item 7 (2026-09-25): sprite overlap differs from
vanilla at some resolutions (builders drawn on top of trees/crystals they are
behind).

The ROM analysis is a prior static pass ("tier 1"). Every site was
re-disassembled here, and every stock
word was re-checked against `out/eur/arm9/section_static_02000000.bin`
(`sprites_checkenc.py`: 10 of 10 OK).

## The cause

A world billboard's depth is its screen row.

* **Where z comes from.** `Unit_BuildBillboard` (`0203ABC8`) stores
  `z = 11 + feet row`. The shared quad emitter (`0204C210`) turns it into
  MTX_TRANS z. The ortho is `G3_OrthoW(0,192,0,256,-1024,1024)` (`020FA694`),
  so lower on the screen is nearer: depth `0x7FFE00 - z*0x2000`.
* **The byte.** The sum is kept in a byte and clamped unsigned to a table
  value of 243:

      0203B28C  ldrb  r0,[r0,#1]    ; max = 243
      0203B290  add   r1,r2,r1      ; 11 + feet row
      0203B294  and   r1,r1,#0xff   ; 8-bit wrap
      0203B298  cmp   r1,r0
      0203B29C  movhs r1,r0         ; unsigned clamp

* **The same shape elsewhere.** Projectiles (`020EBF34`) compute it the same
  way, and a flag forces the maximum. Collectables (`020EBB70`) clamp signed
  first, then wrap.

On the DS the feet row runs -16..240, so the byte never shows. The widened
picture gathers rows far outside that, and two seams appear:

* **The top seam.** From feet row -12 upward the sum wraps to 255.., which
  clamps to 243. A unit behind something is drawn over it.
* **The bottom seam.** Rows 233..244 all tie at 243, and from 245 the sum
  wraps to 0.., so the order reverses the other way.

The trees in question are the ones a spell grows (Forest Spawn / Jungle Growth,
docs/375's stalk). Those are 3D billboards, unlike the map's BG1 trees, which
no sprite can be under.

## The fix

`apply_depth_unwrap` (lb_patches.cpp) writes ten words, all of them or none:

| addr | stock | ours |
|---|---|---|
| 0203B28C | `ldrb r0,[r0,#1]` | `mov r0,#0x1BC` (444) |
| 0203B294 | `and r1,r1,#0xff` | `mov r0,r0` |
| 0203B29C | `movhs r1,r0` | `movgt r1,r0` (signed) |
| 020EC25C | `and r1,r0,#0xff` | `mov r1,r0` |
| 020EC264 | `ldrbne r1,[r0]` | `movne r1,#0x1BC` |
| 020EC26C | `ldrb r0,[r0]` | `mov r0,#0x1BC` |
| 020EC274 | `movhs r1,r0` | `movgt r1,r0` |
| 020EBDF0 | `ldrb r6,[r0]` | `mov r6,#0x1BC` |
| 020EBE18 | `and r1,r6,#0xff` | `mov r1,r6` |
| 0204C290 | `ldrh r0,[r8,#0x56]` | `ldrsh r0,[r8,#0x56]` |

The result is `z = 11 + feet row` across the whole picture:

* **The DS's own rows are unchanged.** Rows -11..232 keep their DS values.
* **The top band goes negative.** It stays far inside the ortho's -1024.
* **The HUD is unaffected.** Everything stays behind the health bars (448)
  and the HUD (503 and up), so `lb_hud`'s threshold is untouched.
* **Models agree.** Siege and transport models already use `s16(11 + y)`
  (`0203B554`), so billboards and models now agree across the bands.

**Draw order only.** `entity+0x66` is written only at `0203B2A4`, and
`sprite+0x56` is read only by the emitter. Every other emitter caller stores
a z under `0x8000`, so `ldrsh` reads it unchanged.

**When.**

* Only with Enhanced on, and only with a vertical band (a surface taller
  than 192). Without one the feet rows are the DS's own, and the stock words
  give the cartridge's exact order.
* Never while an older recording replays. Demos gain `depth=1`, because the
  four patched functions run in Tier 3 while the words are ours.
* `LB_DEPTHWRAP=0` restores the stock words.

## Verification

* **Depth values (`LB_POLYLIST`).** A held Free Play battle at 1473x830
  (surface 736x414, band 111 rows), panned so billboards cross into the top
  band (one-off scripts `polyab.py`, `polycheck.py`). Every billboard-class
  polygon's z = (0x7FFE00 - depth)/0x2000 was set against its guest feet
  row (34 dumps):

  | feet rows | with the fix | stock (`LB_DEPTHWRAP=0`) |
  |---|---|---|
  | the DS's own (-11..232) | 78 of 78 are 11 + feet | 78 of 78 are 11 + feet |
  | top band (< -11) | 9 of 9 are 11 + feet (e.g. feet -32: z -21) | 0 of 11 (feet -96: z 171, -80: 187, -48: 219) |

  * **Stock reads are the byte formula exactly:** (11 - 96) & 0xFF = 171.
  * **Visibility:** a sprite with negative z still draws (the hero standing
    in the top band, `exp-d377on2/win/top.png`), because the rear plane
    clears deeper than that.
  * **The bottom band** had no billboard in these dumps. Its fix is the same
    two instructions, the missing wrap and the signed clamp.
* **Cost (Tier 3).** The debug server's `static_coverage` over 30 s of the
  same scene: 49,946 interpreted ARM9 instructions a second with the fix,
  against 38,326 without. That is about 11.6k a second more, a small fraction
  of a millisecond of host time a second. The first, idle scene read 44,192
  against 31,385.
* **Gate** (exp-gate377):
  * Counts are identical to gate370b, and the eight pictures under 5972 are
    identical to gate375.
  * The gate demo has no `depth=` key, so the stock words ran throughout.
  * `[lb_depth]` is logged only after `[lb_demo] finished at frame 8188`.
  * The replay ran at 59.80 fps.
* **Vanilla** (exp-van377): 240/240 identical.
