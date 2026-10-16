# 266 — The wide-surface emulation cost is the band's units, not the rect

docs/264 left the port about 0.6 ms short of 60 fps at a 3440x1440 window with
magnification 2 and named the gap "the emulation-side surface dependence":
emu_ticks is 10.94 ms/frame at a 946x516 surface and 11.9-12.1 at 1712x768, for
the same build, the same map and the same camera. The plan recorded there was to
build `LB_GX_PROFILE` and time three sites — `SoftRenderer::VCount144`'s
`Semaphore_Wait(Sema_RenderDone)`, `GetLine(y)`'s wait, and gpu2d.cpp's wide-line
snapshot copy.

That patch was not built, because a knob that already exists answers it.

## The measurement

One build, one surface (1740x780 window, pixel_scale 1), three runs per arm.
`LB_CULLWIDE=0` turns off `apply_cull_width` and nothing else — it exists so the
widened entity gather rect can be A/B'd against itself without turning off the
whole widening and changing the surface underneath the probe.

```
                         fps          emu_ticks      present   drain    underruns/6s
default             56.0 57.0 56.1   12.15 11.94 12.05   5.63    0.002    218 233 229
LB_CULLWIDE=0       59.5 59.5 58.6   10.75 10.77 10.95   5.56    0.45      21  30  43
946x516, cull on    59.7 60.1 59.7   10.96 10.97 10.89   2.54    3.17       3   0   0
```

emu at the WIDE surface with the gather rect off (10.82 mean) is emu at the
NARROW surface with it on (10.94 mean). So the whole surface dependence is the
widened gather rect. The 2D decode over `decoded_width`, the wide-line snapshot
copy, the render thread's two semaphore waits and `ClearBuffers` writing
15.19 MiB a frame contribute nothing this measurement can see —
`LB_GX_PROFILE` would have instrumented three innocent sites, which is what
docs/264's own note ("IF rendersync and getline are near zero the render thread
is not on the critical path — do not build it") asked for.

Audio is the same finding stated twice: 218-233 underruns per 6 s against 21-43.
With no slack in the frame the queue starves about five times a second.

## THE OBVIOUS FIX WAS TRIED AND DOES NOTHING. That is the finding.

`apply_cull_width` sized every edge from `lb_band_span`, the WHOLE overhang,
rather than from the half actually on that side, so the rect the guest walks is

```
   (256 + 2*1456) x (192 + 2*576)  =  3240 x 1360  =  4.41 Mpx
   visible                            1712 x  768  =  1.31 Mpx
```

3.25x the area, every frame, inside the guest's own gather loop. That looks
exactly like the 1.23 ms. It is not.

Each edge was rewritten to come from the CURRENT split (`lb_band_left`,
`lb_band_right`, `lb_band_top`, `lb_band_bottom`) quantised up to 96 px
horizontally and 64 px vertically — still a strict superset, falling back to the
old whole-span sizing whenever the width in hand is not the latched one, so it
could not under-gather at a map edge. The runtime log confirms it took effect:

```
   before   left camx-1512  right camx+1728  top camy-592  bottom camy+768
   after    left camx- 792  right camx+1024  top camy-336  bottom camy+512
```

1816 x 848 = 1.54 Mpx, a 2.9x reduction, superset intact (the band's half-split
is 728 px against margins of 792 and 768; 288 px against 336 and 320).

```
                         fps               emu_ticks
   4.41 Mpx rect    56.0 57.0 56.1    12.15 11.94 12.05
   1.54 Mpx rect    56.9 56.0 57.5    11.98 12.18 11.88
```

Nothing. The change was reverted.

## What the 1.23 ms actually is

Not the area the guest walks — the ENTITIES the rect lets through. `LB_ENTLOG=1`
at this surface:

```
[lb_ent] cam=(408,144) 17 entities: 0 on screen natively, 12 IN A WIDENED COLUMN
```

With the stock rect the guest gathers about three; with any rect that reaches
the band it gathers all seventeen. Halving a rect that already contains every
one of them changes nothing. So the cost is roughly 90 us per extra entity, and
it is the price of the band's units existing in the frame at all — which is the
feature, not waste. It cannot be optimised away from the host side, and it is
guest code.

## Where the time actually is left

The phase breakdown, same three arms:

```
                     emu     present   adaptive  upload  draw  swap
   default         12.147     5.654     4.416    0.429  0.104 0.698
   LB_CULLWIDE=0   10.751     5.531     4.318    0.418  0.102 0.686
   LB_WIDEMAP=0    10.825     3.738     2.505    0.435  0.089 0.704
```

`adaptive_ticks` is `nds_gpu2d_adaptive_framebuffer()` for both screens — the
host-side row composite that builds the widened surface, i.e. lb_wide2d.inc's
worker pool. 4.4 ms of a 16.71 ms budget, of which 1.8 ms is compositing real
content in the band rather than black.

That is now the largest single item that is neither the guest nor the feature's
own cost: it is host code, it cannot affect the simulation or the lockstep, and
1.33 Mpx at 3.3 ns/pixel across the pool is not obviously the floor. It is the
next place to look, and unlike the gather rect it has not yet been measured from
the inside.
