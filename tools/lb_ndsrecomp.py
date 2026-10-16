"""Emit ndsrecomp program configs for LEGO Battles from this project's corpus.

WHY. ndsrecomp lifts guest ARM to C ahead of time. Per program it needs a TOML
naming the image and **every function entry point with its instruction set**:

    [program]
    cpu = "arm9"  isa = "armv5te"  load_address = ...  size = ...  entry_pc = ...
    [identity]
    sha1 = ...
    [[entry_point]]
    addr = 0x...  mode = "arm"  name = "..."

THE MODE IS THE WHOLE PROBLEM, AND THIS PROJECT HAS ALREADY SOLVED IT. The
sibling LSW2 generator says so plainly in its own docstring: its corpus kept
only addresses, so it had to guess ARM vs Thumb from byte patterns and label the
result a heuristic. A wrongly-lifted function is silent and total.

LEGO Battles is not in that position. `out/eur/corpus.tsv` carries a mode for
every one of its 9,314 ARM9 functions, taken from the ds-decomp configuration
and then cross-checked two ways: `docs/74` showed the vtable low bit agrees with
the recorded mode for every job slot, and `docs/80`/`docs/83` verified the whole
call graph against it by control flow. So every `mode =` this tool writes is
read, not inferred: port code comes from a strict reading, never a guess.

WHAT IT EMITS. One TOML per program:

    lb_arm9.toml          the ARM9 static image, 9,314 entry points
    lb_arm7_static.toml   the ARM7 static image
    lb_arm7_wram.toml     the ARM7 WRAM autoload
    lb_ov###.toml         one per overlay - all 91 load at the SAME address

plus `game.toml` for the title itself.

THE OVERLAYS ALL LOAD AT 0x0216A2E0 (`docs/39`). That is unusual and it matters
here: they are alternatives, not a contiguous bank, so each needs its own
program config and they must never be lifted into one address space together.

WHAT IT DOES NOT EMIT. The ARM7 `mainram` section at 0x027E0000 (96,376 bytes,
autoload entry 1). `docs/83` found 40 calls into it that survive control flow,
and the corpus does not cover it, so there are no entry points to write. It is
reported as a gap rather than emitted empty.

Usage:
    python tools/lb_ndsrecomp.py --out ../port/config
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lb_code

OVERLAY_BASE = 0x0216A2E0        # docs/39: every overlay loads here


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def toml_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def emit(path, header_lines, entries):
    """Write one program TOML. `entries` is [(addr, mode, size, name)].

    The size is REQUIRED. `[program].authoritative_entry_points` refuses an
    entry without an exact one, and authoritative is the whole point: without
    it the recompiler runs its own speculative finder on top of these seeds and
    turns 9,156 verified functions into 68,489 guessed ones, decoding string
    data as ARM instructions along the way. The corpus has a size for every
    function, so there is no reason to accept that.
    """
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for line in header_lines:
            fh.write(line + "\n")
        fh.write("\n")
        for addr, mode, size, name in entries:
            fh.write("[[entry_point]]\n")
            fh.write("addr = 0x%08X\n" % addr)
            fh.write("size = 0x%X\n" % size)
            fh.write('mode = "%s"\n' % mode)
            if name:
                fh.write('name = "%s"\n' % toml_escape(name))
            fh.write("\n")
    return len(entries)


def names_from_config(root):
    """address -> name, from the project's own symbol and hand-read tables."""
    out = {}
    for rel in ("config/symbols.tsv", "config/read_functions.tsv"):
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            continue
        with open(p, encoding="utf-8", newline="") as fh:
            for row in csv.reader(fh, delimiter="\t"):
                if row and row[0].startswith("0x") and len(row) > 1 and row[1]:
                    out.setdefault(int(row[0], 16), row[1])
    return out


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=os.path.join("out", "eur"))
    ap.add_argument("--out", default=os.path.join("port", "config"))
    ap.add_argument("--root", default=".")
    args = ap.parse_args(argv)

    os.makedirs(args.out, exist_ok=True)
    hdr = json.load(open(os.path.join(args.extract, "header.json"),
                         encoding="utf-8"))
    names = names_from_config(args.root)
    written = []

    # ---------------------------------------------------------------- arm9 --
    a9 = hdr["arm9"]
    funcs = lb_code.load_corpus(os.path.join(args.extract, "corpus.tsv"))
    static9 = next(s for s in a9["sections"] if s["name"] == "static")
    # The ARM9 corpus is NOT all ARM9-static. Validating the first generated
    # config against the section table found 158 entry points outside it:
    #   * 6 in ITCM at 0x01FF8000, a real ARM9 section this tool had skipped;
    #   * 152 at 0x0216A000+, which is the OVERLAY load region. Those are
    #     overlay-resident functions carried in the ARM9 corpus, and lifting
    #     them into the static program would place code at an address 91
    #     different overlays share. The overlay configs already cover them from
    #     the overlay corpus, so they are routed out here rather than dropped.
    lo9 = a9["ram_address"]
    hi9 = lo9 + static9["size"]
    itcm = next((s for s in a9["sections"] if s["name"] == "itcm"), None)

    entries, itcm_ent, ov_resident = [], [], []
    for addr in sorted(funcs):
        rec = funcs[addr]
        row = (addr, rec["mode"], rec["size"], names.get(addr))
        if lo9 <= addr < hi9:
            entries.append(row)
        elif itcm and itcm["ram_address"] <= addr < itcm["ram_address"] + itcm["size"]:
            itcm_ent.append(row)
        elif addr >= OVERLAY_BASE - 0x2E0:      # the overlay page
            ov_resident.append(row)
        else:
            entries.append(row)                 # unclassified: keep and report
    modes = {}
    for _a, m, _s, _n in entries:
        modes[m] = modes.get(m, 0) + 1

    path = os.path.join(args.out, "lb_arm9.toml")
    head = [
        "# AUTO-GENERATED by tools/lb_ndsrecomp.py. Do not hand-edit.",
        "# Every mode below is READ from the corpus, not inferred from bytes:",
        "# see docs/74, docs/80 and docs/83 for how the modes were verified.",
        "",
        "[program]",
        'name         = "LEGO Battles (EUR) ARM9 static"',
        'id           = "lb_arm9"',
        'cpu          = "arm9"',
        'isa          = "armv5te"',
        "load_address = 0x%08X" % a9["ram_address"],
        "size         = 0x%08X" % static9["size"],
        "entry_pc     = 0x%08X" % a9["entry"],
        "# Every entry point below is verified, so the recompiler must NOT run",
        "# its own speculative finder on top of them.",
        "authoritative_entry_points = true",
        "",
        "[identity]",
        # The static SECTION's hash, not the unpacked image's: load_address and
        # size above describe the section, so the identity must match it. The
        # recompiler rejected the mismatch on the first run, which is exactly
        # what that check is for.
        'sha1 = "%s"' % static9["sha1"],
    ]
    n = emit(path, head, entries)
    written.append((os.path.basename(path), n, modes))

    if itcm_ent:
        m = {}
        for _a, mm, _s, _n in itcm_ent:
            m[mm] = m.get(mm, 0) + 1
        path = os.path.join(args.out, "lb_arm9_itcm.toml")
        head = [
            "# AUTO-GENERATED by tools/lb_ndsrecomp.py. Do not hand-edit.",
            "# ITCM is a separate ARM9 section with its own load address.",
            "",
            "[program]",
            'name         = "LEGO Battles (EUR) ARM9 ITCM"',
            'id           = "lb_arm9_itcm"',
            'cpu          = "arm9"',
            'isa          = "armv5te"',
            "load_address = 0x%08X" % itcm["ram_address"],
            "size         = 0x%08X" % itcm["size"],
            "entry_pc     = 0x%08X" % itcm["ram_address"],
            "authoritative_entry_points = true",
            "",
            "[identity]",
            'sha1 = "%s"' % itcm["sha1"],
        ]
        written.append((os.path.basename(path), emit(path, head, itcm_ent), m))

    # ---------------------------------------------------------------- arm7 --
    a7 = hdr["arm7"]
    a7rows = []
    p7 = os.path.join(args.extract, "arm7_corpus.tsv")
    if os.path.exists(p7):
        with open(p7, encoding="utf-8", newline="") as fh:
            a7rows = [r for r in csv.DictReader(fh, delimiter="\t")
                      if r.get("size")]

    for sec in a7["sections"]:
        # mainram was skipped while the corpus had no entry points there.
        # docs/92: lb_scan's EXEC_SECTIONS excluded the section by name, so its
        # seeds were dropped silently. With that fixed the scan finds 153
        # functions covering 41.9% of it, and the ARM9's IPCSYNC wait needs
        # them, so it is emitted like any other program.
        rows = [r for r in a7rows if r.get("section") == sec["name"]]
        if not rows:
            continue
        ent = []
        for r in rows:
            a = int(r["address"], 16)
            ent.append((a, r["mode"], int(r["size"], 16), names.get(a)))
        ent.sort()
        m = {}
        for _a, mm, _s, _n in ent:
            m[mm] = m.get(mm, 0) + 1
        path = os.path.join(args.out, "lb_arm7_%s.toml" % sec["name"])
        head = [
            "# AUTO-GENERATED by tools/lb_ndsrecomp.py. Do not hand-edit.",
            "",
            "[program]",
            'name         = "LEGO Battles (EUR) ARM7 %s"' % sec["name"],
            'id           = "lb_arm7_%s"' % sec["name"],
            'cpu          = "arm7"',
            'isa          = "armv4t"',
            "load_address = 0x%08X" % sec["ram_address"],
            "size         = 0x%08X" % sec["size"],
            "entry_pc     = 0x%08X" % (a7["entry"]
                                       if sec["name"] == "static"
                                       else sec["ram_address"]),
            "authoritative_entry_points = true",
            "",
            "[identity]",
            'sha1 = "%s"' % sec["sha1"],
        ]
        written.append((os.path.basename(path), emit(path, head, ent), m))

    # ------------------------------------------------------------ overlays --
    ovdir = os.path.join(args.extract, "overlays")
    ovcorpus = os.path.join("docs", "data", "overlay_corpus.tsv")
    byov = {}
    if os.path.exists(ovcorpus):
        with open(ovcorpus, encoding="utf-8", newline="") as fh:
            for r in csv.DictReader(fh, delimiter="\t"):
                if not (r.get("overlay") or "").strip().isdigit():
                    continue
                byov.setdefault(int(r["overlay"]), []).append(
                    (int(r["address"], 16), r["mode"], int(r["size"], 16)))

    ov_written = 0
    ov_entries = 0
    if os.path.isdir(ovdir):
        for fn in sorted(os.listdir(ovdir)):
            if not fn.endswith(".bin"):
                continue
            digits = "".join(c for c in fn if c.isdigit())
            if not digits:
                continue
            idx = int(digits)
            with open(os.path.join(ovdir, fn), "rb") as fh:
                data = fh.read()
            ent = sorted({(a, m, sz) for a, m, sz in byov.get(idx, [])})
            ent = [(a, m, sz, names.get(a)) for a, m, sz in ent]
            path = os.path.join(args.out, "lb_ov%03d.toml" % idx)
            head = [
                "# AUTO-GENERATED by tools/lb_ndsrecomp.py. Do not hand-edit.",
                "# All %d overlays load at 0x%08X (docs/39). They are"
                % (len(os.listdir(ovdir)), OVERLAY_BASE),
                "# alternatives, never resident together.",
                "",
                "[program]",
                'name         = "LEGO Battles (EUR) overlay %d"' % idx,
                'id           = "lb_ov%03d"' % idx,
                'cpu          = "arm9"',
                'isa          = "armv5te"',
                "load_address = 0x%08X" % OVERLAY_BASE,
                "size         = 0x%08X" % len(data),
                "entry_pc     = 0x%08X" % OVERLAY_BASE,
                "authoritative_entry_points = true",
                "",
                "[identity]",
                'sha1 = "%s"' % sha1(data),
            ]
            ov_entries += emit(path, head, ent)
            ov_written += 1

    # -------------------------------------------------------------- report --
    print("wrote to %s\n" % os.path.abspath(args.out))
    for name, n, m in written:
        breakdown = ", ".join("%s %d" % (k, v) for k, v in sorted(m.items()))
        print("  %-24s %6d entry points   (%s)" % (name, n, breakdown))
    print("  %-24s %6d entry points across %d files"
          % ("lb_ov###.toml", ov_entries, ov_written))

    if ov_resident:
        print("")
        print("  ROUTED OUT of lb_arm9.toml: %d entry points at or above"
              % len(ov_resident))
        print("  0x%08X. That is the overlay load page; these are overlay-"
              % (OVERLAY_BASE - 0x2E0))
        print("  resident functions carried in the ARM9 corpus, and the")
        print("  overlay configs cover them. Lifting them as ARM9 static would")
        print("  put code at an address 91 different overlays share.")

    total = sum(n for _f, n, _m in written) + ov_entries
    print("\n  %d entry points in %d program configs"
          % (total, len(written) + ov_written))

    mainram = next((s for s in hdr["arm7"]["sections"]
                    if s["name"] == "mainram"), None)
    if mainram and not any("mainram" in f for f, _n, _m in written):
        print("\n  NOT EMITTED: ARM7 mainram at 0x%08X, %d bytes."
              % (mainram["ram_address"], mainram["size"]))
        print("  The corpus has no entry points there. docs/83 found 40 calls")
        print("  into it that survive control flow, so it is real code and a")
        print("  real gap - reported rather than emitted empty.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
