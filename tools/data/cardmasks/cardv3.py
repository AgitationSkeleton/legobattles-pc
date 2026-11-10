"""Round 2: region masks (backdrop flood + seeded Lab region growth) and continuous LCh recolouring.

Everything works on one 96x96 card (RGB as decoded from the ROM, see ../cardlib.py)."""
import os, sys
from collections import deque
import numpy as np
from scipy import ndimage as ndi

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cardlib  # noqa: E402

# ------------------------------------------------------------------ colour -----

def _lin(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _gam(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, 12.92 * c, 1.055 * c ** (1 / 2.4) - 0.055) * 255.0


M = np.array([[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]])
Mi = np.linalg.inv(M)
WP = np.array([0.95047, 1.0, 1.08883])


def rgb2lab(rgb):
    xyz = _lin(np.asarray(rgb, float)) @ M.T / WP
    f = np.where(xyz > 216 / 24389, np.cbrt(xyz), (24389 / 27 * xyz + 16) / 116)
    L = 116 * f[..., 1] - 16
    a = 500 * (f[..., 0] - f[..., 1])
    b = 200 * (f[..., 1] - f[..., 2])
    return np.stack([L, a, b], -1)


def lab2rgb(lab):
    L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
    fy = (L + 16) / 116; fx = fy + a / 500; fz = fy - b / 200
    def inv(f):
        return np.where(f ** 3 > 216 / 24389, f ** 3, (116 * f - 16) / (24389 / 27))
    xyz = np.stack([inv(fx), inv(fy), inv(fz)], -1) * WP
    return _gam(xyz @ Mi.T)


def ds_lab(c):
    return rgb2lab(cardlib.ds_rgb_arr(np.array([c]))[0].astype(float))


TEAMS = {
    "purple": (0x7C1F, 0x4419, 0x1C12),     # stock Purple (0x02127F38 row 4)
    "teal": (0x6764, 0x4E80, 0x31A0),       # docs/412 "Teal (PC Port)"
    "navy": (0x4D06, 0x3083, 0x1842),       # a dark navy, not a stock colour (test of a dark team)
    "red": (0x113F, 0x005C, 0x0015),
    "green": (0x03AF, 0x02A6, 0x01E0),
}

# ------------------------------------------------------------------ backdrop ----

N4 = ((1, 0), (-1, 0), (0, 1), (0, -1))


def smooth_lab(lab, s=0.8):
    return np.stack([ndi.gaussian_filter(lab[..., k], s) for k in range(3)], -1)


def quad_fit(lab, mask):
    """Least-squares quadratic surface (in x, y) for each Lab channel over mask."""
    ys, xs = np.nonzero(mask)
    X = np.stack([np.ones_like(xs), xs, ys, xs * xs, ys * ys, xs * ys], 1).astype(float) / [1, 96, 96, 9216, 9216, 9216]
    yy, xx = np.mgrid[0:96, 0:96]
    G = np.stack([np.ones_like(xx), xx, yy, xx * xx, yy * yy, xx * yy], -1).astype(float) / [1, 96, 96, 9216, 9216, 9216]
    out = np.zeros((96, 96, 3))
    for k in range(3):
        coef, *_ = np.linalg.lstsq(X, lab[..., k][mask], rcond=None)
        out[..., k] = G @ coef
    return out


def de(a, b):
    return np.sqrt(((a - b) ** 2).sum(-1))


def backdrop(card, p):
    """The backdrop: a smooth field touching the card border. Iteratively fitted and flooded.
    p: bg_tol (dE to the fitted field), bg_step (dE between neighbours), bg_iters."""
    rgb = card["rgb"].astype(float); op = card["opaque"]
    lab = rgb2lab(rgb); S = smooth_lab(lab, 0.7)
    T = p.get("bg_tol", 11.0); step = p.get("bg_step", 9.0)
    art = np.zeros((96, 96), bool); art[1:87, 1:94] = True   # the art above the HP band, inside the frame
    ring = np.zeros((96, 96), bool)
    ring[1:4, 3:92] = True; ring[3:85, 1:4] = True; ring[3:85, 91:94] = True
    ring &= op & art
    for (x0, y0, x1, y1) in p.get("bg_noseed", []):
        ring[y0:y1 + 1, x0:x1 + 1] = False
    seed = ring.copy()
    # robust start: the ring pixels near the ring's median colour
    med = np.median(S[seed], 0)
    seed &= de(S, med) < max(T * 2.2, 20)
    bg = seed.copy()
    for it in range(p.get("bg_iters", 3)):
        field = quad_fit(S, bg if bg.sum() > 40 else seed)
        cand = op & art & (de(S, field) < T)
        bg = np.zeros((96, 96), bool)
        q = deque()
        for y, x in zip(*np.nonzero(seed & cand)):
            bg[y, x] = True; q.append((y, x))
        while q:
            y, x = q.popleft()
            for dy, dx in N4:
                Y, X = y + dy, x + dx
                if 0 <= Y < 96 and 0 <= X < 96 and not bg[Y, X] and cand[Y, X] and de(S[Y, X], S[y, x]) < step:
                    bg[Y, X] = True; q.append((Y, X))
    # relaxed pass: the backdrop's brush strokes and the light halo around the figure, reached
    # only from the backdrop itself (a wider tolerance to the field, the same neighbour step)
    relax = p.get("bg_relax", 1.6)
    if relax > 1:
        cand2 = op & art & (de(S, field) < T * relax)
        q = deque(zip(*np.nonzero(bg)))
        while q:
            y, x = q.popleft()
            for dy, dx in N4:
                Y, X = y + dy, x + dx
                if 0 <= Y < 96 and 0 <= X < 96 and not bg[Y, X] and cand2[Y, X] and de(S[Y, X], S[y, x]) < step * 1.3:
                    bg[Y, X] = True; q.append((Y, X))
    # enclosed pockets: backdrop seen through gaps (under an arm, through a hole in the body)
    # is not connected to the border. A component of field-like pixels of pocket_min px or more
    # whose pixels are close to the field AND to the backdrop's own colours becomes backdrop.
    if p.get("pockets", True):
        cand3 = op & art & ~bg & (de(S, field) < T * p.get("pocket_tol", 1.4))
        lp, n = ndi.label(cand3)
        for i in range(1, n + 1):
            comp = lp == i
            if comp.sum() >= p.get("pocket_min", 10):
                bg |= comp
        # grow the pockets by the same neighbour step, like the main flood
        q = deque(zip(*np.nonzero(bg)))
        cand4 = op & art & (de(S, field) < T * relax)
        while q:
            y, x = q.popleft()
            for dy, dx in N4:
                Y, X = y + dy, x + dx
                if 0 <= Y < 96 and 0 <= X < 96 and not bg[Y, X] and cand4[Y, X] and de(S[Y, X], S[y, x]) < step * 1.3:
                    bg[Y, X] = True; q.append((Y, X))
    # small non-backdrop specks enclosed by backdrop (dither) become backdrop
    holes = ndi.binary_fill_holes(bg) & ~bg
    lab_h, n = ndi.label(holes)
    if n:
        sizes = ndi.sum(holes, lab_h, range(1, n + 1))
        for i, sz in enumerate(sizes, 1):
            if sz <= p.get("bg_speck", 6): bg[lab_h == i] = True
    # the HP band (rows 87-95): backdrop under backdrop columns, if it looks like the darkened backdrop
    band = np.zeros((96, 96), bool)
    cols = bg[84:87].all(0)
    if cols.any():
        ref = np.median(S[89:95][:, cols], axis=(0, 1)) if cols.sum() > 2 else None
        for x in range(96):
            for y in range(87, 96):
                if not op[y, x]: continue
                if ref is not None and de(S[y, x], ref) < T * 1.3:
                    band[y, x] = True
    bg |= band & op
    # the frame
    bg[0, :] = True; bg[:, 0] = True; bg[:, 94:] = True
    for (x0, y0, x1, y1) in p.get("bg_add", []):
        bg[y0:y1 + 1, x0:x1 + 1] = True
    for (x0, y0, x1, y1) in p.get("bg_keep", []):   # never backdrop here (hand fix)
        bg[y0:y1 + 1, x0:x1 + 1] = False
    return bg


# ------------------------------------------------------------------ team region -

def rect_mask(rects):
    m = np.zeros((96, 96), bool)
    for r in rects or []:
        x0, y0, x1, y1 = r
        m[y0:y1 + 1, x0:x1 + 1] = True
    return m


def poly_mask(polys):
    """Polygons as lists of (x, y); pixel centres inside are set."""
    m = np.zeros((96, 96), bool)
    yy, xx = np.mgrid[0:96, 0:96] + 0.5
    for poly in polys or []:
        P = np.array(poly, float)
        inside = np.zeros((96, 96), bool)
        n = len(P)
        for i in range(n):
            x1, y1 = P[i]; x2, y2 = P[(i + 1) % n]
            cond = ((y1 > yy) != (y2 > yy))
            xint = (x2 - x1) * (yy - y1) / ((y2 - y1) if y2 != y1 else 1e-9) + x1
            inside ^= cond & (xx < xint)
        m |= inside
    return m


def hue_deg(lab):
    return np.degrees(np.arctan2(lab[..., 2], lab[..., 1])) % 360


def hdist(a, b):
    d = np.abs(a - b) % 360
    return np.minimum(d, 360 - d)


#TEAM_REGION_HERE

def soft_alpha(mask, bg, excl, sigma=0.65):
    """Feathered alpha: a slightly blurred mask, never on the backdrop or an exclusion, never
    more than one pixel outside the mask."""
    a = ndi.gaussian_filter(mask.astype(float), sigma)
    a = np.where(mask, np.clip(a * 1.25, 0, 1), a * 0.8)
    near = ndi.binary_dilation(mask, iterations=1)
    a[~near] = 0; a[bg] = 0; a[excl] = 0
    return np.clip(a, 0, 1)


# ------------------------------------------------------------------ recolour ----

def recolour(card, mask, alpha, team, p=None):
    """Continuous LCh recolour of the masked part onto a team's ramp.
    L: monotone remap of the part's own L range (2..98th pct) into the target's range
       (dark shade's L .. light shade's L, widened a little), soft beyond the ends.
    hue/chroma: the target ramp's a,b at that L (interpolated through the three shades),
       scaled by the pixel's chroma relative to the part's own chroma (90th pct)."""
    p = p or {}
    rgb = card["rgb"].astype(float)
    lab = rgb2lab(rgb)
    out = rgb.copy()
    if not mask.any(): return out.astype(np.uint8)
    sh = [ds_lab(c) for c in team]                   # light, mid, dark
    Ls = lab[..., 0][mask]
    lo, hi = np.percentile(Ls, 2), np.percentile(Ls, 98)
    if hi - lo < 8: lo, hi = lo - 4, hi + 4
    Ld, Lm, Ll = sh[2][0], sh[1][0], sh[0][0]
    spread = p.get("L_spread", 0.18)                 # how far past the shades the part may go
    tlo = max(2.0, Ld - spread * (Ll - Ld) - 4); thi = min(98.0, Ll + spread * (Ll - Ld) + 4)
    # keep some of the source's own contrast if it is larger than the target's
    t = (lab[..., 0] - lo) / (hi - lo)
    t_s = np.where(t < 0, t * 0.5, np.where(t > 1, 1 + (t - 1) * 0.5, t))
    Lout = tlo + t_s * (thi - tlo)
    Lout = np.clip(Lout, 0, 100)
    # target a,b along the ramp, by Lout
    ramp_L = np.array([Ld, Lm, Ll]); ramp_ab = np.array([sh[2][1:], sh[1][1:], sh[0][1:]])
    if not (ramp_L[0] < ramp_L[1] < ramp_L[2]):
        ramp_L = np.sort(ramp_L)
    a_t = np.interp(Lout, ramp_L, ramp_ab[:, 0]); b_t = np.interp(Lout, ramp_L, ramp_ab[:, 1])
    Ct = np.hypot(a_t, b_t)
    C = np.hypot(lab[..., 1], lab[..., 2])
    cref = max(6.0, np.percentile(C[mask], 90))
    crel = np.clip(C / cref, 0.0, 1.25)
    crel = p.get("chroma_keep", 0.35) + (1 - p.get("chroma_keep", 0.35)) * crel   # never fully grey
    Cout = Ct * crel
    ht = np.arctan2(b_t, a_t)
    new = np.stack([Lout, Cout * np.cos(ht), Cout * np.sin(ht)], -1)
    nrgb = lab2rgb(new)
    A = alpha[..., None]
    out = rgb * (1 - A) + nrgb * A
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


#TEAM_MASK_HERE
