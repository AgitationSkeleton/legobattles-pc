#!/usr/bin/env python3
"""Drive the port in PLAY mode, where the frontend owns execution.

    python tools/lb_play.py warp            # reach Free Play gameplay
    python tools/lb_play.py fps [seconds]   # frame rate + per-phase breakdown
    python tools/lb_play.py shot <file>     # capture a screen

lb_warp.py cannot be used here. It advances the emulator with `run_cycles`,
which the debug server refuses once the frontend is running -- "run_cycles
unavailable in play mode: the frontend owns execution". So every wait here is
REAL TIME and every step is an input event, which is also the only way to
observe things that only exist in play mode: frame rate, input latency, and
whatever the compositor actually puts on screen.

That distinction is why the wide field shipped broken twice. Static .ppm dumps
came from a headless run and could not show lag, a mismapped click, or a HUD in
the wrong place, and those were exactly the faults reported.
"""

import argparse
import binascii
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dbg import Dbg


# (label, x, y, seconds-to-wait-after). x is None for "just wait".
ROUTE = [
    ("boot to title",         None, None, 22.0),
    ("title: touch to start",  128,  96,   3.0),
    ("profile: Create",        222, 179,   3.0),
    ("name: Q",                 20,  70,   1.0),
    ("name: W",                 44,  70,   1.0),
    ("name: E",                 68,  70,   1.0),
    ("name: Ok",               213, 175,   3.5),
    ("menu: Single Player",     75,  55,   3.0),
    ("single: select FreePlay",155,  88,   3.0),
    ("single: enter FreePlay", 155,  88,   4.0),
    ("map select: Continue",   215, 176,   4.0),
    ("rules: Continue",        215, 176,   4.5),
    ("army: Continue",         215, 176,   5.0),
    ("opponent: Start",        215, 176,   7.0),
    ("settle in game",        None, None,  6.0),
]


def tap(d, x, y, hold=0.12):
    d.cmd("touch", x=x, y=y, down=True)
    time.sleep(hold)
    d.cmd("touch", x=x, y=y, down=False)


def screen_sig(d, engine="B"):
    """A cheap fingerprint of the current screen."""
    r = d.cmd("framebuffer", engine=engine)
    raw = binascii.unhexlify(r["rgb"])
    # Every 61st byte, not every 1021st. The sparse version could not see a
    # button highlight, so taps that HAD landed were reported as no-response
    # and retried -- turning working taps into double-taps.
    return tuple(raw[i] for i in range(0, len(raw), 61))


def settle(d, timeout=12.0, quiet=0.5):
    """Wait until the screen stops changing, rather than for a fixed duration.

    The route used to wait a fixed number of seconds per step. That silently
    couples the harness to how fast the game runs -- and when the per-instruction
    hook was disabled the port went from 42 to 60 fps, every wait became
    mistimed, and the route stopped reaching gameplay at all. A speed fix broke
    the measurement harness, which is a good argument for never timing a route
    in seconds.

    Animated screens never fully freeze, so "settled" means two consecutive
    samples that are mostly equal, not identical.
    """
    end = time.time() + timeout
    prev = None
    stable_since = None
    while time.time() < end:
        sig = screen_sig(d)
        if prev is not None:
            same = sum(1 for a, b in zip(sig, prev) if a == b)
            if same >= len(sig) * 0.92:
                if stable_since is None:
                    stable_since = time.time()
                elif time.time() - stable_since >= quiet:
                    return True
            else:
                stable_since = None
        prev = sig
        time.sleep(0.15)
    return False


def in_gameplay(d):
    """True when the bottom screen is the battlefield.

    Every route here is timed in real seconds, so it diverges whenever the game
    takes a different path -- an extra confirmation screen, a slower load. When
    it does, the run ends somewhere plausible-looking and MEASUREMENTS ARE TAKEN
    ANYWAY. That has now produced four separate wrong results in this project:
    a frame-rate comparison of a menu against a battle, and a layer probe whose
    five readings all came from the character-customise screen.

    The battlefield is overwhelmingly terrain green and nothing else in the game
    is, so a coarse colour census settles it.
    """
    r = d.cmd("framebuffer", engine="B")
    raw = binascii.unhexlify(r["rgb"])
    w, h = r["w"], r["h"]
    green = total = 0
    for i in range(0, w * h * 3, 3 * 7):
        rr, gg, bb = raw[i], raw[i + 1], raw[i + 2]
        total += 1
        if gg > rr + 24 and gg > bb + 24 and gg > 60:
            green += 1
    return total and (green / total) > 0.30


def tap_until_change(d, x, y, tries=4):
    """Tap, and confirm the screen actually responded.

    A tap that lands during a transition does nothing, and the route then
    carries on pressing the coordinates of a screen it never reached. The
    failure is silent and ends somewhere plausible -- which is how five layer
    readings were taken on the character-customise screen without noticing.

    So: tap, wait for the screen to differ from what it was, and retry if it
    does not. Returns False when the screen never moved, which is a real signal
    rather than something to paper over.
    """
    for attempt in range(tries):
        before = screen_sig(d)
        tap(d, x, y)
        deadline = time.time() + 4.0
        while time.time() < deadline:
            time.sleep(0.2)
            now = screen_sig(d)
            same = sum(1 for p, q in zip(now, before) if p == q)
            if same < len(now) * 0.995:
                settle(d, timeout=10.0)
                return True
    return False


def cmd_warp(d, args):
    for label, x, y, wait in ROUTE:
        if x is None:
            time.sleep(min(wait, 2.0))
            settle(d, timeout=max(wait, 25.0))
            print("   %s" % label, flush=True)
            continue
        moved = tap_until_change(d, x, y)
        print("   %s%s" % (label, "" if moved else "   (no response)"), flush=True)
    if not in_gameplay(d):
        print("WARP FAILED: not in gameplay -- do not measure this run",
              file=sys.stderr)
        return 1
    print("   verified: in gameplay", flush=True)
    return 0


def cmd_fps(d, args):
    span = float(args.rest[0]) if args.rest else 6.0
    a = d.cmd("frontend_stats")
    t0 = time.time()
    time.sleep(span)
    b = d.cmd("frontend_stats")
    dt = time.time() - t0
    frames = b.get("frames", 0) - a.get("frames", 0)
    if not frames:
        print("no frames advanced -- is the frontend running?")
        return 1
    print("%.1f fps  (%d frames in %.1fs)" % (frames / dt, frames, dt))
    keys = ("emu_ticks", "present_ticks", "adaptive_ticks", "upload_ticks",
            "draw_ticks", "swap_ticks")
    delta = {k: b.get(k, 0) - a.get(k, 0) for k in keys}
    total = sum(delta.values())
    if total:
        for k in keys:
            print("   %-15s %5.1f%%  %8.3f ms/frame"
                  % (k, 100.0 * delta[k] / total,
                     delta[k] / max(frames, 1) / 1e6))
    return 0


def cmd_shot(d, args):
    path = args.rest[0] if args.rest else "shot.png"
    engine = args.engine
    r = d.cmd("framebuffer", engine=engine)
    w, h = r["w"], r["h"]
    rgb = binascii.unhexlify(r["rgb"])
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        fh.write(chunk(b"IEND", b""))
    print("%s  %dx%d" % (path, w, h))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["warp", "fps", "shot"])
    ap.add_argument("rest", nargs="*")
    ap.add_argument("--port", type=int, default=19842)
    ap.add_argument("--engine", default="B")
    args = ap.parse_args(argv)

    last = None
    for _ in range(40):
        try:
            d = Dbg(port=args.port)
            break
        except Exception as exc:                      # noqa: BLE001
            last = exc
            time.sleep(1)
    else:
        print("no debug server on %d: %s" % (args.port, last), file=sys.stderr)
        return 2

    return {"warp": cmd_warp, "fps": cmd_fps, "shot": cmd_shot}[args.action](d, args)


if __name__ == "__main__":
    raise SystemExit(main())
