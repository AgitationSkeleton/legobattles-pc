#!/usr/bin/env python3
"""Measure how far sprites drift against the ground while the camera moves.

    python tools/lb_spriteworld.py screenshots/exp-<label>

THIS IS THE GATE FOR THE JITTER WORK, and it replaces tools/lb_groundshift.py
for grading a fix that moves the 3D layer. lb_groundshift compares ground read
from PIXELS against geometry read from the POLYGON LIST; a correction applied
at composite time moves pixels without touching the polygon list, so that tool
is blind to it by construction. LB_LAGFIX and LB_RESHIFT were both graded with
tools that could not see them, and both were withdrawn only after the fact.

WHAT IT MEASURES. For an object that is not walking, its WORLD position is
fixed, so its screen position must change by exactly minus the camera's
movement:

    screen(t+1) - screen(t)  ==  -(cam(t+1) - cam(t))

The residual `dscreen + dcam` is therefore the sprite's positional error in
pixels, and zero is correct. This is the invariant lb_widemap_cam_px() was
added for, stated in its own comment; this tool is that comment carried out.

Only frames where the camera actually moved are counted -- a still frame
satisfies the invariant trivially and would dilute the result towards zero.
That matters: in a 6,439-frame `--pan` recording only 388 frames have camera
motion, so a figure computed over all frames is 94% padding.

MATCHING, and why the obvious scheme overstates the drift. Polygons are
grouped by (texture, palette, vertex count). Grouping alone is not enough:
grass and trees share all three, so nearest-centre matching pairs a polygon
with a DIFFERENT instance of the same art one tile over and the mismatch enters
the statistic as drift. That inflated the first figures from this tool from a
median of 2 px to 4 px. Candidates must therefore lie within --gate px of where
the camera predicts the object moved to, and ambiguous ones are refused.

The bias that leaves is deliberate and has to be stated: an object that
genuinely drifted further than the gate is DISCARDED rather than counted, so
the output is a floor on the drift, not a ceiling.

Geometry with z < 5,000,000 is excluded: that is the in-field HUD, which is
screen-locked and for which the invariant is the opposite one.

GRADING A CORRECTION. Pass --rsdx to subtract the per-frame correction the
runner reports in its polylist header, and the tool prints the corrected series
beside the uncorrected one. A correction that helps lowers the median and the
p90 and raises `within 2px`. LB_RESHIFT raised all three and was withdrawn.
"""

import argparse
import collections
import os
import re
import statistics
import sys

# NAMED GROUPS, NOT POSITIONAL, and that is not style either. This header has
# grown a field at a time -- rsdx, then hofs/vofs, then prom -- and every time
# it grew, every positional index after the insertion point silently shifted.
# That has produced three published-then-retracted numbers in two days: a swap
# rate of 0.001/frame that was really `vofs`, and a camera pick age of 40 that
# was really the submitted polygon count (choose_camera picks from a history of
# FOUR, so it cannot exceed 3 -- the impossible value is what exposed it).
# A name cannot shift.
HDR = re.compile(
    r"\[lb_polylist\] frame (?P<f>-?\d+) .*? cam (?P<cam>-?\d+),(?P<camy>-?\d+) "
    r"rcam (?P<rcam>-?\d+),(?P<rcamy>-?\d+) "
    r"chosen (?P<gnd>-?\d+),(?P<gndy>-?\d+).*?"
    r"campick (?P<age>\d+) subm (?P<subm>\d+) rsdx (?P<rs>-?\d+)")
POLY = re.compile(
    r"\[lb_polylist\]\s+\d+ sub\s+\d+ v(\d+)\s+surf x\s+(-?\d+)\.\.(-?\d+)\s+"
    r"y\s+(-?\d+)\.\.(-?\d+)\s+guest x\s+(-?\d+)\.\.(-?\d+)\s+"
    r"y\s+(-?\d+)\.\.(-?\d+)\s+z (\d+)\.\.(\d+)\s+tex ([0-9A-F]+) "
    r"pal ([0-9A-F]+)")

HUD_Z = 5000000


def parse(path):
    frames = []
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = HDR.search(ln)
            if m:
                cur = {"f": int(m.group("f")), "cam": int(m.group("cam")),
                       "rcam": int(m.group("rcam")),
                       "gnd": int(m.group("gnd")),
                       "rs": int(m.group("rs")), "p": []}
                frames.append(cur)
                continue
            m = POLY.search(ln)
            if m is None or cur is None:
                continue
            if int(m.group(10)) < HUD_Z:
                continue
            cur["p"].append({
                "gx": (int(m.group(6)) + int(m.group(7))) / 2.0,
                "gy": (int(m.group(8)) + int(m.group(9))) / 2.0,
                "w": int(m.group(7)) - int(m.group(6)),
                "k": (m.group(12), m.group(13), int(m.group(1))),
            })
    return [f for f in frames if f["p"]]


def residuals(frames, use_rsdx, ref="gnd", strict=True, gate=24):
    """Residuals in px, plus how many matches were refused as ambiguous.

    WHICH CAMERA TO GRADE AGAINST -- this decides what the number MEANS, and
    getting it wrong rigged an entire verdict (docs/340).

      "gnd"  the camera the TERRAIN is drawn with. This is the default and it
             is what the player perceives: a sprite is judged against the
             ground it stands on.
      "rcam" the camera the GEOMETRY was transformed with. Against this, a
             correction that moves sprites toward the ground necessarily
             scores WORSE, because rcam is the frozen pipeline value the
             sprites already agree with. LB_RESHIFT was graded this way and
             the test was rigged by construction.
      "cam"  the live pixel camera; reproduces a known artefact, see below.

    PAIR THE LIST WITH ITS OWN CAMERA. lb_band_render_cam() is documented as
    "the camera the geometry now queued for rendering was transformed against
    -- pair this with the render polygon list, never the live camera", and
    warns that pairing the live one manufactures a swing "shaped exactly like
    the shear it would be used to detect". Measured here, the live camera adds
    about 1.5 px to the p90. --livecam reproduces that for comparison.

    GATE ON THE PREDICTED POSITION. Grass and trees repeat, so nearest-centre
    matching on (tex, pal, vcount) alone will happily pair a polygon with a
    DIFFERENT instance of the same art one tile over, and that mismatch enters
    the statistic as drift. Candidates are therefore required to lie within
    `gate` px of where the camera says the object should have moved to.

    This is deliberately CONSERVATIVE and the bias must be stated: an object
    that genuinely drifted further than `gate` is discarded rather than
    counted, so the figures are a floor on the drift, not a ceiling. Raising
    --gate widens the admitted range at the cost of readmitting mismatches.
    """
    out = []
    ambiguous = 0
    key = ref
    for a, b in zip(frames, frames[1:]):
        if b["f"] - a["f"] != 1:
            continue
        dcam = b[key] - a[key]
        if dcam == 0:
            continue
        drs = (b["rs"] - a["rs"]) if use_rsdx else 0
        by_key = collections.defaultdict(list)
        for p in a["p"]:
            by_key[p["k"]].append(p)
        for p in b["p"]:
            cands = by_key.get(p["k"])
            if not cands:
                continue
            near = [c for c in cands
                    if abs(c["gy"] - p["gy"]) <= 64
                    and abs(c["gx"] - dcam - p["gx"]) <= gate]
            if not near:
                continue
            if strict and len(near) > 1:
                ambiguous += 1
                continue
            q = min(near, key=lambda c: (c["gx"] - dcam - p["gx"]) ** 2 +
                    (c["gy"] - p["gy"]) ** 2)
            if strict and q["w"] != p["w"]:
                continue
            out.append(abs((p["gx"] - q["gx"] - drs) + dcam))
    return out, ambiguous


def report(name, res):
    v, amb = res
    if not v:
        print("  %-12s REFUSED: no matched polygons on moving frames" % name)
        return
    s = sorted(v)
    print("  %-12s n=%d  median %.1f px  mean %.2f  p90 %.1f  max %.0f  "
          "within 2px %.1f%%  (ambiguous, refused: %d)"
          % (name, len(s), s[len(s) // 2], statistics.fmean(s),
             s[int(len(s) * 0.9)], s[-1],
             100.0 * sum(1 for x in s if x <= 2) / len(s), amb))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--ref", choices=("gnd", "rcam", "cam"), default="gnd",
                    help="camera to grade against: gnd (the terrain's, the "
                         "default and what the player sees), rcam (the "
                         "geometry pipeline's -- sprites trivially agree with "
                         "it, see docs/340), cam (live, a known artefact)")
    ap.add_argument("--loose", action="store_true",
                    help="accept ambiguous and differently-sized matches")
    ap.add_argument("--gate", type=int, default=24,
                    help="px from the predicted position a candidate may lie")
    ap.add_argument("--rsdx", action="store_true",
                    help="also report the series with the runner's rsdx "
                         "correction applied, to grade that correction")
    args = ap.parse_args()

    log = args.dir
    if os.path.isdir(log):
        log = os.path.join(log, "stderr.log")
    if not os.path.exists(log):
        print("REFUSED: no %s" % log)
        return 2

    frames = parse(log)
    print("frames carrying world geometry: %d" % len(frames))
    strict = not args.loose
    unc = residuals(frames, False, args.ref, strict, args.gate)
    if not unc[0]:
        print()
        print("REFUSED: no frame pair had both camera motion and a matched "
              "polygon. Record with --pan --pan-return and a short --pan-hold: "
              "a pan that reaches the map edge stops moving and measures "
              "nothing.")
        return 2
    print()
    print("Residual sprite-vs-ground error, frames where the %s camera MOVED"
          % args.ref)
    print("(0 px = the sprite held its world position, as a stationary "
          "object must)")
    report("uncorrected", unc)
    if args.rsdx:
        report("reshifted",
               residuals(frames, True, args.ref, strict, args.gate))
    return 0


if __name__ == "__main__":
    sys.exit(main())
