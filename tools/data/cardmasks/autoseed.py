"""Seeds for cards without hand seeds: the round-1 hue rule's pixels (../rules.py), restricted to
the round-2 character (not backdrop), eroded to their cores so edge/dither pixels never seed."""
import os, sys
import numpy as np
from scipy import ndimage as ndi
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rules, segment  # noqa: E402


def rule_hit(rule, h, s, v):
    h0, h1 = rule["h0"], rule["h1"]
    hm = (h >= h0) & (h <= h1) if h0 <= h1 else (h >= h0) | (h <= h1)
    m = hm & (s >= rule["s"]) & (v >= rule["v0"]) & (v <= rule["v1"])
    if rule.get("rect"):
        rm = np.zeros_like(m)
        for (x0, y0, x1, y1) in rule["rect"]:
            rm[y0:y1 + 1, x0:x1 + 1] = True
        m &= rm
    for (x0, y0, x1, y1) in (rule.get("ex") or []):
        m[y0:y1 + 1, x0:x1 + 1] = False
    return m


def card_rule(name):
    if name in rules.RULES: return rules.RULES[name]["r"]
    for p, rr in rules.BUILDING_FACTION.items():
        if name.startswith(p): return rr["r"]
    return []


def seeds(card, name, bg):
    rr = card_rule(name)
    if not rr: return np.zeros((96, 96), bool)
    h, s, v = segment.hsv(card["rgb"])
    m = np.zeros((96, 96), bool)
    for r in rr: m |= rule_hit(r, h, s, v)
    m &= card["opaque"] & ~bg
    core = ndi.binary_erosion(m, iterations=1)
    lab, n = ndi.label(core)
    out = np.zeros_like(core)
    for i in range(1, n + 1):
        c = lab == i
        if c.sum() >= 4: out |= c
    return out
