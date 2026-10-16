
---

## Fog EDGE variants are now learned (2026-09-03)

The last open row of the widescreen fog work. The six tiles this document
identified are the INTERIOR pattern -- what the cartridge draws where a shrouded
metatile is surrounded by shrouded metatiles. At a boundary it draws a
transition variant, and the widened columns were drawing the interior pattern
there.

Nothing is invented. `learn_fog_edges` watches the guest's OWN 32 columns: for
each shrouded metatile whose eight neighbours are all inside the window, it
records which tile the cartridge actually drew at each of the six positions,
keyed by the neighbour mask. The widened columns then look the same key up, and
a mask never observed falls back to the interior pattern -- which is what was
drawn before, so this can improve the picture and cannot make it worse.

Working, after two bugs that both hid the same symptom:

    [lb_fogedge] cand=120 notshroud=57 margin=23 notfog=0 took=40 masks=6

120 candidate metatiles a frame, 57 skipped as revealed, 23 too near the window
edge for their neighbours to be observations rather than the unseen default, 40
learned, **6 distinct masks**.

### Why it read zero twice first

1. **A legitimately zero address used as a sentinel.** The guard was
   `if (!g_bg2_base || g_seen_w <= 0) return;` and BG2's screen base in this game
   is genuinely **0** -- this document records the fog layer at base 0 -> 0x0000.
   So it bailed on every frame. `g_seen_w` is the honest liveness test.
2. **The counters that would have said so were never printed.** They were added
   when it first read zero, and the print did not survive the edit, so a second
   run reported the same uninformative `edgemasks=0`.

Six masks is not many against 256 possible, but the common ones are what a
boundary uses, and the table keeps filling as the player explores.
