#!/usr/bin/env python3
"""Read an LB_SAMPLE profile (port/runner/src/lb_prof.cpp) and rank the cost.

    python tools/lb_prof.py screenshots/exp-X/samples.bin [--last 6] [--top 40]
                            [--callers runtime_code_cycles] [--exe PATH]

The runner samples its main thread -- the frontend loop, so all of the
emulation and the compositor -- and records the leaf pc plus up to eleven
callers per sample. This script maps each pc to the nearest preceding text
symbol of the exe (nm), then prints

  * SELF: where the thread was when sampled (the leaf), by function;
  * GROUPS: the same leaves folded into what they are -- generated guest code,
    the runtime helpers it calls per instruction, the 2D/3D renderers, the
    port's own code, and everything outside the image (driver, OS);
  * INCLUSIVE: the share of samples with the function anywhere on the stack.

`--last N` keeps only the final N seconds of samples, which is the harness's
fps window (lb_exp.py measures the last ~6 s and then kills the process).
`--callers F` lists the functions directly above F -- e.g. which generated
functions spend the most time inside runtime_code_cycles.

Names are only as good as the symbol table: a function the compiler inlined
is charged to its caller, and a static helper appears under its own name only
if nm lists it.
"""

import argparse
import bisect
import collections
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_EXE = os.path.join(ROOT, "port", "runner", "build", "nds_runner.exe")


def load_samples(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"LBS1":
        sys.exit("%s: not an LB_SAMPLE file" % path)
    frames, = struct.unpack_from("<I", data, 4)
    freq, base = struct.unpack_from("<QQ", data, 8)
    rec = struct.Struct("<Q%dQ" % frames)
    out = []
    for off in range(24, len(data) - rec.size + 1, rec.size):
        vals = rec.unpack_from(data, off)
        out.append((vals[0], [p for p in vals[1:] if p]))
    return freq, base, out


def pe_layout(exe):
    """(preferred ImageBase, SizeOfImage) from the exe ON DISK.

    The copy of the header in memory is no use for this: the loader rewrites
    OptionalHeader.ImageBase to wherever ASLR put the image, so the runner
    can only record its load base, and the rebase to nm's addresses happens
    here."""
    with open(exe, "rb") as fh:
        head = fh.read(4096)
    pe, = struct.unpack_from("<I", head, 0x3C)
    opt = pe + 24
    magic, = struct.unpack_from("<H", head, opt)
    if magic != 0x20B:
        sys.exit("%s: not a PE32+ image" % exe)
    image_base, = struct.unpack_from("<Q", head, opt + 24)
    size_of_image, = struct.unpack_from("<I", head, opt + 56)
    return image_base, size_of_image


def load_symbols(exe):
    txt = subprocess.run(["nm", "-C", "--defined-only", exe],
                         capture_output=True, text=True, errors="replace").stdout
    syms = []
    for line in txt.splitlines():
        parts = line.split(" ", 2)
        if len(parts) != 3 or parts[1] not in "Tt":
            continue
        try:
            syms.append((int(parts[0], 16), parts[2]))
        except ValueError:
            continue
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def group_of(name):
    if name.startswith("?"):
        return "outside the image (driver/OS)"
    if name.startswith(("lb_arm9_", "lb_arm7_", "fw_", "bios")):
        return "generated guest code"
    if name.startswith(("runtime_", "arm_", "arm7_", "arm9_", "bus_",
                        "nds_busf", "cp15")):
        return "runtime helpers"
    if "tier3" in name or name.startswith("armv4t"):
        return "tier3 interpreter"
    if "gpu2d" in name or "GPU2D" in name or "Unit::" in name or any(
            k in name for k in ("decode_text_line", "decode_bg_line",
                                "decode_extended_line", "render_engine_line",
                                "render_obj_line", "::compose(")):
        return "gpu2d (native scanlines)"
    if "lb_adaptive_fb" in name:
        return "compositor (lb_adaptive_fb)"
    if "gpu3d" in name or "GPU3D" in name or "SoftRenderer" in name \
            or "melonDS" in name:
        return "gpu3d / melonDS"
    if name.startswith(("lb_", "(anonymous namespace)::lb_")) or "lb_" in name:
        return "port (lb_*)"
    if "sched" in name:
        return "scheduler"
    return "other runner"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--last", type=float, default=0.0)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--callers", action="append", default=[])
    # A lifted guest call reaches its callee through runtime_dispatch, so the
    # direct caller of every lb_arm9_* function is the dispatcher. --skip
    # steps over frames whose name starts with PREFIX when naming a caller:
    # `--skip runtime_ --skip "(anonymous namespace)::run_slice"` shows the
    # guest function that made the call.
    ap.add_argument("--skip", action="append", default=[],
                    help="caller-name prefix to step over (repeatable)")
    args = ap.parse_args()

    freq, load_base, recs = load_samples(args.samples)
    if not recs:
        sys.exit("no samples")
    pref, size = pe_layout(args.exe)
    if load_base != pref:
        recs = [(q, [p - load_base + pref if load_base <= p < load_base + size
                     else p for p in pcs]) for q, pcs in recs]
    if args.last > 0:
        end = recs[-1][0]
        recs = [r for r in recs if end - r[0] <= args.last * freq]
    addrs, names = load_symbols(args.exe)

    cache = {}

    def name_of(pc):
        n = cache.get(pc)
        if n is None:
            i = bisect.bisect_right(addrs, pc) - 1
            n = names[i] if i >= 0 and pc - addrs[i] < 0x400000 else "?%x" % pc
            if n.startswith("?") is False and pc < addrs[0]:
                n = "?%x" % pc
            cache[pc] = n
        return n

    total = len(recs)
    span = (recs[-1][0] - recs[0][0]) / float(freq)
    print("%d samples over %.1f s" % (total, span))

    self_c = collections.Counter()
    group_c = collections.Counter()
    incl_c = collections.Counter()
    callers = {c: collections.Counter() for c in args.callers}
    for _qpc, pcs in recs:
        if not pcs:
            continue
        stack = [name_of(p) for p in pcs]
        leaf = stack[0]
        self_c[leaf] += 1
        group_c[group_of(leaf)] += 1
        for n in set(stack):
            incl_c[n] += 1
        for c in args.callers:
            for i, n in enumerate(stack):
                if n == c:
                    j = i + 1
                    while j < len(stack) and any(
                            stack[j].startswith(s) for s in args.skip):
                        j += 1
                    callers[c][stack[j] if j < len(stack) else "?"] += 1
                    break

    print("\nGROUPS (leaf)")
    for g, n in group_c.most_common():
        print("  %6.2f%%  %s" % (100.0 * n / total, g))
    print("\nSELF (leaf), top %d" % args.top)
    for f, n in self_c.most_common(args.top):
        print("  %6.2f%%  %s" % (100.0 * n / total, f[:110]))
    print("\nINCLUSIVE, top %d" % args.top)
    for f, n in incl_c.most_common(args.top):
        print("  %6.2f%%  %s" % (100.0 * n / total, f[:110]))
    for c, cc in callers.items():
        print("\nCALLERS of %s" % c)
        tot = sum(cc.values()) or 1
        for f, n in cc.most_common(args.top):
            print("  %6.2f%%  %s" % (100.0 * n / tot, f[:110]))


if __name__ == "__main__":
    main()
