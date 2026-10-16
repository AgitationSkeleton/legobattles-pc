"""Which layer wins, in the band versus in the guest's own picture?

The dump stores (Pixel::order + 1) * 40, where order is the equal-priority
order OBJ, BG0, BG1, BG2, BG3, and 0 means nothing opaque so the backdrop
shows. Reports a histogram for three regions so "the band is covered by an
overlay" and "the band draws nothing" and "the band draws terrain badly" can be
told apart, which pixel colour cannot do.
"""
import glob
import os
import re
import sys

# 6 is the BACKDROP -- nothing was drawn on that pixel. The backdrop Pixel
# is valid with order 5, so it records as 6; 0 never occurs.
NAMES = {0: "unset", 1: "OBJ", 2: "BG0/3D", 3: "BG1", 4: "BG2", 5: "BG3",
         6: "NOTHING DRAWN"}


def read_pgm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    # P5\nW H\n255\n
    m = re.match(rb"P5\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise SystemExit("not a P5 pgm: %s" % path)
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():]
    return w, h, px


def hist(px, w, x0, x1, y0, y1):
    counts = {}
    for y in range(y0, y1):
        base = y * w
        for x in range(x0, x1):
            v = px[base + x] // 40
            counts[v] = counts.get(v, 0) + 1
    return counts


def show(label, counts):
    total = sum(counts.values()) or 1
    parts = []
    for v in sorted(counts, key=lambda k: -counts[k]):
        parts.append("%s %.1f%%" % (NAMES.get(v, "?%d" % v),
                                    100.0 * counts[v] / total))
    print("    %-14s %s" % (label, "  ".join(parts[:5])))


for path in sorted(glob.glob(sys.argv[1] + "/lb_layers-*.pgm")):
    w, h, px = read_pgm(path)
    if len(px) < w * h:
        print("%s TRUNCATED" % os.path.basename(path))
        continue
    extra = (w - 256) // 2
    extra_y = (h - 192) // 2
    print("%s  extra=%d extra_y=%d" % (os.path.basename(path), extra, extra_y))
    if extra > 0:
        show("LEFT band", hist(px, w, 0, extra, max(0, extra_y),
                               min(h, extra_y + 192)))
        show("RIGHT band", hist(px, w, extra + 256, w, max(0, extra_y),
                                min(h, extra_y + 192)))
    show("GUEST window", hist(px, w, extra, extra + 256,
                              max(0, extra_y), min(h, extra_y + 192)))
    if extra_y > 0:
        show("TOP band", hist(px, w, 0, w, 0, extra_y))
        show("BOTTOM band", hist(px, w, 0, w, extra_y + 192, h))
    print()
