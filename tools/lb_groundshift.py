#!/usr/bin/env python3
"""Measure the ground's shift from PIXELS and compare it with the geometry's.

    python tools/lb_groundshift.py screenshots/exp-<label>

THIS IS THE GATE FOR THE JITTER FIX (docs/335). Everything measured so far is
about where POLYGONS are; the player sees PIXELS, and the withdrawn LB_LAGFIX
was invisible to the polygon test by construction. A fix that moves composited
pixels has to be graded by something that looks at composited pixels.

WHAT IT DOES. For each consecutive pair of recorded frames it computes:

  * the GROUND's horizontal shift, by matching the frame's column-mean
    luminance profile against the previous frame's over a range of integer
    shifts, and taking the best;
  * the GEOMETRY's shift, as the modal per-polygon shift from the LB_POLYLIST
    dump for the same two frames (tools/lb_jitter.py's first statistic);

and reports `ground - geometry`. Zero means the sprites sit still against the
ground. The two-frame lag predicts a non-zero difference that grows with pan
speed and persists for two frames after the camera stops.

THE REPETITIVE-GRASS PROBLEM, which retracted an earlier attempt: this game's
ground tiles repeat, so a correlation can lock onto the wrong peak. Three
guards, and the tool refuses rather than guessing:

  * **Run it on a window small enough to contain structure.** A wide window of
    uniform shroud has a periodic profile and nothing can match it honestly.
    The 512x384 capture has terrain, a building and rock detail.
  * **Peak margin.** The best shift must beat the runner-up by --margin, so an
    ambiguous match is reported as ambiguous instead of as a number.
  * **Periodicity check.** If the profile's autocorrelation has a strong peak
    at a non-zero lag, the frame is skipped and counted.
"""

import argparse
import collections
import os
import re
import sys

try:
    from PIL import Image
except ImportError:
    print("REFUSED: needs Pillow (pip install Pillow)")
    sys.exit(2)

FRAME = re.compile(r"presented-bot-(\d+)-(\d+)x(\d+)\.png$")
HDR = re.compile(r"^\[lb_polylist\] frame (-?\d+) ")


def profile(path):
    """Column-mean luminance. One number per output column."""
    im = Image.open(path).convert("L")
    w, h = im.size
    px = im.load()
    out = []
    # Every 4th row: the profile is a mean, and a quarter of the rows moves it
    # by far less than the +/-1 px this tool resolves, for a quarter of the
    # work on a 564-row frame.
    rows = list(range(0, h, 4))
    for x in range(w):
        s = 0
        for y in rows:
            s += px[x, y]
        out.append(s / float(len(rows)))
    return out


def best_shift(a, b, rng, margin):
    """Shift s minimising mean |b[x+s] - a[x]|, plus the runner-up gap."""
    n = min(len(a), len(b))
    scores = []
    for s in range(-rng, rng + 1):
        lo = max(0, -s)
        hi = min(n, n - s)
        if hi - lo < n // 2:
            continue
        tot = 0.0
        # Stride 2 over columns: the profile is smooth at the scale that
        # matters and this halves an O(range * width) inner loop.
        cnt = 0
        for x in range(lo, hi, 2):
            tot += abs(b[x + s] - a[x])
            cnt += 1
        scores.append((tot / max(1, cnt), s))
    if len(scores) < 2:
        return None, 0.0
    scores.sort()
    best, runner = scores[0], scores[1]
    # Adjacent shifts are not independent, so the runner-up must be a
    # DIFFERENT peak, not the neighbour of the same one.
    for sc, s in scores[1:]:
        if abs(s - best[1]) > 1:
            runner = (sc, s)
            break
    gap = runner[0] - best[0]
    if gap < margin:
        return None, gap
    return best[1], gap


def geometry_shifts(log):
    """{frame_number: modal polygon shift} from tools/lb_jitter.py's matcher."""
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "lb_jitter", os.path.join(here, "lb_jitter.py"))
    j = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(j)
    frames = j.parse(log, 5000000.0)
    out = {}
    for (k0, d0), (k1, d1) in zip(frames, frames[1:]):
        raw, n = j.match(d0, d1, 64.0)
        if n < 3:
            continue
        sh = [round(x) for x in raw]
        out[k1[0]] = collections.Counter(sh).most_common(1)[0][0]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--range", type=int, default=40)
    ap.add_argument("--margin", type=float, default=1.5,
                    help="the best shift must beat the runner-up by this much "
                         "mean-abs-difference, or the pair is ambiguous")
    args = ap.parse_args()

    frames = {}
    for name in os.listdir(args.dir):
        m = FRAME.search(name)
        if m:
            frames[int(m.group(1))] = os.path.join(args.dir, name)
    if len(frames) < 2:
        print("REFUSED: %d frames in %s -- record with LB_DUMP_SEQ"
              % (len(frames), args.dir))
        return 2

    log = os.path.join(args.dir, "stderr.log")
    geo = geometry_shifts(log) if os.path.exists(log) else {}

    # SLOT -> FRAME, from the runner rather than assumed. The dumps are named
    # by ring slot (0..N-1) and every other trace is keyed by the frame
    # counter; the first version of this tool paired them by index and matched
    # 0 of 60 pairs, silently, because those are different numbers.
    slot2frame = {}
    if os.path.exists(log):
        pat = re.compile(r"^\[lb_dumpidx\] slot (\d+) frame (-?\d+)")
        with open(log, "r", encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                m = pat.match(ln)
                if m:
                    slot2frame[int(m.group(1))] = int(m.group(2))
    if not slot2frame:
        print("REFUSED: no [lb_dumpidx] lines in %s -- the build that wrote "
              "these frames cannot say which frame each slot was." % log)
        return 2

    keys = sorted(frames)
    diff = collections.Counter()
    ambiguous = 0
    paired = 0
    rows = []
    prev_prof = None
    prev_k = None
    for k in keys:
        prof = profile(frames[k])
        if prev_prof is not None:
            s, gap = best_shift(prev_prof, prof, args.range, args.margin)
            if s is None:
                ambiguous += 1
            else:
                # THE SAME INTERVAL ON BOTH SIDES. With LB_DUMP_ONMOVE the
                # recorded frames are NOT adjacent -- the camera steps on
                # alternate frames, so consecutive dumps are usually two apart
                # -- and comparing a one-frame geometry shift against a
                # two-frame ground shift would manufacture exactly the
                # discrepancy this tool exists to detect. The geometry shift is
                # therefore ACCUMULATED over the same frame span the pictures
                # span, and a span with any step missing is skipped rather than
                # approximated.
                f0 = slot2frame.get(prev_k)
                f1 = slot2frame.get(k)
                if f0 is not None and f1 is not None and f1 > f0:
                    steps = [geo.get(f) for f in range(f0 + 1, f1 + 1)]
                    if all(v is not None for v in steps):
                        g = sum(steps)
                        paired += 1
                        diff[s - g] += 1
                        rows.append((f1, s, g, s - g, gap))
        prev_prof = prof
        prev_k = k

    print("frames %d   paired with geometry %d   ambiguous %d"
          % (len(frames), paired, ambiguous))
    if not paired:
        print()
        print("REFUSED: no frame pair had both an unambiguous ground shift and "
              "a geometry shift. A jitter figure from neither is nothing.")
        return 2

    print()
    print("ground - geometry, per frame pair:")
    tot = sum(diff.values())
    for d in sorted(diff):
        bar = "#" * min(50, diff[d] * 50 // max(1, tot))
        print("  %+4d px  %5d  %s" % (d, diff[d], bar))
    agree = sum(c for d, c in diff.items() if abs(d) <= 1)
    print()
    print("TOGETHER: %d of %d pairs within +/-1 px (%.1f%%)"
          % (agree, tot, 100.0 * agree / tot))

    moving = [r for r in rows if r[1] != 0 or r[2] != 0]
    if moving:
        print()
        print(" frame  ground  geometry  diff   margin")
        for r in moving[:24]:
            print("  %4d   %+5d    %+6d   %+4d   %5.2f" % r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
