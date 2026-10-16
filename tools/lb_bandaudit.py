"""Match rate per region, counted only where the map has real DETAIL.

Uniform grass matches almost any offset, which is how the previous search found
a 99.2% "peak" for the LEFT band at a shift of (-197,+253) when that band is
provably 100% correct at (-4,0). Scoring flat ground proves nothing.

So: compare only pixels whose neighbourhood in the MAP render actually varies.
A band drawing the right ground scores high there; a band drawing something
else cannot, because distinctive terrain does not coincide by chance.

The alignment is fixed at the offset the GUEST WINDOW proves correct, rather
than searched per region -- searching per region is what let self-similarity in.

Usage: detailcmp.py <run-directory> [dx] [dy]
"""
import glob
import os
import re
import sys

from PIL import Image

RUN = sys.argv[1]
DX = int(sys.argv[2]) if len(sys.argv) > 2 else -4
DY = int(sys.argv[3]) if len(sys.argv) > 3 else 0
TOL = 24
DETAIL = 40          # min local range in the map, per channel sum


def frame_of(p):
    m = re.search(r"-(\d+)\.(png|pgm)$", p)
    return int(m.group(1)) if m else -1


def txt(p):
    out = {}
    for line in open(p):
        q = line.split()
        if len(q) == 2:
            out[q[0]] = int(q[1])
    return out


comps = sorted(glob.glob(os.path.join(RUN, "lb_comp-*.png")))
maps = sorted(glob.glob(os.path.join(RUN, "lb_map-*.png")))
layers = sorted(glob.glob(os.path.join(RUN, "lb_layers-*.pgm")))

print("alignment (%+d,%+d); counting only map pixels with local detail >= %d"
      % (DX, DY, DETAIL))
for cp in comps:
    cf = frame_of(cp)
    mp = min(maps, key=lambda m: abs(frame_of(m) - cf))
    meta_path = mp[:-4] + ".txt"
    if not os.path.exists(meta_path):
        continue
    meta = txt(meta_path)
    ci = Image.open(cp).convert("RGB")
    # THE TWO DUMPS MUST BE THE SAME FRAME.
    #
    # lb_comp/lb_layers are numbered by the compositor's own counter and lb_map
    # by the panel's, and the two tick in different places, so "nearest frame
    # number" can pair a composited image with a camera rect it was never drawn
    # with. That is exactly what produced the retracted "RIGHT band 12-23%":
    # a map sidecar claiming 386x276 sitting beside a 306x216 layer map. The
    # sidecar records the surface it saw, so demand it match.
    if meta.get("vw") != ci.size[0] or meta.get("vh") != ci.size[1]:
        print("%-26s SKIPPED: map sidecar says surface %sx%s, this composite is "
              "%dx%d -- different frames, not comparable"
              % (os.path.basename(cp), meta.get("vw"), meta.get("vh"),
                 ci.size[0], ci.size[1]))
        continue
    mi = Image.open(mp).convert("RGB")
    cw, ch = ci.size
    mw, mh = mi.size
    cpx, mpx = ci.tobytes(), mi.tobytes()
    lay = [l for l in layers if frame_of(l) == cf and Image.open(l).size == (cw, ch)]
    if not lay:
        continue
    lpx = Image.open(lay[0]).convert("L").tobytes()

    extra, extra_y = (cw - 256) // 2, (ch - 192) // 2
    ox, oy = meta["cx"] - extra + DX, meta["cy"] - extra_y + DY

    def detailed(mx, my):
        if mx < 1 or my < 1 or mx + 1 >= mw or my + 1 >= mh:
            return False
        lo = [255, 255, 255]
        hi = [0, 0, 0]
        for yy in (my - 1, my, my + 1):
            for xx in (mx - 1, mx, mx + 1):
                q = (yy * mw + xx) * 3
                for c in range(3):
                    v = mpx[q + c]
                    if v < lo[c]:
                        lo[c] = v
                    if v > hi[c]:
                        hi[c] = v
        return (hi[0] - lo[0]) + (hi[1] - lo[1]) + (hi[2] - lo[2]) >= DETAIL

    def region(x, y):
        if extra_y > 0 and y < extra_y:
            return "TOP"
        if extra_y > 0 and y >= extra_y + 192:
            return "BOTTOM"
        if x < extra:
            return "LEFT"
        if x >= extra + 256:
            return "RIGHT"
        return "guest"

    stats = {}
    for y in range(0, ch):
        my = oy + y
        if my < 0 or my >= mh:
            continue
        for x in range(0, cw):
            mx = ox + x
            if mx < 0 or mx >= mw:
                continue
            if lpx[y * cw + x] // 40 != 3:
                continue
            if not detailed(mx, my):
                continue
            o = (y * cw + x) * 3
            q = (my * mw + mx) * 3
            ok = (abs(cpx[o] - mpx[q]) <= TOL and
                  abs(cpx[o + 1] - mpx[q + 1]) <= TOL and
                  abs(cpx[o + 2] - mpx[q + 2]) <= TOL)
            s = stats.setdefault(region(x, y), [0, 0])
            s[1] += 1
            if ok:
                s[0] += 1

    parts = []
    for r in ("LEFT", "RIGHT", "TOP", "BOTTOM", "guest"):
        if r in stats and stats[r][1] >= 30:
            good, tot = stats[r]
            parts.append("%s %.1f%% (%d)" % (r, 100.0 * good / tot, tot))
    print("%-26s %-10s %s" % (os.path.basename(cp), "%dx%d" % (cw, ch),
                              "  ".join(parts) if parts else "too few detailed px"))
