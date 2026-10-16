#!/usr/bin/env python3
"""Report the lit/black column bands of a dumped adaptive surface.

    python tools/lb_bands.py screenshots/exp-*/adaptive-0-*.ppm

The bands are the reported "vertical striping". Measuring them on the surface
the frontend actually uploads -- rather than on a window screenshot, where a
non-integer scale has already blurred every boundary -- is what makes the
column numbers mean anything: 96 is `extra`, and 96 and 352 are the native
region's own edges.

Also writes a .png beside each .ppm so the frame can simply be looked at.
"""

import os
import sys

from PIL import Image


def bands(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    lit = [max(sum(px[x, y]) for y in range(0, h, 4)) > 30 for x in range(w)]
    runs = []
    cur, n, start = lit[0], 0, 0
    for x, v in enumerate(lit):
        if v == cur:
            n += 1
        else:
            runs.append((cur, start, n))
            cur, n, start = v, 1, x
    runs.append((cur, start, n))
    im.save(os.path.splitext(path)[0] + ".png")
    return w, h, runs


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        w, h, runs = bands(path)
        label = os.path.basename(os.path.dirname(path))
        print("%-22s %dx%d" % (label, w, h))
        for is_lit, start, n in runs:
            print("    %-5s cols %3d..%-3d  (%d wide)"
                  % ("lit" if is_lit else "BLACK", start, start + n - 1, n))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
