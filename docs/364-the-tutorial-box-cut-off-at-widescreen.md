# 364 — The campaign tutorial's text box was cut off at widescreen shapes

Status: **fixed, verified, deployed** (`8f5a47a4`, `build/LegoBattles.exe`
sha256 `7787b871...`; Vanilla 240/240 identical). One cause, found
in the adaptive sizing and fixed there. A second fault, in the harness's
window placement, had been hiding it. With the whole box now on screen, a
third fault showed, in the HUD relocation: at 21:9 the box can come apart.
That one is docs/365.

## 1. What was wrong

The campaign tutorial (Knights, Act 1 mission 1) opens with a box across
the bottom of the field, "Touch a unit with the stylus to select it.", with
a red cancel button to its left. In Enhanced, at every window wider than
16:10, the box's second line and the button were cut in half by the
window's bottom edge.

| window | before | after |
|---|---|---|
| 800x600, 1024x768 (4:3) | whole (1024x768 lost ~4 rows at each end) | whole |
| 1280x720, 1600x900, 1920x1080 (16:9) | second line and button cut off | whole |
| 1720x720 (21:9, magnification 1) | ~61 of the 192 rows off (computed; see below) | all 192 rows on screen, surface 458x192 |

The 21:9 "before" is computed, not photographed. The A/B run with the old
floor stopped at the mission briefing, because the route does not always
reach the tutorial at 1720x720. At that shape the old floor was 5.5
against a cap of 3.75: a 312x192 surface showing 131 of its rows. A
3440x1440 window has the same shape and the same loss.

## 2. Why

The frontend chooses the pixel scale in three steps, each added to fix
an observed defect:

1. the magnification setting;
2. a **cap**, so the DS's own 256x192 always fits the window (its comment:
   "showing less than the DS is a regression however well it fills the
   window");
3. a **floor**, so the window never shows more than the world. This is
   the zoom-out limit, added for "bumping into black" at magnification 1
   in a big window.

The floor was added after the cap and applied after it, so when the two
disagreed the floor won. They disagree only when the world is smaller than
the window's view at the cap.

The tutorial opens on a world of 13x12 tiles, 312x192 px. That is tiles
x 8..20, y 43..54: the two tile positions at view+0x150/+0x158, read the
same way the cartridge's own `View_ClampCamera` (`0203906C`, via
`FUN_0201479c`) reads them. The camera sits pinned at its top-left
(192, 688).

A 16:9 window at the cap shows 341x192, more than 312, so at 1473x830 the
floor raised the scale from 4.32 to 4.72. The surface came out 312x192,
and the cover fit scaled it to 1473x906. The 38 px beyond the window at
the top and at the bottom are 8 DS rows each, and the box's second line is
in the bottom 8.

A full-size 64x64 map (1536x1024) cannot reach this at any window shape
between 1:4 and 8:1. That is why no skirmish or free-play test saw it.

## 3. The fix

The floor still limits zooming out, but it no longer raises the scale past
the cap (`port/runner/apply_hooks.py`, stanza 4y). With a world this
small, the view is now the world plus a margin in one axis: 340 px against
the world's 312 at 16:9.

The margin shows the map around the world, as 28 px of it already did.
The band is placed against the map, not the world rect, so the old 312 px
surface was not aligned to the world either. In the captures the margin is
the same rock and fog as the rest of the edge, with no black.

`LB_FLOORCAP=0` restores the old floor for an A/B.

## 4. A capture fault that hid it

At `LB_WINDOW=1473x830` the window sat at (444,315) and reached
(1945,1194) on a 1920x1080 test display. SDL centres the window at its default
size, and the `LB_WINDOW` resize kept that top-left.

Window captures do not see the off-screen part. So every route capture at
that size had lost its bottom 114 rows and right 25 columns, which is
exactly where this box sits. The black bands they showed looked like a
presentation fault, and were not.

The window is now re-centred after the resize. Only its position changes,
so emulation is untouched. Measurements graded from the port's own dumps
(layer maps, composites, fps.txt, the exactness gate) never went through a
window capture and are unaffected.

## 5. Verified

- **Exactness gate, identical.** The reference demo at 1473x830: all six
  instruction-count pairs match the reference (frame 7500: 1769454450 /
  104969363), and 8 of 8 layer maps and composites match gate-quiet.
  59.85 fps. A 64x64 map never reaches the changed branch, so this was
  the expected result.
- **The tutorial, before and after, same route.** Before is exp-tut365
  (the build before this change); after is exp-tut366. They are the
  end-of-run window shots at 800x600, 1024x768, 1280x720, 1600x900 and
  1920x1080 (the 2560 and 3440 shots overrun the 1920x1080 test display). The
  surface at 1473x830 went from 312 to 340 wide.
- **Vanilla untouched.** The scale code runs only in Enhanced. With
  `LB_WINDOW` set, the window position is the one Vanilla change.
- **The switch, A/B on the same build** (exp-tut372). The same route with
  `LB_FLOORCAP=0` brings back the 312 px surface, and the 1280x720 shot
  cuts "Touch this box to continue." off at the bottom again. The default
  shows it whole (exp-tut366).

## 6. The mission marker

The tutorial waits for the player: 150 s untouched left its first box up.
`tools/lb_exp.py` now takes `--game-tap X,Y[,S]` (taps made in gameplay,
after `--advance` and `--dismiss`). `--keep` now also holds a
`--stop-after` route open, and lets go of the debug socket so another
client can drive it.

A held-open session followed the tutorial through its first nine boxes:

1. select a unit;
2. deselect;
3. drag-select three;
4. move them to a yellow marker;
5. pick up a blue stud;
6. attack an enemy;
7. "Use the +Control Pad to center the view on the Castle".

The world grows as it goes: 312x192 px, then 360x224 (tiles x 8..22,
y 41..54), then the whole map. `020DAAE4` was not called once. The yellow
marker of step 4 is a world object, and it sat inside the DS's own
256x192, so no edge indicator was needed.
