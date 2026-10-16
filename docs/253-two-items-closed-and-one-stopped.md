# 253 — The HUD off-screen guard is a sweep artifact; the fog edges are stopped

## The HUD is not dropped at wide surfaces

Recorded earlier as still open: "at the widest surface tried (1706x696) all five
are rejected by the offscreen guard -- 0 corner element(s) moved ...
offscreen=5". **That was overstated.**

Every `[lb_hud]` line at the run's own stable surface reports five of five
moved:

    5 corner element(s) surface=786x426 visible=786x426   (x9, consecutive)
    0 corner element(s) surface=1706x696 ... offscreen=5   (once)

1706x696 occurs **only in the end-of-run size sweep**, which changes the window
every second. The guard tests `surface - extra` against the DS's own 0..256,
and `extra` is the band the 3D frame was transformed with, handed to
`lb_hud_scan` deliberately so the anchor undoes exactly the shift the geometry
received. When the surface changes **between** the render and the composite,
those two are from different sizes and every quad mis-locates by the
difference -- at that moment `shift=725` against `band=393`, a gap of 332,
which is more than enough to push a native 0..256 quad outside the test.

So it is the sweep's own transient, not a fault in play. On a 1920x1080 test
display the surface is about 960x540, so 1706 is not reachable at a stable
window size there at all.

Which also means the anchoring fix in `dba1389b` -- inset elements classified
by the outer third rather than requiring edge contact -- is the whole of that
defect, and it holds at every stable size tested.

## The remaining square fog edges: stopping, with the reason

1440 cells per report still take the interior pattern. Four routes have been
tried and closed **by measurement**, not by argument:

1. **Observation.** Every learnable mask has `E1 S1`. The explored region
   starts at the map's own west edge, so every boundary has revealed ground to
   its west or north. Ordering units east grew the record (`0..22` to `0..27`)
   and produced no new mask, as predicted.
2. **A second observation condition.** With the camera inside explored ground
   the learner sees no cloud at all: `cand=120 notshroud=110 took=0`.
3. **Tileset mirroring.** An east edge is not the mirror of a west edge. Best
   match across all twenty tiles differs on 22 of 64 pixels; the rest 38..52.
   The cloud shapes are drawn independently per orientation.
4. **The static call graph.** `func_0203A4B4` is a memcmp dirty-check against
   the cached fog buffer, and `func_0203D90C` is a four-instruction address
   helper (`rowtab[y] + x*2`) that **nothing branches to** in the lifted ARM9 --
   so the caller chain that solved the fog record is unavailable here.
5. **The write watch.** Watching the BG2 tilemap names only DMA and clear
   helpers (`01FF8260`, `021104C4`, `02110714`) writing 0 and 0x1300, because
   the tilemap is DMA'd from a RAM staging buffer. Watching that buffer's known
   address returns **zero events**: it moves between runs, and `LB_WATCH` is
   read once at startup so it cannot follow a runtime pointer.

Fixing this needs a port-side watch that resolves the row table at runtime --
real work, for the last third of one cosmetic defect, while sprite truncation
at the top of the view is still unexplained. Stopping here is the right call
rather than a sixth approach.

**What stands:** the cardinal fallback fixed 2880 of 4320 square cells, 67%,
measured by A/B on one route with one variable.
