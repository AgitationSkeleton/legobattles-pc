"""Round 3 team regions: seeded growth with leftover absorption, hue-select parts for small
pieces, and the defect check (leftover source-hue pixels; bleed into other hues)."""
import os, sys
from collections import deque
import numpy as np
from scipy import ndimage as ndi
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cardv3 import (rgb2lab, smooth_lab, de, hue_deg, hdist, rect_mask, poly_mask, N4)  # noqa: E402

ST4 = ndi.generate_binary_structure(2, 1)


def _clusters(m, min_px):
    lab, n = ndi.label(m, structure=np.ones((3, 3)))
    out = np.zeros_like(m)
    if n:
        sz = ndi.sum(m, lab, range(1, n + 1))
        for i, s in enumerate(sz, 1):
            if s >= min_px: out |= lab == i
    return out


def region_masks(p):
    excl = rect_mask(p.get("excl")) | poly_mask(p.get("excl_poly"))
    within = None
    if p.get("within") or p.get("within_poly"):
        within = rect_mask(p.get("within")) | poly_mask(p.get("within_poly"))
    return excl, within


def _dominant_hue(H, C, seeds, tol):
    sh = H[seeds & (C >= 8)]; sw = C[seeds & (C >= 8)]
    if sh.size == 0:
        sh = H[seeds]; sw = C[seeds] + 1e-3
    best, hs = -1, 0.0
    for cand in range(0, 360, 5):
        sc = sw[hdist(sh, cand) < tol].sum()
        if sc > best: best, hs = sc, float(cand)
    near = hdist(sh, hs) < tol
    if near.any():
        ww = sw[near]
        hs = np.degrees(np.arctan2((ww * np.sin(np.radians(sh[near]))).sum(), (ww * np.cos(np.radians(sh[near]))).sum())) % 360
    return float(hs)


def part_region(card, p, bg):
    """One part. mode 'grow' (default): seeded growth, then absorb every same-hue cluster of
    absorb_min px or more in the allowed area. mode 'hue': every allowed pixel in the hue window
    (for small, scattered team pieces: lights, tubes, stripes), crisp, islands >= island px."""
    rgb = card["rgb"].astype(float); op = card["opaque"]
    lab = rgb2lab(rgb); S = smooth_lab(lab, 0.5)
    L = lab[..., 0]; C = np.hypot(lab[..., 1], lab[..., 2]); H = hue_deg(lab)
    excl, within = region_masks(p)
    allowed = op & ~excl & (~bg if not p.get("ignore_bg") else True)
    if within is not None: allowed &= within
    tol = p.get("hue_tol", 28.0)
    Lmin = p.get("L_min", 6.0); Lmax = p.get("L_max", 101.0)
    Cmin = p.get("C_min", 0.0); Cmax = p.get("C_max", 999.0)
    mode = p.get("mode", "grow")
    seeds = np.zeros((96, 96), bool)
    for (x, y) in p.get("seeds", []): seeds[y, x] = True
    seeds |= rect_mask(p.get("seed_rects")) | poly_mask(p.get("seed_polys"))
    if p.get("_seedmask") is not None: seeds |= p["_seedmask"]
    seeds &= allowed
    if mode == "hue":
        hs = float(p["hue"])
        cmin = max(Cmin, p.get("hue_Cmin", 10.0))
        reg = allowed & (hdist(H, hs) < tol) & (C >= cmin) & (C <= Cmax) & (L >= Lmin) & (L <= Lmax)
        if p.get("enclosed"):   # thin parts outlined in dark: keep pixels with dark outline on two opposite sides
            R = int(p["enclosed"]); dk = (L < p.get("dark_L", 24)) & ~bg & op
            pad = np.pad(dk, R)
            ok2 = np.zeros_like(reg)
            for dy, dx in ((0, 1), (1, 0), (1, 1), (1, -1)):
                a = np.zeros_like(reg); b = np.zeros_like(reg)
                for k in range(1, R + 1):
                    a |= pad[R + dy * k:R + dy * k + 96, R + dx * k:R + dx * k + 96]
                    b |= pad[R - dy * k:R - dy * k + 96, R - dx * k:R - dx * k + 96]
                ok2 |= a & b
            reg &= ok2
        if p.get("close", 0): reg = ndi.binary_closing(reg, ST4, iterations=p["close"]) & allowed
        if p.get("rim", 1):   # the lit halo between a figure and the backdrop is not a team part
            if not p.get("ignore_bg"): reg &= ~ndi.binary_dilation(bg & op, structure=ST4, iterations=p.get("rim", 1))
        if p.get("fill", 0):   # cover the whole part: highlights and shades enclosed by it
            reg = (ndi.binary_fill_holes(ndi.binary_closing(reg, ST4, iterations=p["fill"])) & allowed) | reg
        if p.get("drop_hue") is not None:   # parts of another hue enclosed by the team part (trim, emblems) stay
            reg &= ~((hdist(H, p["drop_hue"]) < p.get("drop_tol", 25)) & (C >= p.get("drop_Cmin", 20)))
        if p.get("drop2_hue") is not None:
            reg &= ~((hdist(H, p["drop2_hue"]) < p.get("drop2_tol", 25)) & (C >= p.get("drop2_Cmin", 20)))
        reg = _clusters(reg, p.get("island", 3))
        cm = float(np.median(C[reg])) if reg.any() else cmin
        return reg, dict(hue=hs, tol=tol, Lmin=Lmin, Lmax=Lmax, cmed=cm, cmin=cmin, within=within, excl=excl, hue_mode=True)
    if not seeds.any():
        return np.zeros((96, 96), bool), None
    hs = float(p["seed_hue"]) if p.get("seed_hue") is not None else _dominant_hue(H, C, seeds, tol)
    cmed = float(np.median(C[ndi.binary_dilation(seeds) & allowed]))
    cfree = p.get("chroma_free", 7.0)
    ok = allowed & (L >= Lmin) & (L <= Lmax) & (C >= Cmin) & (C <= Cmax)
    ok &= (hdist(H, hs) < tol) | ((C < cfree) & (L < p.get("free_L", 28.0)))
    step = p.get("grow_step", 14.0)
    reg = seeds & ok
    q = deque(zip(*np.nonzero(reg)))
    while q:
        y, x = q.popleft()
        for dy, dx in N4:
            Y, X = y + dy, x + dx
            if 0 <= Y < 96 and 0 <= X < 96 and not reg[Y, X] and ok[Y, X] and de(S[Y, X], S[y, x]) < step:
                reg[Y, X] = True; q.append((Y, X))
    # absorb: same-hue clusters the growth did not reach (separated by an outline or a highlight)
    if p.get("absorb", True):
        cabs = max(p.get("absorb_Cmin", 10.0), 0.3 * cmed)
        extra = ok & (hdist(H, hs) < tol) & (C >= cabs) & ~reg
        reg |= _clusters(extra, p.get("absorb_min", 3))
    holes = ndi.binary_fill_holes(reg) & ~reg
    hl, n = ndi.label(holes)
    if n:
        sz = ndi.sum(holes, hl, range(1, n + 1))
        for i, s in enumerate(sz, 1):
            if s <= p.get("hole_max", 24): reg[hl == i] = True
    if p.get("close", 1): reg = ndi.binary_closing(reg, ST4, iterations=p.get("close", 1))
    if p.get("open", 0): reg = ndi.binary_opening(reg, ST4, iterations=p.get("open", 0))
    forced = rect_mask(p.get("incl")) | poly_mask(p.get("incl_poly"))
    reg = (reg & allowed) | (forced & op & ~bg)
    reg = _clusters(reg, p.get("island", 4))
    return reg, dict(hue=hs, tol=tol, Lmin=Lmin, Lmax=Lmax, cmed=cmed, cmin=max(10.0, 0.3 * cmed), within=within, excl=excl)


def team_mask(card, p, bg, name=None):
    """Union of parts. Returns (mask, infos, excl_union)."""
    excl_all, _ = region_masks(p)
    if p.get("disabled"):
        return np.zeros((96, 96), bool), [], excl_all
    parts = p.get("parts")
    if not parts:
        if p.get("seeds") or p.get("seed_rects") or p.get("seed_polys") or p.get("mode") == "hue":
            parts = [{}]
        elif name is not None:
            import autoseed
            parts = [{"_rule": r} for r in autoseed.card_rule(name)]
        else:
            parts = [{}]
    # an empty part with no seeds and no hue mode means "the card's automatic part": expand it to
    # the round-1 rule seeds, as a card without parts would get (v6 regression: the Queen's dress)
    if name is not None and p.get("mode", "grow") != "hue" and not (p.get("seeds") or p.get("seed_rects") or p.get("seed_polys")):
        import autoseed
        auto = [{"_rule": r} for r in autoseed.card_rule(name)]
        new = []
        for part in parts:
            if set(part) <= {"seed_hue"}: new.extend([dict(a, **part) for a in auto])
            else: new.append(part)
        parts = new
    m = np.zeros((96, 96), bool); infos = []
    for part in parts:
        q = dict(p); q.pop("parts", None); q.update(part)
        if "_rule" in q:
            import autoseed
            h, s, v = __import__("segment").hsv(card["rgb"])
            hit = autoseed.rule_hit(q["_rule"], h, s, v) & card["opaque"] & ~bg
            core = ndi.binary_erosion(hit, iterations=1)
            q["_seedmask"] = _clusters(core, 4)
        r, info = part_region(card, q, bg)
        if info is not None:
            infos.append(info); excl_all |= info["excl"]
        m |= r
    return m, infos, excl_all


def defects(card, mask, alpha, bg, excl_all, infos):
    """leftover: pixels in the character, not carved out, not recoloured (alpha < 0.5), in a part's
    source hue (tol + 8) with real chroma, in clusters of 4+ px. bleed: recoloured pixels of real
    chroma whose hue is far (tol + 20) from every part's source hue. Returns (leftover, bleed, maps)."""
    op = card["opaque"]
    lab = rgb2lab(card["rgb"].astype(float))
    L = lab[..., 0]; C = np.hypot(lab[..., 1], lab[..., 2]); H = hue_deg(lab)
    # the 1-px rim next to the backdrop is the lit halo between figure and backdrop: not counted
    rim = ndi.binary_dilation(bg & op, structure=ST4) & ~bg
    char = op & ~bg & ~rim & ~excl_all & (alpha < 0.5)
    left = np.zeros((96, 96), bool)
    for inf in infos:
        c = char & (hdist(H, inf["hue"]) < inf["tol"] + 8) & (C >= max(15.0, 0.4 * inf["cmed"], inf["cmin"] if inf.get("hue_mode") else 0.0)) \
            & (L >= inf["Lmin"] - 5) & (L <= inf["Lmax"] + 5)
        if inf["within"] is not None: c &= inf["within"]
        left |= c
    left = _clusters(left, 4)
    bleed = np.zeros((96, 96), bool)
    if infos:
        far = np.ones((96, 96), bool)
        for inf in infos: far &= hdist(H, inf["hue"]) > inf["tol"] + 20
        bleed = mask & (C >= 15) & far
    return int(left.sum()), int(bleed.sum()), left, bleed
