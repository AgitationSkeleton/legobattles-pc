"""Backdrop / character separation for a card, automatically.

Region-grow from the border over a lightly blurred copy: a pixel joins the backdrop
when it is close to an already-backdrop neighbour (local step) and its hue/sat/value
stay inside the backdrop's own range measured on the card's border ring."""
import os, sys
from collections import deque
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cardlib


def hsv(rgb):
    a = rgb.astype(float) / 255.0
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    mx = a.max(-1); mn = a.min(-1); d = mx - mn
    h = np.zeros_like(mx)
    m = d > 1e-6
    rr = m & (mx == r); gg = m & (mx == g) & ~rr; bb = m & ~rr & ~gg
    h[rr] = ((g - b)[rr] / d[rr]) % 6
    h[gg] = ((b - r)[gg] / d[gg]) + 2
    h[bb] = ((r - g)[bb] / d[bb]) + 4
    h = h * 60.0
    s = np.where(mx > 0, d / np.maximum(mx, 1e-6), 0)
    return h, s, mx


def blur(rgb):
    a = rgb.astype(float)
    p = np.pad(a, ((1, 1), (1, 1), (0, 0)), mode="edge")
    o = np.zeros_like(a)
    for dy in range(3):
        for dx in range(3):
            o += p[dy:dy + 96, dx:dx + 96]
    return o / 9.0


def hue_dist(a, b):
    d = np.abs(a - b) % 360
    return np.minimum(d, 360 - d)


def backdrop_mask(c, step=34.0, hue_tol=None, seed_rows=None, mh_override=None):
    rgb = c["rgb"]; op = c["opaque"]
    B = blur(rgb)
    h, s, v = hsv(B.astype(np.uint8))
    ring = np.zeros((96, 96), bool)
    ring[2:6, 6:90] = True; ring[6:86, 2:6] = True; ring[6:86, 90:94] = True
    ring &= op
    # backdrop hue: circular median of ring hues weighted to saturated pixels
    if ring.sum() < 20:
        return np.zeros((96, 96), bool), 0.0
    rh = h[ring]; rs = s[ring]; rv = v[ring]
    ang = np.deg2rad(rh)
    mh = np.rad2deg(np.arctan2(np.median(np.sin(ang)), np.median(np.cos(ang)))) % 360
    if mh_override is not None:
        mh = mh_override
    # ring pixels that agree with that hue are the seeds
    near = hue_dist(rh, mh) < 30
    ht = hue_tol if hue_tol is not None else max(18.0, float(np.percentile(hue_dist(rh[near] if near.sum() > 20 else rh, mh), 80)) + 8)
    ht = min(ht, 40.0)
    seed = ring & (hue_dist(h, mh) < ht)
    lo_s = max(0.0, float(np.percentile(s[seed], 3)) - 0.10) if seed.any() else 0
    lo_v = max(0.0, float(np.percentile(v[seed], 2)) - 0.12) if seed.any() else 0
    hi_v = min(1.0, float(np.percentile(v[seed], 98)) + 0.12) if seed.any() else 1
    ok = op & (hue_dist(h, mh) < ht + 6) & (s >= lo_s) & (v >= lo_v) & (v <= hi_v)
    bg = np.zeros((96, 96), bool)
    q = deque()
    for y, x in zip(*np.nonzero(seed & ok)):
        bg[y, x] = True; q.append((y, x))
    while q:
        y, x = q.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            Y, X = y + dy, x + dx
            if 0 <= Y < 96 and 0 <= X < 96 and not bg[Y, X] and ok[Y, X]:
                if np.abs(B[Y, X] - B[y, x]).max() < step:
                    bg[Y, X] = True; q.append((Y, X))
    # the frame: the dark outline (row 0, column 0), the right shadow (94-95) are never team;
    # the bottom band (rows 88-95) is the art darkened: it is backdrop where row 86 above it is
    bg[0, :] = True; bg[:, 0] = True; bg[:, 94:] = True
    for x in range(96):
        if bg[86, x] or not op[86, x]:
            bg[87:, x] |= op[87:, x]
    return bg, mh


def fill_small_holes(mask, maxsize=12):
    """Turn small enclosed islands of ~mask into mask (noise inside the backdrop)."""
    seen = np.zeros_like(mask)
    out = mask.copy()
    for y in range(96):
        for x in range(96):
            if mask[y, x] or seen[y, x]: continue
            comp = []; q = deque([(y, x)]); seen[y, x] = True
            while q:
                a, b = q.popleft(); comp.append((a, b))
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    Y, X = a + dy, b + dx
                    if 0 <= Y < 96 and 0 <= X < 96 and not mask[Y, X] and not seen[Y, X]:
                        seen[Y, X] = True; q.append((Y, X))
            if len(comp) <= maxsize:
                for a, b in comp: out[a, b] = True
    return out
