# 397 — Clicks at every window size, and the menu in DS Vanilla

Status: **built, gated, deployed with the next release.**

Two player reports and one defect found while reproducing them:

- **Missed clicks at varying window sizes.** The host lobby's back arrow (docs/396, "Not reproduced") failed on other screens too. Another player could not pick character slots and rows in the Free Play army builder.
- **The settings menu in DS Vanilla.** A player chose DS Vanilla, found that clicks did not land where they clicked, and could not switch back to Enhanced. They had to delete their settings file.
- **Enhanced with adaptive width off cropped the screen.** The cartridge's 256x192 picture was scaled to the window's width, so its top and bottom rows fell outside the window.

## Missed clicks: the battle's HUD boxes outlived the battle

**How the correction works.** Every screen's buttons are tested by one rectangle test, `FUN_020EA1A4`. The port observes it (`widget_rect_observer`) because the battle HUD's anchored widgets are drawn away from the rectangle the game tests (docs/383):

- The observer asks `lb_hud_hit_unmap` whether the press is on a face the HUD moved.
- If so, it maps the press back to the widget's own rectangle.
- If the widget's element "moved away", it reports a miss, so a press on the old spot does not fire a widget drawn elsewhere.

**The defect.** The face boxes (`lb_hud` scan, `g_boxes`) are rebuilt only when the battle composite runs. Leave a battle and the last battle's boxes stay live. Any later screen whose button rectangle overlapped one of those boxes then had its press corrected against geometry from a screen no longer on show.

**Measured** (run `t396menuab`, the old rule forced with `LB_HUDHITOWN=0`, 1900x795 window, pixel scale 1):

```
[lb_hudhit] v2 rect 10..34 x 160..184, press 22,172: its element moved away: miss -> 22,172
            [scan band 740,225 surface 1536x642 scale 1.00; shown band 640,225]
[lb_hudhit] widget 22 rect 10..34 x 160..184: point 22,172 -> moved away, MISS 22,172 (lr 020E5625)
```

The main menu's back arrow (rect 10..34 x 160..184) took a battle face's verdict:

- the scan band of 740 is the battle's;
- the menu on show had a band of 640.

The press was refused. Which buttons fail depends on where the last battle's faces were, so it depends on the window size and zoom. That is why it came and went with size and showed on different screens for different players.

**Change.** The correction applies only to widgets of the battle view on show:

- The battle screen is the registry slot `0x02157FAC` whose vtable is `0x0214F0EC`.
- Its layout is at `screen+0x1C`: a count byte at `+2` and widget pointers from `+0xC`.
- `widget_in_battle_view` checks that the widget being tested (`R1`) is one of them. Any other screen's buttons take the game's own test, unaltered.
- `LB_HUDHITOWN=0` restores the old rule, for an A/B.

**Verified.** The same sequence with the new rule: the back arrow answers after a battle.

## DS Vanilla: the menu was drawn at twice its size

**Cause.** DS Vanilla presents through `SDL_RenderSetLogicalSize`. That scales everything drawn afterwards, the menu included, and it hands mouse events over in logical coordinates:

- ImGui laid itself out for the whole window and was then drawn at the logical scale (2x on a 1920x1080 window). The player's screenshot shows the panel running off the bottom right, with the Enhanced radio button at the edge.
- ImGui read the cursor at half its real position. It thought the cursor was over its panel when the player was clicking the game, and swallowed those clicks.

**Change** (`lb_overlay.cpp`):

- The overlay is drawn with the logical size cleared, and the size is restored afterwards.
- ImGui's copy of each mouse event is converted back to window pixels (`SDL_RenderLogicalToWindow`). The game's copy is untouched.
- The "Top Screen" panel is hidden in DS Vanilla, which already shows the top screen.

**Verified** (run `t397van`, 1473x830):

- The menu fills the window at its normal size.
- Clicking DS Vanilla switches the presentation, and clicking Enhanced switches it back.
- A click on the results screen's Stats brick in DS Vanilla opens the stats.

## Enhanced with adaptive width off: fit, not cover

The enhanced presentation sizes the guest surface from the window and then covers the window with it. Cover only absorbs rounding, because the surface was sized to fit. With adaptive width off (`--adaptive-widescreen none`), though, the surface stays at 256x192. Covering a 16:9 window then took the width's scale:

- 5.75x on a 1473x830 window;
- 1104 rows shown in an 830-row window;
- a quarter of the cartridge's own picture cut off, the results screen's buttons included.

**Change** (the frontend stanza beside the cover rule): when covering would show fewer than the guest's own 256 columns or 192 rows (beyond 2 pixels of rounding), the field fits instead. With adaptive width on, the surface already follows the window and the rule never fires.

**Verified** (run `t397van2`):

- `field 256x192 at 4.323x`, the whole picture with bars at the sides.
- A click on a minifigure selects it, and the HUD bricks come up where the cartridge draws them.

## Found, not yet fixed

**The quit dialog and pause menu at full zoom-out.** At 1536x642, pixel scale 1:

- the pause menu's right side is cut off, with a blue streak;
- the "This will terminate the current game" dialog's No button lacks its brick, and a stray brick sits at the right edge.

This shows in the guest's own framebuffer. At the default size the same dialog is correct. It points at the widened BG/OBJ handling at large band offsets, which is next.

## Gate

The final build passed:

- rep397a == rep397b, and equal to rep396ma: the simulation is unchanged;
- gate397 counts equal gate370b;
- 8/8 pictures equal gate381, by pixels;
- 59.89 fps;
- van397 240/240 against van-hudmask.

The guest pictures the gate compares are taken before presentation, so the fit change cannot show in them. It was checked in the window instead (above).
