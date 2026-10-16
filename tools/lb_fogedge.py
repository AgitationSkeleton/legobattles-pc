#!/usr/bin/env python3
"""Is the fog/terrain frontier in the widened bands as organic as the guest's?

THE MEASUREMENT CARRIES ITS OWN CONTROL, which is the whole reason it is worth
having. The cartridge's own 256 columns sit inside the same composited surface
as the bands, drawn by the same renderer in the same frame, so the guest region
is a known-good sample of what this game's fog edge is supposed to look like.
Any band figure is read against it, never against an absolute.

docs/270 made this comparison by hand to fix a flat-edged shroud, and it was
made by hand a second time afterwards. Twice is enough; it lives here now.

WHAT IT MEASURES. LB_LAYERDUMP writes a greyscale map per pixel of which layer
won, as (order + 1) * 40 -- BG1 terrain is 120, BG2 fog is 160. For each row the
boundary column between them is located, and two things are reported:

  mean run   how many consecutive rows share the same boundary column. A
             diagonal, organic coastline steps often and scores LOW. A flat
             horizontal edge holds one column for many rows and scores HIGH.
             docs/270's defect read 28.81 against the cartridge's 2.04.

  steps      the histogram of |column change| between adjacent rows. The
             cartridge's signature is 1-2 px steps dominating.

USAGE
    python tools/lb_fogedge.py <run-directory> [surface_width]

The run needs LB_LAYERDUMP set, e.g.

    LB_LAYERDUMP=90 python tools/lb_exp.py myrun --pan --window-size 1600x800

A REGION WITH NO BOUNDARY REPORTS SO. A band that is entirely fog, or entirely
terrain, has no frontier to measure and says "no boundary" rather than scoring
a number from too few rows -- that distinction is what stops an empty region
looking like a clean one.
"""
import glob
import os
import re
import sys

BG1, BG2 = 120, 160          # (order+1)*40 for terrain and fog
MIN_ROWS = 8                 # below this a mean is not worth printing


def read_pgm(path):
    d = open(path, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while d[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i:i + w * h]


def edge_stats(px, w, h, x0, x1):
    """Boundary column per row within [x0, x1), and how it moves."""
    seq = []
    for y in range(h):
        row = px[y * w + x0:y * w + x1]
        for k in range(len(row) - 1):
            a, b = row[k], row[k + 1]
            if (a == BG1 and b == BG2) or (a == BG2 and b == BG1):
                seq.append(x0 + k)
                break
    if len(seq) < MIN_ROWS:
        return None
    runs, cur = [], 1
    for i in range(1, len(seq)):
        if seq[i] == seq[i - 1]:
            cur += 1
        else:
            runs.append(cur)
            cur = 1
    runs.append(cur)
    steps = {}
    for i in range(1, len(seq)):
        dd = abs(seq[i] - seq[i - 1])
        steps[dd] = steps.get(dd, 0) + 1
    return (len(seq), sum(runs) / len(runs), max(runs),
            dict(sorted(steps.items())[:6]))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    run = sys.argv[1]
    want_w = int(sys.argv[2]) if len(sys.argv) > 2 else None
    pat = os.path.join(run, "lb_layers-*.pgm")
    files = sorted(glob.glob(pat),
                   key=lambda p: int(re.search(r"-(\d+)\.pgm", p).group(1)))
    if not files:
        print("no lb_layers-*.pgm in %s -- the run needs LB_LAYERDUMP set"
              % run)
        return 1
    # One surface size, so the band arithmetic is a single number.
    sizes = {}
    for p in files:
        m = re.search(r"lb_layers-(\d+)x(\d+)-", p)
        if m:
            sizes.setdefault((int(m.group(1)), int(m.group(2))), []).append(p)
    if want_w:
        sizes = {k: v for k, v in sizes.items() if k[0] == want_w}
    if not sizes:
        print("no dumps at that width")
        return 1
    (w0, h0), chosen = max(sizes.items(), key=lambda kv: len(kv[1]))
    band = (w0 - 256) // 2
    print("run %s" % run)
    print("surface %dx%d, %d dump(s); guest columns assumed at x %d..%d"
          % (w0, h0, len(chosen), band, band + 256))
    print("mean run: LOW is organic, HIGH is a flat edge. Read the bands "
          "against CARTRIDGE.\n")
    regions = (("CARTRIDGE (guest)", band, band + 256),
               ("LEFT band", 0, band),
               ("RIGHT band", band + 256, w0))
    for p in chosen:
        w, h, px = read_pgm(p)
        if (w, h) != (w0, h0):
            continue
        print("frame %s" % re.search(r"-(\d+)\.pgm", p).group(1))
        for name, a, b in regions:
            if b <= a:
                continue
            r = edge_stats(px, w, h, a, b)
            if r:
                print("   %-18s rows=%3d  mean run %5.2f  longest %3d  "
                      "steps %s" % (name, r[0], r[1], r[2], r[3]))
            else:
                print("   %-18s no boundary here (all fog or all terrain)"
                      % name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
