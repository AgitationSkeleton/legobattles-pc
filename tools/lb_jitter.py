#!/usr/bin/env python3
"""Measure in-motion jitter from the polygon list alone -- no camera, no pixels.

    python tools/lb_jitter.py screenshots/exp-<label>/stderr.log

THE REPORT: *"while in motion sprites become jittery and disseheveled"*
(2026-09-13). Two attempts to measure it are retracted and the reasons are the
whole design of this tool:

  * `chosen - rcam` (commit 37ad4aa3) said the 2D and the 3D are drawn from
    cameras up to 136 px apart in 86% of moving frames. RETRACTED: `rcam` is a
    VBLANK camera sampled inside the vertex transform, not the camera the game
    used for those vertices. If the game advances its camera and then submits
    geometry, the proxy is stale by construction and the difference says
    nothing about the picture. **So this tool uses no camera at all.**

  * terrain-vs-building, by cross-correlating BG1 between consecutive frames,
    found 41 of 376 pairs where a stationary building did not track the ground,
    by up to 17 px. Its own stated weak link: the ground is repetitive grass
    and a correlation mis-peak looks exactly like the finding. **So this tool
    uses no pixels at all.**

WHAT IT MEASURES INSTEAD. Within ONE frame's polygon list, every piece of
stationary world geometry must move between frames by the SAME amount --
whatever the camera did, whatever the band did, whatever the ground looks like.
A rigid picture has one shift. Jitter is polygons disagreeing about it.

Units legitimately walk, so a minority of polygons SHOULD deviate by a pixel or
two. The statistic is therefore the mode of the per-polygon shift and the
distribution of deviations from it, not the mean.

MATCHING WITHOUT AN IDENTITY. The dump has no stable object id --
`submission_index` is a per-frame ordinal (docs/212). Polygons are grouped by
(tex_param, tex_palette, vertex_count) and matched within a group by NEAREST
CENTRE, greedily, closest pair first.

The first version demanded that the tuple occur exactly once in both frames so
a match could never be a guess. On a real frame that matched **32 polygons out
of 13,288 pairs** and the tool refused, correctly: a battlefield draws many
objects with one texture, so almost no fingerprint is unique. Nearest-centre
matching keeps the guarantee that matters -- a polygon is only ever matched to
one of the same texture, palette and vertex count -- and `--max-jump` bounds
how far a match may move, so a genuinely new object cannot be paired with a
departed one.

Unmatched polygons are counted and reported: a run where most polygons cannot
be matched is not evidence of anything and says so.
"""

import argparse
import collections
import re
import sys

HDR = re.compile(r"^\[lb_polylist\] frame (-?\d+) surface (\d+)x(\d+) "
                 r"extra -?\d+,-?\d+ cam (-?\d+),(-?\d+) .*?"
                 r"chosen (-?\d+),(-?\d+)")
POLY = re.compile(
    r"^\[lb_polylist\]\s+(\d+) sub\s+(\d+) v(\d+)\s+surf x\s*(-?\d+)\.\.(-?\d+)\s+"
    r"y\s*(-?\d+)\.\.(-?\d+)\s+guest x\s*-?\d+\.\.-?\d+\s+y\s*-?\d+\.\.-?\d+\s+"
    r"z (\d+)\.\.(\d+)\s+tex ([0-9A-Fa-f]+) pal ([0-9A-Fa-f]+)")


def parse(path, min_z):
    """[(frame, {fingerprint: (cx, cy)})], fingerprints seen more than once
    within a frame are dropped rather than disambiguated."""
    frames, cur, key = [], None, None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = HDR.match(line)
            if m:
                if cur is not None:
                    frames.append((key, cur))
                # cam is in PIXELS; chosen is ctx*8 and therefore a
                # staircase. Which one is used decides whether a +/-2 px
                # residual is a defect or quantisation, so it is a flag.
                key = (int(m.group(1)), int(m.group(4)), int(m.group(6)))
                cur = collections.defaultdict(list)
                continue
            if cur is None:
                continue
            m = POLY.match(line)
            if m:
                (_i, _sub, vc, x0, x1, y0, y1, z0, _z1, tex, pal) = m.groups()
                # THE HUD IS NOT WORLD GEOMETRY AND MUST NOT BE IN THIS TEST.
                # docs/212 measured the in-field HUD at z 4,259,328 and
                # 4,267,520 and world geometry at 6,987,264..7,380,480; the
                # health bars sit at 4,718,080 and are anchored to units that
                # walk. A screen-locked quad does not move when the camera
                # pans, so including it would report the HUD's stillness as
                # the world's jitter -- which is exactly how lb_drift3d
                # produced a 92.5% tracking residual that was entirely HUD.
                if int(z0) < min_z:
                    continue
                fp = (tex, pal, vc)
                cur[fp].append(((int(x0) + int(x1)) / 2.0,
                                (int(y0) + int(y1)) / 2.0))
    if cur is not None:
        frames.append((key, cur))
    return [(fnum, dict(d)) for fnum, d in frames]


def match(d0, d1, max_jump):
    """Greedy nearest-centre pairing inside each (tex, pal, vcount) group.

    Closest pair first, each polygon used once, and never across groups -- so a
    match is always same-texture, same-palette, same vertex count. Returns the
    x-shifts of the matched pairs.
    """
    shifts, matched = [], 0
    for fp in set(d0) & set(d1):
        a, b = d0[fp], d1[fp]
        cand = []
        for i, (ax, ay) in enumerate(a):
            for j, (bx, by) in enumerate(b):
                d2 = (bx - ax) ** 2 + (by - ay) ** 2
                if d2 <= max_jump * max_jump:
                    cand.append((d2, i, j, bx - ax))
        cand.sort()
        used_a, used_b = set(), set()
        for _d2, i, j, dx in cand:
            if i in used_a or j in used_b:
                continue
            used_a.add(i)
            used_b.add(j)
            shifts.append(dx)
            matched += 1
    return shifts, matched


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--tol", type=float, default=1.0,
                    help="deviation from the mode that still counts as rigid")
    ap.add_argument("--max-jump", type=float, default=64.0,
                    help="a match may not move further than this in one "
                         "frame; bounds pairing a new object with a gone one")
    ap.add_argument("--cam", choices=("cam", "chosen"), default="cam",
                    help="'cam' is lb_widemap_cam_px, in pixels; 'chosen' is "
                         "ctx*8 and quantised to a tile, which manufactures a "
                         "+/-2 px residual on a 2 px/frame pan")
    ap.add_argument("--min-z", type=float, default=5000000.0,
                    help="polygons below this z are HUD or health bars, not "
                         "stationary world geometry (docs/212)")
    ap.add_argument("--min-matched", type=int, default=3,
                    help="frame pairs with fewer matches are not evidence")
    args = ap.parse_args()

    frames = parse(args.log, args.min_z)
    if len(frames) < 2:
        print("REFUSED: %d frames parsed from %s -- run with LB_POLYLIST=1"
              % (len(frames), args.log))
        return 2

    still = moving = thin = 0
    dev_hist = collections.Counter()
    ground_hist = collections.Counter()
    worst = []
    matched_total = unmatched_total = 0

    ci = 1 if args.cam == "cam" else 2
    for ((f0, cam0, ch0), d0), ((f1, cam1, ch1), d1) in zip(frames,
                                                            frames[1:]):
        c0 = cam0 if ci == 1 else ch0
        c1 = cam1 if ci == 1 else ch1
        raw, nmatch = match(d0, d1, args.max_jump)
        n1 = sum(len(v) for v in d1.values())
        matched_total += nmatch
        unmatched_total += max(0, n1 - nmatch)
        if nmatch < args.min_matched:
            thin += 1
            continue
        shifts = [round(x) for x in raw]
        mode = collections.Counter(shifts).most_common(1)[0][0]
        if mode == 0:
            still += 1
            # A still frame cannot show in-motion jitter; excluded so the
            # denominator is "pairs where the view moved", which is what the
            # report is about.
            continue
        moving += 1
        # THE GROUND, against the same frame's geometry. `chosen` is the camera
        # the 2D layers are drawn from, and choose_camera picks it by measuring
        # candidate cameras against the guest's OWN tilemap -- so it is the one
        # the terrain actually matches. This is NOT `rcam`, the vblank proxy
        # whose use was retracted; no pixel correlation is involved either.
        #
        # A camera moving right by N puts the world N to the left, so a rigid
        # picture has mode == -(chosen1 - chosen0). Anything else is the ground
        # and the sprites disagreeing, which is what "sprites become jittery"
        # would look like from the player's seat.
        ground_hist[mode + (c1 - c0)] += 1
        for s in shifts:
            dev = s - mode
            dev_hist[dev] += 1
            if abs(dev) > args.tol:
                worst.append((f1, dev, mode))

    print("frames parsed          %d" % len(frames))
    print("polygons matched       %d  (unmatched %d)"
          % (matched_total, unmatched_total))
    print("pairs: moving %d   still %d   too-thin %d"
          % (moving, still, thin))
    if moving == 0:
        print()
        print("REFUSED: no frame pair showed the view moving. A jitter figure "
              "from a static camera measures nothing.")
        return 2

    total = sum(dev_hist.values())
    off = sum(c for d, c in dev_hist.items() if abs(d) > args.tol)
    print()
    print("deviation from the frame's own modal shift, moving pairs only:")
    for dev in sorted(dev_hist):
        bar = "#" * min(60, dev_hist[dev] * 60 // max(1, total))
        print("  %+4d px  %6d  %s" % (dev, dev_hist[dev], bar))
    gtot = sum(ground_hist.values())
    if gtot:
        gok = sum(c for d, c in ground_hist.items() if abs(d) <= args.tol)
        print()
        print("ground vs geometry: mode + d(%s), moving pairs:" % args.cam)
        for dev in sorted(ground_hist):
            bar = "#" * min(60, ground_hist[dev] * 60 // max(1, gtot))
            print("  %+4d px  %6d  %s" % (dev, ground_hist[dev], bar))
        print("  TRACKING: %d of %d within +/-%g px (%.2f%%)"
              % (gok, gtot, args.tol, 100.0 * gok / gtot))

    print()
    print("RIGID: %d of %d polygon-shifts within +/-%g px of the mode (%.2f%%)"
          % (total - off, total, args.tol, 100.0 * (total - off) / total))
    print("JITTER: %d shifts deviate (%.2f%%)" % (off, 100.0 * off / total))
    if worst:
        print()
        print("largest deviations:")
        for fnum, dev, mode in sorted(worst, key=lambda w: -abs(w[1]))[:10]:
            print("  frame %5d  shift %+d where the frame's mode was %+d"
                  % (fnum, dev + mode, mode))
    return 0


if __name__ == "__main__":
    sys.exit(main())
