# 172 — The whole-battlefield renderer, now inside the port

Status: **ported to C++ and verified against the Python reference.**
`port/runner/src/lb_field.{h,cpp}`.

docs/167 established that the emulated 2D path can never show more field than
the DS does. docs/168 and docs/169 worked out how to draw the whole map from the
level grid instead, in `tools/lb_field.py`. A Python tool driving the debug
server is fine for research and useless as a port feature, so the algorithms now
live in the runner.

## What it does

    bool lb_field_locate(LbFieldInfo*);        // once per level; scans main RAM
    void lb_field_size(const LbFieldInfo*, int* w, int* h);
    bool lb_field_render(const LbFieldInfo*, uint32_t* dst);   // per frame
    void lb_field_camera_rect(const LbFieldInfo*, int* x, int* y);

Nothing is hardcoded. Every structure is located at runtime by matching against
what the guest is displaying, because the addresses move between levels and
pretending otherwise would be guessing.

## It finds the same structures, independently

Run against a warped save with `LB_FIELD_DUMP`:

    [lb_field] primary=022BC0CA second=022822AE count=5120 grid=0227F730
               64x63 phase=(1,0) cam_cell=(4,9) cam_px=(88,144) -> 1536x1008

Every value matches what `tools/lb_field.py` found by a separate implementation
against a separate copy of memory.

One difference is an improvement rather than a discrepancy: **`cam_cell` is
(4,9), not (0,9)**. The Python version's grid-base walk went upward in whole-row
strides from the camera's own row, which forces the reported camera column to 0
by construction (docs/169). The C++ version anchors the base on the object header
that precedes the array — its last two words are RAM pointers — so the camera
column is now a real measurement. That is what makes `lb_field_camera_rect()`
possible, and therefore what a camera box drawn on a full-map view would use.

## Verified against the reference render

Pixel-comparing the C++ output with `tools/lb_field.py`'s:

    1460013 / 1548288 pixels identical  (94.30%)

The residual is **animated water**: of the 88,275 differing pixels, **86,337
(97.8%) fall inside the lake**, and disagreement outside water is 1,938 pixels —
**0.13% of the map**. The two renders were taken at different moments and the
water tiles animate, so this is the expected result rather than a defect. Worth
stating explicitly, because "94%" read alone looks like a porting bug.

## The lessons the code carries

The comments in `lb_field.cpp` record the failures rather than only the answers,
because each cost a measurement:

* the metatile table is anchored on a **generated-table signature** (first
  entries reference tiles 0..5 then 6..11). Two weaker anchors both found the
  level grid instead — taking the first RAM occurrence of an on-screen block,
  and accepting any run where "all six values look like tile indices".
* the grid probe is the **most varied row**, because a row of pure grass matches
  almost everywhere.
* the grid base is **header-anchored**, because a base wrong by k cells rolls the
  map k cells horizontally with wraparound rather than shearing it, so it still
  looks coherent (docs/169).
* the palette expansion goes through a **6-bit intermediate**, melonDS style; a
  naive 5->8 shift is off by two in green and made every pixel differ.
* which of the two metatile tables a cell uses needs **no threshold**: the
  primary wins unless its entry references a tile that is blank in VRAM.

## Cost

`lb_field_locate()` copies main RAM once and scans it, so it is a per-level
operation, not a per-frame one. `lb_field_render()` reads the grid and tiles
through the bus and draws 1536x1008 in well under a frame.

## Not done yet

The renderer exists and is correct; nothing presents it to the player. The next
step is a consumer — the natural one being a full-map view with the camera
rectangle from `lb_field_camera_rect()` and unit markers from the entity
coordinates in docs/169, which is also what the port's requirement that the
minimap match the window's coverage asks for.

Fog of war is still not applied: the render draws the whole level regardless of
what has been scouted. docs/171 wired the cartridge's own **Reveal Map** control,
which is the honest way to make the two agree.
