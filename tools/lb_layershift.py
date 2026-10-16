#!/usr/bin/env python3
"""Sprite-vs-ground drift measured from COMPOSITED PIXELS (docs/342).

    python tools/lb_layershift.py screenshots/exp-<label>

Record with LB_DUMP_SEQ=N, LB_DUMP_EVERY=1 and LB_DUMP3D=1. The runner then
writes, for each recorded frame, the presented picture AND the 3D layer the
compositor actually used for it (`layer3d-*`, magenta where transparent).

WHY ANOTHER GATE. tools/lb_groundshift.py and tools/lb_spriteworld.py both read
sprite positions from the POLYGON LIST. A change made at composite time -- the
re-shift of docs/338, or pairing the live raster in docs/342 -- never touches
the polygon list, so both tools are blind to it by construction. LB_RESHIFT was
graded that way and the grading was void. This tool reads nothing but pixels:

  * SPRITE shift: the displacement that best overlays one frame's opaque 3D
    pixels onto the next frame's (intersection over union of the two masks).
  * GROUND shift: the displacement that best overlays the presented pictures
    with every pixel that is 3D in either frame excluded, so the answer is the
    2D layers' motion alone.

Both are integer displacements (dx, dy) meaning "content at x in frame t is at
x + d in frame t+1". For anything standing still in the world they must be
EQUAL, whatever the camera did; `sprite - ground` is the drift, 0 is correct.

WHAT A LAG LOOKS LIKE HERE. On a perfectly steady pan a one-frame lag is
invisible: the sprites move by the previous step, the ground by the current
one, and the steps are equal. The lag shows where the step CHANGES -- and this
game's camera steps on alternate frames (16, 0, 16, 0), so a lagging layer
reads sprite 0 against ground 16 on every stepping frame. The tool therefore
must not discard 3D pixels that stayed put while the ground moved: those ARE
the lagging sprites.

AMBIGUITY. Terrain tiles repeat every 8 or 16 px, so a ground match can lock
onto the wrong period. Each shift must beat the best DIFFERENT peak (more than
1 px away) by --margin, or the pair is reported as ambiguous and not scored --
the same rule as lb_groundshift.py. A sprite mask smaller than --minmask
pixels is likewise not scored.

Uses numpy. On this machine that means the Windows interpreter,
"C:/Program Files/Python313/python.exe", not the MSYS one first on PATH.
"""

import argparse
import collections
import os
import re
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("REFUSED: needs numpy and Pillow -- run with the Windows Python, "
          "\"C:/Program Files/Python313/python.exe\"")
    sys.exit(2)

PRES = re.compile(r"presented-bot-(\d+)-(\d+)x(\d+)\.(png|ppm)$")
L3D = re.compile(r"layer3d-(\d+)-(\d+)x(\d+)\.(png|ppm)$")
IDX = re.compile(r"^\[lb_dumpidx\] slot (\d+) frame (-?\d+)")
MAGENTA = (255, 0, 255)


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def shifted_views(h, w, dx, dy):
    """Slices (a_rows, a_cols, b_rows, b_cols) overlaying a[x] onto b[x+d]."""
    ay0, ay1 = max(0, -dy), min(h, h - dy)
    ax0, ax1 = max(0, -dx), min(w, w - dx)
    return (slice(ay0, ay1), slice(ax0, ax1),
            slice(ay0 + dy, ay1 + dy), slice(ax0 + dx, ax1 + dx))


def best_with_margin(scores, higher_is_better):
    """Best shift and its gap to the best DIFFERENT peak (> 1 px away)."""
    if len(scores) < 2:
        return None, 0.0
    order = sorted(scores, key=lambda t: t[0], reverse=higher_is_better)
    best = order[0]
    runner = None
    for sc, d in order[1:]:
        if max(abs(d[0] - best[1][0]), abs(d[1] - best[1][1])) > 1:
            runner = (sc, d)
            break
    if runner is None:
        return best[1], float("inf")
    gap = (best[0] - runner[0]) if higher_is_better else (runner[0] - best[0])
    return best[1], gap


def sprite_shift(m0, m1, rx, ry, minmask):
    if m0.sum() < minmask or m1.sum() < minmask:
        return None, 0.0
    h, w = m0.shape
    scores = []
    for dy in range(-ry, ry + 1):
        for dx in range(-rx, rx + 1):
            ar, ac, br, bc = shifted_views(h, w, dx, dy)
            a = m0[ar, ac]
            b = m1[br, bc]
            union = np.count_nonzero(a | b)
            if union == 0:
                continue
            scores.append((np.count_nonzero(a & b) / union, (dx, dy)))
    return best_with_margin(scores, True)


def ground_shift(p0, p1, excl0, excl1, rx, ry):
    h, w = excl0.shape
    scores = []
    for dy in range(-ry, ry + 1):
        for dx in range(-rx, rx + 1):
            ar, ac, br, bc = shifted_views(h, w, dx, dy)
            valid = ~(excl0[ar, ac] | excl1[br, bc])
            n = np.count_nonzero(valid)
            if n < (h * w) // 4:
                continue
            diff = np.abs(p0[ar, ac] - p1[br, bc]).sum(axis=2)
            scores.append((float(diff[valid].mean()), (dx, dy)))
    return best_with_margin(scores, False)


def dilate(m):
    out = m.copy()
    out[1:, :] |= m[:-1, :]
    out[:-1, :] |= m[1:, :]
    out[:, 1:] |= m[:, :-1]
    out[:, :-1] |= m[:, 1:]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--rx", type=int, default=40, help="horizontal search, px")
    ap.add_argument("--ry", type=int, default=8, help="vertical search, px")
    ap.add_argument("--margin", type=float, default=1.5,
                    help="ground: mean-abs-diff gap the best must win by")
    ap.add_argument("--smargin", type=float, default=0.05,
                    help="sprite: IoU gap the best must win by")
    ap.add_argument("--minmask", type=int, default=150)
    ap.add_argument("--show", type=int, default=20,
                    help="print this many scored moving pairs")
    ap.add_argument("--lagscan", action="store_true",
                    help="slide the sprite-shift series against the ground-"
                         "shift series by -2..+2 frames and report which "
                         "offset brings the drift to zero")
    args = ap.parse_args()

    pres, l3d = {}, {}
    for name in os.listdir(args.dir):
        m = PRES.search(name)
        if m:
            pres.setdefault(int(m.group(1)), os.path.join(args.dir, name))
            continue
        m = L3D.search(name)
        if m:
            l3d.setdefault(int(m.group(1)), os.path.join(args.dir, name))
    slot2frame = {}
    log = os.path.join(args.dir, "stderr.log")
    if os.path.exists(log):
        with open(log, "r", encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                m = IDX.match(ln)
                if m:
                    slot2frame[int(m.group(1))] = int(m.group(2))
    slots = sorted(s for s in pres if s in l3d and s in slot2frame)
    print("slots with picture + 3D layer + frame number: %d  "
          "(pictures %d, 3D layers %d, indexed %d)"
          % (len(slots), len(pres), len(l3d), len(slot2frame)))
    if len(slots) < 2:
        print("REFUSED: record with LB_DUMP_SEQ, LB_DUMP_EVERY=1, LB_DUMP3D=1")
        return 2

    tally = collections.Counter()
    rows = []
    rows_all = []
    prev = None
    for s in slots:
        p = load(pres[s])
        l = load(l3d[s])
        if p.shape != l.shape:
            tally["size mismatch"] += 1
            prev = None
            continue
        m = ~((l[:, :, 0] == 255) & (l[:, :, 1] == 0) & (l[:, :, 2] == 255))
        cur = (slot2frame[s], p, m)
        if prev is not None and cur[0] - prev[0] == 1:
            f0, p0, m0 = prev
            f1, p1, m1 = cur
            g, gg = ground_shift(p0, p1, dilate(m0), dilate(m1),
                                 args.rx, args.ry)
            # A STILL GROUND IS SCORED TOO. Under a one-frame lag the frames
            # where the ground stood still are exactly where the sprites
            # catch up -- sprite -16 against ground 0 -- and skipping them
            # hid half the defect in the first version of this tool. Pairs
            # where NEITHER layer moved are counted but not listed: they
            # satisfy the invariant trivially.
            if g is None or gg < args.margin:
                tally["ground ambiguous"] += 1
            else:
                sp, sg = sprite_shift(m0, m1, args.rx, args.ry, args.minmask)
                if sp is None:
                    tally["too little 3D"] += 1
                elif sg < args.smargin:
                    tally["sprite ambiguous"] += 1
                else:
                    rec = (f1, g, sp, (sp[0] - g[0], sp[1] - g[1]), gg, sg)
                    rows_all.append(rec)   # the lag scan needs still pairs
                    if g == (0, 0) and sp == (0, 0):
                        tally["both still"] += 1
                    else:
                        tally["scored"] += 1
                        rows.append(rec)
        elif prev is not None:
            tally["not adjacent"] += 1
        prev = cur

    print("pairs:", ", ".join("%s %d" % kv for kv in sorted(tally.items())))

    if args.lagscan:
        # WHICH LAYER IS EARLY, AND BY HOW MUCH -- measured, not argued.
        # Every unambiguous pair contributes its ground and sprite shift,
        # keyed by the later frame. drift_k(t) = sprite(t + k) - ground(t).
        # k = 0 is the picture as shown. If k = +1 zeroes it, the sprites
        # move one frame AFTER the ground (they lag); k = -1, they lead.
        # A hand-derived timing model got this sign wrong twice.
        gs, ss = {}, {}
        for f1, g, sp, d, gg, sg in rows_all:
            gs[f1] = g[0]
            ss[f1] = sp[0]
        print()
        print("LAG SCAN  drift_k(t) = sprite(t+k) - ground(t), frames where "
              "either moved:")
        for k in (-2, -1, 0, 1, 2):
            ds = [ss[t + k] - gs[t] for t in gs
                  if (t + k) in ss and (gs[t] != 0 or ss[t + k] != 0)]
            if not ds:
                print("   k=%+d  no overlap" % k)
                continue
            a = sorted(abs(v) for v in ds)
            print("   k=%+d  n=%3d  exactly 0: %5.1f%%  within 1 px: %5.1f%%  "
                  "median %2d  p90 %2d"
                  % (k, len(a), 100.0 * a.count(0) / len(a),
                     100.0 * sum(1 for v in a if v <= 1) / len(a),
                     a[len(a) // 2], a[int(len(a) * 0.9)]))
    if not rows:
        print()
        print("REFUSED: no moving pair had both an unambiguous ground shift "
              "and an unambiguous sprite shift.")
        return 2

    rx = [abs(r[3][0]) for r in rows]
    ry = [abs(r[3][1]) for r in rows]
    srt = sorted(rx)
    n = len(rows)
    print()
    print("DRIFT, sprite shift minus ground shift, on %d pairs where either layer moved:" % n)
    print("   x: exactly 0 on %d (%.1f%%)   within 1 px %.1f%%   "
          "median %d   p90 %d   max %d"
          % (rx.count(0), 100.0 * rx.count(0) / n,
             100.0 * sum(1 for v in rx if v <= 1) / n,
             srt[n // 2], srt[int(n * 0.9)], srt[-1]))
    print("   y: exactly 0 on %d (%.1f%%)   max %d"
          % (ry.count(0), 100.0 * ry.count(0) / n, max(ry)))
    hist = collections.Counter(r[3][0] for r in rows)
    print("   x drift histogram:",
          "  ".join("%+d:%d" % kv for kv in sorted(hist.items())))
    print()
    print(" frame   ground     sprite     drift    g-margin  s-margin")
    for f1, g, sp, d, gg, sg in rows[:args.show]:
        print(" %5d  %+4d,%+3d  %+4d,%+3d  %+4d,%+3d   %6.2f   %6.3f"
              % (f1, g[0], g[1], sp[0], sp[1], d[0], d[1],
                 gg if gg != float("inf") else 99.0, sg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
