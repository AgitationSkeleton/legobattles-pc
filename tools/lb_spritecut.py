"""Are unit sprites CUT at the native viewport boundary?

The reported defect: unit and building sprites truncate as the camera pans. Units
are the 3D layer (BG0), which is rendered across the whole widened surface, so
a sprite cut at the boundary of the ORIGINAL 256x192 viewport would be a port
defect -- the 3D raster stopping where the guest's own picture stops.

A single sprite has edges everywhere, so one frame proves nothing. This sums
unit pixels PER COLUMN and PER ROW over every dump in a run: a real cut shows
as a cliff at exactly the boundary column, while ordinary sprite edges spread
out and average away.

Two things are deliberately NOT counted as cuts:

  * the surface's own outer edge -- a sprite leaving the window is not a bug;
  * fog. BG2 draws IN FRONT of BG0, so a unit at a fog edge is legitimately
    covered, exactly as on the cartridge. A pixel that lost to fog is simply
    not a unit pixel here, which is why the fog quality and this are separate
    questions.

Usage: lb_spritecut.py <run-directory>
"""
import glob
import os
import sys

from PIL import Image

RUN = sys.argv[1]
UNIT = 2          # (order + 1) for BG0, the 3D layer
SPAN = 10         # columns/rows reported each side of a boundary

colsum = {}
rowsum = {}
dumps = 0

for lp in sorted(glob.glob(os.path.join(RUN, "lb_layers-*.pgm"))):
    im = Image.open(lp).convert("L")
    w, h = im.size
    px = im.tobytes()
    extra, extra_y = (w - 256) // 2, (h - 192) // 2
    if extra <= 0 and extra_y <= 0:
        continue
    dumps += 1

    # Relative to each boundary, so dumps of different sizes can be summed.
    for y in range(h):
        base = y * w
        for x in range(w):
            if px[base + x] // 40 != UNIT:
                continue
            for name, b in (("L", extra), ("R", extra + 256)):
                d = x - b
                if -SPAN <= d < SPAN:
                    colsum[(name, d)] = colsum.get((name, d), 0) + 1
            for name, b in (("T", extra_y), ("B", extra_y + 192)):
                d = y - b
                if -SPAN <= d < SPAN:
                    rowsum[(name, d)] = rowsum.get((name, d), 0) + 1

print("unit (BG0/3D) pixels near the ORIGINAL viewport's edges, summed over "
      "%d dumps." % dumps)
print("offset 0 is the first column/row INSIDE the original 256x192 picture;")
print("negative is outside it, in the widened band.")
print("A cut at the boundary reads as a cliff between -1 and 0.")
print()
for name, label in (("L", "left edge"), ("R", "right edge"),
                    ("T", "top edge"), ("B", "bottom edge")):
    src = colsum if name in ("L", "R") else rowsum
    vals = [(d, src.get((name, d), 0)) for d in range(-SPAN, SPAN)]
    if not any(v for _, v in vals):
        print("%-11s no unit pixels within %d of this edge -- says nothing"
              % (label, SPAN))
        continue
    print("%-11s %s" % (label,
                        "  ".join("%+d:%d" % (d, v) for d, v in vals)))
    outside = sum(v for d, v in vals if d < 0)
    inside = sum(v for d, v in vals if d >= 0)
    print("%-11s outside %d, inside %d%s"
          % ("", outside, inside,
             "   <-- NOTHING OUTSIDE: units stop at the edge"
             if outside == 0 and inside > 0 else ""))
