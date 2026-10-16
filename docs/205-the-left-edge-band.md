# 205 - The left-edge band

Status: **FIXED.** It was self-inflicted: this port extended BG3 into the
widened columns with tile 0x1F, believing that tile transparent. It is not, and
BG3 is priority 0, so it painted over everything.

The file was first called `205-the-left-edge-stripe-is-3d.md`. That title was
wrong twice over and the history below is kept deliberately -- two confident
conclusions were drawn and retracted before the measurement that settled it.

## The fix

`lb_widemap_extends(3)` now returns 0. The widened columns get no BG3 source at
all, which is what they should have had all along: `hud_x` is negative out
there and the compositor skips a layer with no source column.

Measured before and after, column-averaged over the field rows, with BG2 masked
so the terrain underneath is visible:

    surface col:      81     85     89     93     97    101
    BG3 extended:  (39,155) (56,224) (58,227) (56,224) (58,227) (41,155)
    BG3 not:       (39,155) (40,154) (40,154) (39,154) (40,155) (41,155)

**The band is gone completely** -- and so is the part that had been attributed
to a pre-existing artefact. All sixteen columns of it were this bug.

## How it was found

The decisive measurement was dumping every layer's decoded colour across the
boundary in the same run as the capture, rather than reasoning about which
layer it might be:

    bg1 le=96 dw=448 prio=3: 9665 9665 9665 9665 92A6 9686 9665 9665 9665 9285 9665 9665
    bg3 le=96 dw=448 prio=0: 0000 0000 9665 9665 9665 9285 9665 9665 9665 9285 0000 0000
                              80   82   84   86   88   90   92   94   96   98  100  102

BG3 opaque across 84..98, transparent either side -- and the capture from the
same run puts the band at surface columns 84.8..99.7. Exact correlation.

## Two retractions on the way here

: "the left-edge stripe is the 3D layer"

The evidence was a layer-masking table: masking BG2 left the band, masking BG0
removed it. The second row is worthless. **Masking BG0 also makes the adaptive
widener decline** - its own diagnostic says `BG0 is not the 3D layer` - so that
run had no widened surface at all, and the band disappeared because the whole
picture changed, not because the 3D layer stopped drawing it.

Reading the 3D line directly settles it. At y=96, across the boundary:

    [lb_3dedge] w=448 extra=96 snapshot=1 y=96
    [lb_3dedge] 3d   80: 000000 000000 000000 000000 000000 000000 000000 000000
    [lb_3dedge] 3d   88: 000000 000000 000000 000000 000000 000000 000000 000000
    [lb_3dedge] 3d   96: 000000 000000 000000 000000 000000 000000 000000 000000

**The 3D layer is transparent everywhere the band is.** It cannot be drawing it.

The lesson is the same one this project keeps paying for: a probe that changes
more than the one thing it is supposed to change is not a controlled experiment.
`LB_BGMASK` was already gated to the field so it would stop breaking the route;
it also needed checking against what else clearing an enable bit turns off.

## What the band actually is

Not content -- **brightness**. Column-averaged from the capture, the band is the
same green as its surroundings, about 1.45x brighter:

    outside   (54, 157, 50)
    band      (63, 225, 60)

That is a blend or brightness-effect signature, not a wrong-tile signature.

## Ruled out, each by measurement

| candidate                  | result                                         |
|----------------------------|------------------------------------------------|
| the widened tilemap        | reproduces with `LB_WIDEMAP` unset             |
| the 3D layer               | 3D line is transparent across the band         |
| BG2 / the fog              | band present with BG2 masked                   |
| the adaptive skybox repair | band unchanged with `NDS_ADAPTIVE_SKY_REPAIR=0`|
| hardware windows           | the compositor implements none                 |

## Measured extent

Eyeballing a screenshot is not a measurement, so the band was extracted from
the capture by averaging each column over the field rows:

    x=148  (54, 156, 48)      outside
    x=150  (63, 225, 60)      <- band starts
    ...
    x=204  (65, 228, 60)
    x=206  (54, 155, 49)      outside

Screen x 150..205, 56 pixels.

Converting that needs a calibrated scale, not an assumed one. An earlier pass
here guessed 3.75 from the cover-fit arithmetic and placed the band "at the very
start of the native region". Measuring instead: with the widening off, the black
bar ends and content begins at **x = 191**, and content ends at **x = 1084**, so
the 256 native columns span 894 pixels and the scale is **3.4922** px per surface
column, not 3.75.

At that scale the band is **surface columns 84..100** -- about twelve columns
inside the widened region and four inside the native one, straddling the
boundary at 96 rather than starting at it. Width is still about 16 columns, two
tiles.

## Next

The band is applied at composite time, not in any layer's decoded line: the
per-layer dump shows BG1 holding a uniform 0x9665 straight across the boundary
while the composited image is brighter in the band. So the next measurement is
to print, per column, WHICH layer won the priority resolve and what the final
colour was.

The working hypothesis is BLDCNT: the compositor applies a brightness effect
when the winning layer is a blend target, so a band where a different layer
wins would be brightened while its neighbours are not. That is consistent with
the band tracking the boundary, where which layer is on top changes.
