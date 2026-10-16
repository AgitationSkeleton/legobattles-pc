"""Do UNITS draw in the widened band, or are they culled at the native edge?

Terrain and fog in the band are graded against the cartridge by time. Sprites
cannot be: an entity's position is not a property of the map, so there is no
"what should this absolute cell hold" to check against later.

What IS decidable from the picture alone is CULLING. The game gathers entities
into a rect built from the camera (camx..camx+0x100), so if that rect was never
widened, sprites stop dead at the native boundary -- column `extra` on the left
and `extra + 256` on the right. That is a step in the data, not a judgement
call: count sprite pixels per column and look at what happens across those two
columns.

The layer map records the WINNER per pixel as (order + 1) * 40 with the order
OBJ, BG0, BG1, BG2, BG3. UNITS ARE BG0, the 3D layer, not OBJ: a first version
of this counted OBJ and reported zero everywhere INCLUDING the guest window,
which is the control saying the test was broken rather than the port. Both are
counted now, and the guest window is printed beside the bands for exactly that
reason.

A unit pixel that lost to a nearer layer is not counted, which biases against
finding units in the band, not for it. Fog (BG2) is nearer than the 3D layer,
so a run panned out into unexplored ground scores zero everywhere honestly --
check the guest column before reading anything into a band's zero.

Usage: lb_objband.py <run-directory>
"""
import glob
import os
import re
import sys

from PIL import Image

RUN = sys.argv[1]
# (order + 1); order is OBJ, BG0, BG1, BG2, BG3.
UNIT_IDS = {1: "OBJ", 2: "BG0/3D"}


def frame_of(p):
    m = re.search(r"-(\d+)\.(png|pgm)$", p)
    return int(m.group(1)) if m else -1


tot = {}
edge_rows = []
for lp in sorted(glob.glob(os.path.join(RUN, "lb_layers-*.pgm"))):
    im = Image.open(lp).convert("L")
    w, h = im.size
    px = im.tobytes()
    extra, extra_y = (w - 256) // 2, (h - 192) // 2
    if extra <= 0 and extra_y <= 0:
        continue

    col = [0] * w
    reg = {}
    for y in range(h):
        base = y * w
        for x in range(w):
            lid = px[base + x] // 40
            if lid not in UNIT_IDS:
                continue
            col[x] += 1
            if extra_y > 0 and y < extra_y:
                r = "TOP"
            elif extra_y > 0 and y >= extra_y + 192:
                r = "BOTTOM"
            elif x < extra:
                r = "LEFT"
            elif x >= extra + 256:
                r = "RIGHT"
            else:
                r = "guest"
            reg[(UNIT_IDS[lid], r)] = reg.get((UNIT_IDS[lid], r), 0) + 1

    for k, v in reg.items():
        tot[k] = tot.get(k, 0) + v

    # The cull signature: sprite pixels present right up to the native edge and
    # absent immediately outside it. Averaged over 8 columns each side so a
    # single empty column of map cannot fake a cliff.
    def band(a, b):
        a, b = max(0, a), min(w, b)
        return sum(col[a:b]) / max(1, b - a)

    edge_rows.append((os.path.basename(lp), w, h, extra,
                      band(extra - 8, extra), band(extra, extra + 8),
                      band(extra + 248, extra + 256),
                      band(extra + 256, extra + 264)))

print("unit pixels by region, summed over every layer dump "
      "(guest is the CONTROL -- if it is 0 the test saw no units at all):")
print("  %-8s %9s %9s %9s %9s %9s"
      % ("layer", "guest", "LEFT", "RIGHT", "TOP", "BOTTOM"))
for lid in ("OBJ", "BG0/3D"):
    print("  %-8s %9d %9d %9d %9d %9d"
          % (lid, tot.get((lid, "guest"), 0), tot.get((lid, "LEFT"), 0),
             tot.get((lid, "RIGHT"), 0), tot.get((lid, "TOP"), 0),
             tot.get((lid, "BOTTOM"), 0)))

print()
print("at the native boundary -- mean sprite pixels per column, 8 columns each "
      "side.\nA cull shows as a nonzero inner column against a zero outer one.")
print("  %-28s %6s %8s %8s %8s %8s"
      % ("dump", "extra", "L-out", "L-in", "R-in", "R-out"))
for name, w, h, extra, lo, li, ri, ro in edge_rows:
    print("  %-28s %6d %8.2f %8.2f %8.2f %8.2f"
          % (name, extra, lo, li, ri, ro))
