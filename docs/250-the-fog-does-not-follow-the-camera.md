# 250 — "The fog follows the camera": not reproduced on the current build

Status: **investigated, not reproduced, cause identified as build skew.**

## The report

The reported defect: the fog appeared to follow the camera, the starting uncovered area moving with the view pan.

## What was measured

Two boxes, printed every report, both in MAP coordinates.

**The record's box** — the bounding box of every metatile the game's own
visibility record calls revealed:

    revealed 413/4096  box 0..22,0..22   cam_tx 17
    revealed 413/4096  box 0..22,0..22   cam_tx 39
    revealed 413/4096  box 0..22,0..22   cam_tx 96
    revealed 413/4096  box 0..22,0..22   cam_tx 128
    revealed 413/4096  box 0..22,0..22   cam_tx 142

**Fixed at metatiles 0..22 while the camera crossed 125 tiles.** The record is
map-anchored and the port indexes it correctly. Explored ground stays where it
is in the world.

**The drawn fog's box** — the same conversion applied to the cells `g_ext2`
actually fogs, using the camera the table was filled from:

    fillcam  17  ->  -59..80
    fillcam  43  ->  -50..89
    fillcam 144  ->  -16..122

This one does move with the camera, and that is correct: `g_ext2` covers a
fixed window *around* the camera, so the set of cells it describes travels with
the view. Its content is "everything fogged" because at `cam_tx 144` the camera
has left the explored corner entirely — which is also why the screen is solid
cloud in those captures, correctly.

So on the current build the fog is map-anchored end to end.

## A retraction of my own evidence

An earlier pass reported the shroud boundary sitting at "column 674, then 305
in four consecutive frames" and read that as screen-locking. **That measurement
was wrong.** The detector scanned full-height columns and counted the HUD as
non-shroud: the resource icons sit at x≈14–55 and the cancel button at the
bottom-left, so the run it found started at column 18 and had nothing to do
with fog. Re-measured in a HUD-free row band, it shows no such thing.

The isolated rectangular patches seen in one capture remain unexplained, but a
single frame caught mid-pan is as likely to be the guest's own tilemap
mid-rewrite — the game scrolls by rewriting the tilemap, so some columns lag
during a fast pan.

## The likely cause: the build being played is nine commits old

`build/LegoBattles.exe` is stamped **2026-09-12 10:57**. Every recent fog
change lands after it:

    f22a7ebd  locate the fog record instead of correlating for it
    46c2231d / a53a2147  seed the fog tiles
    7c0bf0fd  cardinal edge variants
    2f2eae25  reject non-metatiles in the edge learner

In particular, that build still has `g_fogsrc_map` defaulting to the
**window-derived cache**, which is explicitly a record of "which map tiles the
guest's own 32x24 window has personally covered" — a camera-history record —
with the old correlation search adopting and dropping wrong buffers underneath
it. Fog that appears to travel with the view is what that combination
produces, and it is the thing f22a7ebd replaced.

Nothing here rules out a real residual bug. It says the report cannot be
reproduced against current code, and that the most likely explanation is that
the reported behaviour was fixed before it was reported.

## RETRACTED: the "22 band cells drawn clear" reproduction

A probe was added to count band cells drawn `kClearTile` where the record calls
the metatile hidden -- the patches, as a number. It reported 22, then 46, then
92 as pan steps were added, and that was taken as the reported fault
reproduced.

**It was measuring its own staleness.** The probe sat in `scan_shroud`'s
periodic report, and the order inside `lb_widemap_scan` is

    fill_terrain -> scan_shroud (the report) -> build_vis_bits -> the g_ext2 fill

so it inspected the **previous** frame's `g_ext2` against the **current**
camera and record. During a pan that mislabels cells wholesale, and the count
duly grew with the length of the pan.

The giveaway was available in the same runs and was not read correctly at
first: an assignment-time probe fired **zero** times. Its first version tested
only `!s`, which was too narrow -- the assignment is
`(s && have_fog) ? fog : kClearTile`, so `!have_fog` also yields a clear cell.
Widened to cover both terms it still fired zero times. A cell that is never
written that way cannot be written that way, and the disagreement was therefore
in the reader, not the fill.

Moved after the fill, where the table, the camera and the record are all the
same frame's, the count is **0**. `declined_out_of_table` is also **0**, so no
band cell falls through to the hardware wrap either.

So the band fill is clean on both counts, and the isolated square patches
visible in the play-run captures are **not** explained by this. They
remain real and unexplained; the next place to look is which LAYER supplies
those pixels, since BG1 terrain and BG2 fog are separate tables and only BG2
was under test here.

The rule this cost: a probe that reads shared state must be placed where that
state is the same age as everything it is compared against. Two recent
wrong answers -- this one and the HUD-contaminated boundary scan --
were faults in the instrument, not the port.
