#!/usr/bin/env python3
"""Drive a live session by KEYS and photograph each press.

    python tools/lb_drive.py A A DOWN A START --shot out/dir

The menus in this game are cursor driven -- the profile screen shows a
highlight, the name entry keeps its highlight on Q however precisely the key
rectangles are tapped -- so they are operated with the D-pad and A, not by
touch. Six route runs were spent discovering that one hypothesis at a time,
because each hypothesis cost a boot. This drives a session that is already up.

Key names are the DS's own. KEYINPUT is active low, so a held key is its bit
CLEARED in the mask.
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dbg import Dbg, assert_single_runner  # noqa: E402

KEYS = {
    "A": 0x001, "B": 0x002, "SELECT": 0x004, "START": 0x008,
    "RIGHT": 0x010, "LEFT": 0x020, "UP": 0x040, "DOWN": 0x080,
    "R": 0x100, "L": 0x200,
}


def press(d, name, hold=0.12, after=0.6):
    bit = KEYS[name]
    d.cmd("keys", mask=0x3FF & ~bit)
    time.sleep(hold)
    d.cmd("keys", mask=0x3FF)
    time.sleep(after)


def shot(path):
    subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File",
                    os.path.join(HERE, "lb_capture.ps1"), "-Out", path],
                   capture_output=True, timeout=90)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keys", nargs="+", help="key names, or TOUCH:x,y")
    ap.add_argument("--shot", default="")
    ap.add_argument("--port", type=int, default=19857)
    ap.add_argument("--after", type=float, default=1.2)
    args = ap.parse_args()

    assert_single_runner(args.port)
    d = Dbg(port=args.port)
    if args.shot:
        os.makedirs(args.shot, exist_ok=True)
        shot(os.path.join(args.shot, "00-start.png"))

    for i, k in enumerate(args.keys):
        if k.upper().startswith("TOUCH:"):
            x, y = (int(v) for v in k.split(":")[1].split(","))
            # Raw `touch`, in GUEST coordinates. frontend_input action=touch
            # takes WINDOW coordinates and runs them through the enhanced
            # mapping, which rejects anything outside the centred 256-column
            # band -- so guest-space arguments there are discarded silently.
            d.cmd("touch", x=x, y=y, down=True)
            time.sleep(0.15)
            d.cmd("touch", x=x, y=y, down=False)
            time.sleep(args.after)
            label = "touch-%d-%d" % (x, y)
        else:
            press(d, k.upper(), after=args.after)
            label = k.upper()
        print("  pressed %s" % label, flush=True)
        if args.shot:
            shot(os.path.join(args.shot, "%02d-%s.png" % (i + 1, label)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
