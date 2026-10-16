#!/usr/bin/env python3
"""Debug warp: drive the port from reset into a Free Play match.

The port's front end is stylus-driven, so nothing past the title screen can be
reached with buttons. This drives it through the debug server (`--serve`) with
`touch` and `run_cycles`, capturing frames so each step is verified rather than
assumed.

Every coordinate and cycle budget below was found by running the game and
looking at the frame, not guessed. The route is:

    logo -> title -> Choose Profile -> name keyboard -> Ok
         -> Main Menu (Single Player) -> Free Play (select, then confirm)
         -> map select (Continue) -> rules (Continue)
         -> army customise (Continue) -> opponent setup (Start) -> in game

Start the runner first:

    nds_runner.exe <bios> 4000000000 --rom <rom> --config port/game.toml \
        --boot direct --save-path lb.sav --serve --port 19842 \
        --generated-firmware --network off --freebios

then: python tools/lb_warp.py [--shot-dir DIR]
"""

from __future__ import annotations

import argparse
import binascii
import json
import socket
import struct
import sys
import zlib


class Dbg:
    def __init__(self, host="127.0.0.1", port=19842, timeout=1800.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.f = self.s.makefile("rwb")

    def cmd(self, name, **kw):
        kw["cmd"] = name
        self.f.write((json.dumps(kw) + "\n").encode())
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise RuntimeError("debug server closed the connection")
        return json.loads(line.decode("utf-8", "replace"))

    # -- execution -------------------------------------------------------
    def cycles(self):
        return self.cmd("run_cycles", arm9=0)["cycles"][0]

    def advance(self, n):
        target = self.cycles() + n
        for _ in range(60):
            if self.cmd("run_cycles", arm9=target).get("reached"):
                return True
        return False

    def tap(self, x, y, hold=4_000_000, settle=40_000_000):
        self.cmd("touch", x=x, y=y, down=True)
        self.advance(hold)
        self.cmd("touch", x=x, y=y, down=False)
        self.advance(settle)

    # -- capture ---------------------------------------------------------
    def shot(self, path, engine="B", adaptive=False):
        r = self.cmd("framebuffer", engine=engine, adaptive=adaptive)
        if "error" in r:
            return r
        w, h, rgb = r["w"], r["h"], binascii.unhexlify(r["rgb"])
        raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

        def chunk(tag, data):
            body = struct.pack(">I", len(data)) + tag + data
            return body + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        with open(path, "wb") as fh:
            fh.write(b"\x89PNG\r\n\x1a\n")
            fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
            fh.write(chunk(b"IDAT", zlib.compress(raw, 6)))
            fh.write(chunk(b"IEND", b""))
        return {"w": w, "h": h, "path": path}


# The route. (label, x, y, settle-cycles); x is None for "just wait".
ROUTE = [
    ("boot to title",        None, None, 2_200_000_000),
    ("title: touch to start", 128,  96,     40_000_000),
    ("profile: Create",       222, 179,     40_000_000),
    ("name: Q",                20,  70,     12_000_000),
    ("name: W",                44,  70,     12_000_000),
    ("name: E",                68,  70,     12_000_000),
    ("name: Ok",              213, 175,     60_000_000),
    ("menu: Single Player",    75,  55,     50_000_000),
    ("single: select FreePlay",155, 88,     60_000_000),
    ("single: enter FreePlay", 155, 88,     80_000_000),
    ("map select: Continue",  215, 176,     80_000_000),
    ("rules: Continue",       215, 176,     90_000_000),
    ("army: Continue",        215, 176,    120_000_000),
    ("opponent: Start",       215, 176,    150_000_000),
    ("settle in game",       None, None,   400_000_000),
]


def warp(d, shot_dir=None, verbose=True):
    for i, (label, x, y, settle) in enumerate(ROUTE):
        if x is None:
            # An absolute target for the boot step, relative for the rest.
            if settle > 1_000_000_000:
                now = d.cycles()
                if now < settle:
                    d.advance(settle - now)
            else:
                d.advance(settle)
        else:
            d.tap(x, y, settle=settle)
        if verbose:
            print("  %2d. %-26s @ %d cycles" % (i + 1, label, d.cycles()))
        if shot_dir:
            d.shot("%s/%02d_%s.png" % (shot_dir, i,
                                       label.split(":")[0].replace(" ", "_")))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19842)
    ap.add_argument("--shot-dir")
    args = ap.parse_args()
    d = Dbg(port=args.port)
    print("warping into Free Play...")
    warp(d, args.shot_dir)
    print("in game at %d cycles" % d.cycles())
    return 0


if __name__ == "__main__":
    sys.exit(main())
