"""Find thin holes in the widened field's fog, from LB_LAYERDUMP maps.

    python tools/lb_thinholes.py screenshots/exp-<name> [...]

LB_LAYERDUMP=N makes the port write lb_layers-WxH-F.pgm every N composites:
per surface pixel, (the winning layer's order + 1) * 40, so 1 = OBJ, 2 = BG0
(3D), 3 = BG1 (terrain), 4 = BG2 (fog), 5 = BG3, 6 = backdrop, 0 = nothing.

A HOLE is a run where something BEHIND the fog wins -- terrain, the 3D layer
or the backdrop -- while the fog wins on both sides of the run over its whole
length: 1 or 2 px tall with fog directly above and below, or 1 or 2 px wide
with fog directly left and right, at least --min px long. The cartridge draws
its fog in 8x8 tiles, so the fog's own edges cannot make a feature that thin
with cloud on both sides (docs/352). What the 3D layer draws in front of the
fog is not a hole, so a run that OBJ wins breaks it.

BLIND SPOT: the surface's own edge rows and columns have no neighbour on the
outer side, so a hole along them is not reported. docs/352's second hole was
on the last surface row, and an A/B of two builds' maps is what found it.

FALSE POSITIVE: a FADE. The winner map records layers, not brightness. The
first widened composite of a battle is entirely black (the fade-in), while
BG2 holds a dithered load-time pattern in alternating columns. That reads as
60 one-pixel "holes" nobody can see (2026-09-24, resize sweep, composite 45).
Before reporting a hole, check the matching lb_comp-*.png is not all black.
"""
import argparse
import os
import re
import sys

BG2 = 4
OBJ = 1


def read_pgm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    if parts[0] != b"P5":
        raise ValueError("%s: not a binary PGM" % path)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def scan(path, min_len):
    w, h, px = read_pgm(path)
    win = [v // 40 for v in px]

    def behind(i):
        return win[i] not in (BG2, OBJ)

    found = []
    for t in (1, 2):
        for y in range(1, h - t):
            x = 0
            while x < w:
                def hole_at(xx):
                    return (all(behind((y + k) * w + xx) for k in range(t)) and
                            win[(y - 1) * w + xx] == BG2 and
                            win[(y + t) * w + xx] == BG2)
                if not hole_at(x):
                    x += 1
                    continue
                x0 = x
                while x < w and hole_at(x):
                    x += 1
                if x - x0 >= min_len:
                    found.append(("row", y, t, x0, x - 1))
        for x in range(1, w - t):
            y = 0
            while y < h:
                def hole_at_y(yy):
                    return (all(behind(yy * w + x + k) for k in range(t)) and
                            win[yy * w + x - 1] == BG2 and
                            win[yy * w + x + t] == BG2)
                if not hole_at_y(y):
                    y += 1
                    continue
                y0 = y
                while y < h and hole_at_y(y):
                    y += 1
                if y - y0 >= min_len:
                    found.append(("col", x, t, y0, y - 1))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dirs", nargs="+", help="run directories holding lb_layers-*.pgm")
    ap.add_argument("--min", type=int, default=4, help="shortest run reported (px)")
    args = ap.parse_args()
    grand = 0
    for d in args.dirs:
        maps = [n for n in os.listdir(d)
                if n.startswith("lb_layers-") and n.endswith(".pgm")]
        maps.sort(key=lambda n: int(re.findall(r"(\d+)\.pgm$", n)[0]))
        total = 0
        for n in maps:
            found = scan(os.path.join(d, n), args.min)
            total += len(found)
            print("%s  %s: %d thin hole(s)" % (os.path.basename(os.path.normpath(d)), n, len(found)))
            for kind, a, t, lo, hi in found[:12]:
                if kind == "row":
                    print("    %d px tall at surface y %d, x %d..%d (%d px)" % (t, a, lo, hi, hi - lo + 1))
                else:
                    print("    %d px wide at surface x %d, y %d..%d (%d px)" % (t, a, lo, hi, hi - lo + 1))
        print("%s: %d map(s), %d thin hole(s)" % (os.path.basename(os.path.normpath(d)), len(maps), total))
        grand += total
    return 1 if grand else 0


if __name__ == "__main__":
    sys.exit(main())
