#!/usr/bin/env python3
"""Lift the ARM9 functions this port patches, from their PATCHED bytes.

The port changes a few instructions and literals of cartridge code at run time
(lb_patches.cpp byte patches: a widened allocation, a relocated table, ...).
The lifted bank lb_arm9 validates each function against the ROM's bytes, so a
function holding one patched word fails validation and runs in the Tier-3
interpreter, and so does everything it calls from there. In a late Free Play
battle that was ~3.4k interpreted ARM9 instructions a frame, ~4% of the
emulation thread (2026-11-07).

This makes a second, small bank, lb_arm9_patched, lifted from a copy of the
ARM9 static image with the live patches applied. The runtime tries every bank
and takes the first entry whose bytes validate (runtime_arm.cpp lookup_static),
so in patched RAM the patched bank's code runs, and in unpatched RAM (a setting
off) the stock bank's does, as before. Nothing is changed in the runtime.

    py -3.13 tools/lb_patched_bank.py scan --port 19859
        reads live ARM9 static RAM from a running battle (debug server) and
        writes port/config/lb_arm9_patched.toml + out/eur/arm9/patched_02000000.bin
    ./port/lift.sh emit   (lifts it with the others: lift.sh runs the config)

Only words inside a lifted function's validation range are taken from RAM;
the rest of the image (data, .bss) stays the ROM's. A function is included
when any of its validated bytes differ.
"""
import argparse
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

BASE = 0x02000000
ROM_BIN = os.path.join(ROOT, "out", "eur", "arm9", "section_static_02000000.bin")
OUT_BIN = os.path.join(ROOT, "out", "eur", "arm9", "patched_static_02000000.bin")
TOML = os.path.join(ROOT, "port", "config", "lb_arm9.toml")
OUT_TOML = os.path.join(ROOT, "port", "config", "lb_arm9_patched.toml")
DISPATCH = os.path.join(ROOT, "port", "generated", "lb_arm9_dispatch.c")
# Patches the port applies only in some matches, which a scan may not see:
# folded in after every scan. 0203AD34: Unit_BuildBillboard's sprite slot
# read, while the larger sprite pool's extension table is in use (docs/479).
EXTRA_WORDS = ["0203AD34=E5911004"]


def validation_ranges():
    """[(addr, size)] of every lb_arm9 validation record."""
    src = open(DISPATCH, encoding="utf-8", errors="replace").read()
    out = []
    for m in re.finditer(r"static const NdsStaticValidation g_validation_lb_arm9__\d+ = "
                         r"\{0x([0-9A-F]+)u, (\d+)u,", src):
        out.append((int(m.group(1), 16), int(m.group(2))))
    return sorted(set(out))


def entry_points():
    """[(addr, size, mode, text)] from lb_arm9.toml, in file order."""
    txt = open(TOML, encoding="utf-8").read()
    head, _, rest = txt.partition("[[entry_point]]")
    eps = []
    for block in ("[[entry_point]]" + rest).split("[[entry_point]]")[1:]:
        a = re.search(r"addr\s*=\s*0x([0-9A-Fa-f]+)", block)
        s = re.search(r"size\s*=\s*0x([0-9A-Fa-f]+)", block)
        mo = re.search(r'mode\s*=\s*"(\w+)"', block)
        if a and s:
            eps.append((int(a.group(1), 16), int(s.group(1), 16),
                        mo.group(1) if mo else "arm", "[[entry_point]]" + block))
    return head, eps


def scan(port):
    from dbg import Dbg
    rom = bytearray(open(ROM_BIN, "rb").read())
    d = Dbg(port=port, timeout=120)
    live = bytearray()
    step = 0x10000
    for off in range(0, len(rom), step):
        n = min(step, len(rom) - off)
        live += bytes.fromhex(d.cmd("read_mem", cpu=9, addr=BASE + off, len=n)["hex"])
    patched = bytearray(rom)
    changed = []
    for a, s in validation_ranges():
        o = a - BASE
        if o < 0 or o + s > len(rom):
            continue
        if live[o:o + s] != rom[o:o + s]:
            patched[o:o + s] = live[o:o + s]
            changed.append((a, s))
    head, eps = entry_points()
    keep = []
    for ea, es, mode, text in eps:
        if any(ea < a + s and a < ea + es for a, s in changed):
            keep.append((ea, es, mode, text))
    words = sum(1 for i in range(0, len(rom), 4)
                if patched[i:i + 4] != rom[i:i + 4])
    print("%d validated ranges differ (%d words); %d entry points to lift"
          % (len(changed), words, len(keep)))
    for a, s in changed:
        o = a - BASE
        diff = [BASE + i for i in range(o, o + s, 4) if patched[i:i + 4] != rom[i:i + 4]]
        print("  %08X+%-4d words %s" % (a, s, " ".join("%08X" % x for x in diff[:6])))
    open(OUT_BIN, "wb").write(bytes(patched))
    sha = hashlib.sha1(bytes(patched)).hexdigest()
    head = re.sub(r'name\s*=\s*"[^"]*"', 'name         = "LEGO Battles (EUR) ARM9 static, port-patched"', head, count=1)
    head = re.sub(r'id\s*=\s*"lb_arm9"', 'id           = "lb_arm9_patched"', head, count=1)
    head = re.sub(r'sha1\s*=\s*"[0-9a-f]+"', 'sha1 = "%s"' % sha, head, count=1)
    head = head.replace("# AUTO-GENERATED by tools/lb_ndsrecomp.py. Do not hand-edit.",
                        "# AUTO-GENERATED by tools/lb_patched_bank.py from a live battle's RAM.\n"
                        "# The functions the port byte-patches, lifted from their patched bytes.")
    with open(OUT_TOML, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(head)
        for _a, _s, _m, text in keep:
            fh.write(text if text.endswith("\n") else text + "\n")
    print("wrote %s (sha1 %s) and %s" % (OUT_BIN, sha, OUT_TOML))
    add(EXTRA_WORDS)


def add(words):
    """Fold patch words the port applies only some of the time (so a scan of
    one battle may not see them) into the existing patched image and config.
    words: ["ADDR=WORD", ...] in hex. Adds each word's entry point."""
    rom = open(ROM_BIN, "rb").read()
    patched = bytearray(open(OUT_BIN, "rb").read())
    txt = open(OUT_TOML, encoding="utf-8").read()
    have = set(int(m.group(1), 16) for m in re.finditer(r"addr\s*=\s*0x([0-9A-Fa-f]+)", txt))
    _head, eps = entry_points()
    for w in words:
        a, v = (int(x, 16) for x in w.split("="))
        o = a - BASE
        patched[o:o + 4] = v.to_bytes(4, "little")
        print("  %08X: %08X -> %08X" % (a, int.from_bytes(rom[o:o + 4], "little"), v))
        for ea, es, _m, text in eps:
            if ea <= a < ea + es and ea not in have:
                txt += text if text.endswith("\n") else text + "\n"
                have.add(ea)
                print("  entry point %08X+%X added" % (ea, es))
    sha = hashlib.sha1(bytes(patched)).hexdigest()
    txt = re.sub(r'sha1\s*=\s*"[0-9a-f]+"', 'sha1 = "%s"' % sha, txt, count=1)
    open(OUT_BIN, "wb").write(bytes(patched))
    with open(OUT_TOML, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(txt)
    print("wrote %s (sha1 %s) and %s" % (OUT_BIN, sha, OUT_TOML))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["scan", "add"])
    ap.add_argument("--port", type=int, default=19859)
    ap.add_argument("words", nargs="*", help="add: ADDR=WORD (hex)")
    a = ap.parse_args()
    if a.cmd == "add":
        add(a.words)
    else:
        scan(a.port)


if __name__ == "__main__":
    main()
