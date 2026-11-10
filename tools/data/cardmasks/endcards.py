"""Victory/defeat end-screen art: UI/Heroes/<name>.NCBR (256x192, 8bpp linear) + .NCLR (256 colours).
Decoded into the same card dict shape as cardlib (I, P, rgb, opaque, bp)."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lbcard, cardlib  # noqa: E402

D = os.path.join(lbcard.FS, "UI", "Heroes")
# hero blueprint -> (victory art, defeat art), from FUN_020C63E8 (SkirmishScoreTopScreen)
HERO = {0: "K", 1: "K_2", 20: "W", 21: "W_2", 40: "P", 41: "P_2", 60: "I", 61: "I_2", 80: "E", 81: "E_2", 100: "A", 101: "A_2"}


def names():
    return sorted({os.path.splitext(n)[0] for n in os.listdir(D) if n.upper().endswith(".NCBR")})


def load(name):
    d = lbcard.rom("UI/Heroes/%s.NCBR" % name)
    pal = lbcard.nclr("UI/Heroes/%s.NCLR" % name)
    th, tw = lbcard.u16(d, 0x18), lbcard.u16(d, 0x1A)
    base = 0x18 + lbcard.u32(d, 0x2C); w, h = tw * 8, th * 8
    I = np.frombuffer(d[base:base + w * h], np.uint8).reshape(h, w).astype(np.int32)
    P = np.array(pal + [0] * (256 - len(pal)), np.int64)
    rgb = cardlib.ds_rgb_arr(P[I])
    return dict(bp=-1, I=I, P=P, rgb=rgb, opaque=np.ones((h, w), bool), name=name)
