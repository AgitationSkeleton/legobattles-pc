# Headless widescreen, and the field letterbox seen directly

## LB_FORCE_WIDTH: the two halves of the harness now work together

`nds_gpu3d_set_output_width` is called only from `frontend.cpp`, so widescreen
existed **only** in the interactive window -- and the interactive window refuses
`run_cycles`, because the frontend owns execution, so `tools/lb_warp.py` cannot
reach gameplay there. Widening was verifiable only where gameplay was
unreachable, and gameplay was reachable only where nothing widened.

    LB_FORCE_WIDTH=448      (serve mode; even, 256..448)

sets the width in serve mode. One detail was needed to make it work:

```
[lb] LB_FORCE_WIDTH 448 refused        <- first attempt
```

`nds_gpu3d_set_output_width` refuses a resize while
`IsRendererAccelerated()`. Serve mode has no GL surface anyway, so the hook
calls `nds_gpu3d_use_soft_renderer(false)` first:

```
[lb] serve output width 448            <- accepted
```

Now a single headless run can warp into a Free Play match **and** render at 448.

## What gameplay actually looks like at 448

```
field448_s0.png   448x192  margin=96  left-margin black    19/4608  filled
field448_s1.png   448x192  margin=96  left-margin black  4608/4608  LETTERBOX
```

Looking at the bottom screen: the match renders correctly in the **centre 256
columns** -- castle, units, terrain, all intact -- with **solid black bars 96
pixels wide on each side**.

That is exactly what `docs/117` and `docs/120` predict, now seen rather than
inferred:

* the field is engine A with 3D, so it takes the upstream adaptive path;
* that path needs tile data for the extra columns, and there is none -- BG2's
  second screen block is BG1's live map (`docs/117`), and engine A's BG VRAM is
  fully allocated (`docs/120`);
* so it centres the native image and leaves the margins black.

The letterbox is **correct behaviour given the constraint**, not a bug. It is
the honest alternative to inventing pixels, which is what the earlier edge-fill
did before it was removed.

## Where the widescreen work stands

| | at 448 wide | why |
| --- | --- | --- |
| **Menus** | **widened, verified** (`docs/127`) | 2D text BGs; `lb_wide2d` extends them from real tilemap data |
| **Game field** | **letterboxed** | no tile data exists for the extra columns; needs the guest-side work in `docs/119` plus VRAM that `docs/120` shows is unavailable |

Both halves of adaptive screen size (menus and game field) are now demonstrated on screen rather than
argued about, and the remaining gap has a specific, measured cause.
