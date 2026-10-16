# 291 — The band audit cannot run, which is why its numbers are stale

Chasing the shroud seam of docs/290 to its source led to a standing measurement
recorded in `gpu2d.cpp`'s own comments:

> tools/lb_bandaudit.py says the RIGHT band matches the map on 12-23% of its
> detailed pixels and the BOTTOM band on 36-55%, while the guest window is
> 100.0% and the LEFT band 99.9%.

That is a large, specific discrepancy and it bears directly on the reported
shroud defects. The obvious move is to re-measure it. **It cannot currently be
measured**, and this records why so the next attempt does not rediscover it.

## What the audit needs

`tools/lb_bandaudit.py` pairs three dumps by frame number, from one run:

```
lb_comp-*.png     the composited frame
lb_map-*.png      the whole level decoded from RAM, no fog, no units, no HUD
lb_layers-*.pgm   which layer won each pixel
```

`lb_comp` and `lb_layers` come from `LB_DUMP_ADAPTIVE` and `LB_LAYERDUMP` and
arrive fine — 37 of each in one run. `lb_map` comes from `LB_MAPDUMP` and does
not arrive at all.

## Why lb_map never appears

The dump is written **inside the "Battlefield" ImGui panel's draw function**
(`lb_overlay.cpp`, immediately after the panel's own `dl->AddRect`), behind:

```c
if (!g_field_visible || !renderer) return;
if (!ImGui::Begin("Battlefield", &g_field_visible)) { ImGui::End(); return; }
```

So `LB_MAPDUMP` alone does nothing: the panel has to be open. It is opened by
`LB_FIELDPANEL`, which `g_field_visible` reads at startup.

## And opening the panel breaks the route

With `LB_FIELDPANEL=1` the run produces **zero** dumps of any kind — worse than
without it. The log says why:

```
[lb_band] 786x376 ... cam=0,0 map=0x0px geom=0 ... fill ok=0
[lb_polypeak] peak 0/2048 polygons, 0/6144 vertices
```

`map=0x0px` and zero polygons mean no battle ever loaded. An open ImGui window
takes the mouse, `lb_overlay_wants_mouse()` then suppresses guest touch
delivery so a click on a panel is not also a stylus tap — correct behaviour, and
it means the route's menu clicks never reach the game.

**The audit and the route are mutually exclusive as currently built.** The
measurement needs a battle; reaching a battle needs the panel shut; the dump
needs the panel open.

## The fix, which is small and is next

Move the `lb_map` dump **out of the panel draw**. It needs only two things the
panel happens to compute: `g_field_px`, the level decoded from the grid in RAM,
and the `cx, cy, vw, vh` rectangle the guest surface covers. Neither depends on
ImGui, on the panel being visible, or on a renderer. Emitting it from the same
place the other two dumps are emitted would make all three land together on the
same frame cadence, with the panel shut and the route intact.

Until then, the 12-23% figure should be treated as **undated**: it was true when
measured, and nothing since has been able to confirm or refute it.

## Not to be confused with docs/290

docs/290's seams are a phase discontinuity in the shroud's tiling, measured from
composited frames and confirmed screen-locked by a camera pan. This document is
about a *tool* that cannot run. They point at the same region of the code — the
band fill served by `lb_widemap_tile` / `lb_widemap_shroud` / `lb_widemap_fog`,
keyed on absolute tile columns — but one is a measured defect and the other is a
measurement that is unavailable. Neither has been shown to cause the other.
