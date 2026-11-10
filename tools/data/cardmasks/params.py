"""Per-card parameters for round 2 (loaded from params.json; defaults here)."""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, "params.json")
DEFAULT = dict(bg_tol=11.0, bg_step=9.0, bg_iters=3, grow_step=14.0, hue_tol=28.0, chroma_free=7.0,
               L_min=6.0, close=1, open=1, island=12, hole_max=24)


def all_params():
    if os.path.exists(PATH):
        return json.load(open(PATH, encoding='utf-8-sig'))
    return {}


def get(name):
    p = dict(DEFAULT)
    p.update(all_params().get(name, {}))
    return p
