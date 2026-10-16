"""Per-ROW match rate through a composite, to locate where a band fails.

lb_bandaudit.py says BOTTOM is 87-91% at the two tall sizes while every other
region is 100%. A percentage cannot say whether that is a thin stripe that is
completely wrong or a haze spread over the whole band, and those have different
causes: a stripe at the far edge is an extension limit, a haze is a lookup that
is subtly off everywhere.

Same alignment and same detail gate as lb_bandaudit -- this only changes how the
result is bucketed. The guest rows stay in the output as the control.

Usage: lb_bandrows.py <run-directory> [composite-basename]
"""
import glob
import os
import re
import sys

from PIL import Image

RUN = sys.argv[1]
ONLY = sys.argv[2] if len(sys.argv) > 2 else None
DX, DY = -4, 0
TOL = 24
DETAIL = 40


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

for cp in comps:
    if ONLY and ONLY not in os.path.basename(cp):
        continue
    cf = frame_of(cp)
    mp = min(maps, key=lambda m: abs(frame_of(m) - cf))
    meta_path = mp[:-4] + ".txt"
    if not os.path.exists(meta_path):
        continue
    meta = txt(meta_path)
    ci = Image.open(cp).convert("RGB")
    if meta.get("vw") != ci.size[0] or meta.get("vh") != ci.size[1]:
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

    print("%s  %dx%d  guest rows [%d,%d)  map %dx%d  cam (%d,%d)"
          % (os.path.basename(cp), cw, ch, extra_y, extra_y + 192, mw, mh,
             meta["cx"], meta["cy"]))
    for y in range(0, ch):
        my = oy + y
        good = tot = 0
        if 0 <= my < mh:
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
                tot += 1
                if (abs(cpx[o] - mpx[q]) <= TOL and
                        abs(cpx[o + 1] - mpx[q + 1]) <= TOL and
                        abs(cpx[o + 2] - mpx[q + 2]) <= TOL):
                    good += 1
        tag = "guest" if extra_y <= y < extra_y + 192 else "band "
        if tot < 8:
            print("  y=%3d my=%4d %s        -- %d px" % (y, my, tag, tot))
        else:
            print("  y=%3d my=%4d %s %6.1f%% of %4d %s"
                  % (y, my, tag, 100.0 * good / tot, tot,
                     "" if good == tot else "<<"))
    print()
