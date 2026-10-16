#!/usr/bin/env python3
"""Merge every source of function knowledge into one seed corpus.

Two independent sources exist for the ARM9's functions, and neither is complete:

* the third-party `ds-decomp` configuration — 9,158 static-module functions and
  364 across the overlays, each with an address, a size and an ARM/Thumb mode,
  produced by that project's delinking analysis;
* this project's RTTI walk (`tools/lb_rtti.py`) — 2,466 distinct virtual
  function addresses, each attributed to a class and a vtable slot, with the
  ARM/Thumb bit taken from the pointer itself.

Merging them does two jobs. It produces the seed list Ghidra and a static
recompiler both want, and — more valuable early — it **cross-checks two
derivations that share no method**. Where they disagree about an instruction
set, one of them is wrong, and a function lifted in the wrong instruction set is
not subtly wrong but entirely wrong.

Usage:
    python tools/build_corpus.py --dsd <config/eur/arm9> --rtti out/eur/rtti.json
                                 [--out out/eur/corpus.tsv]
"""

import argparse
import json
import os
import re
import sys

SYM = re.compile(r"^(\S+)\s+kind:function\((arm|thumb),size=(0x[0-9a-fA-F]+)\)\s+addr:(0x[0-9a-fA-F]+)")


def read_dsd(root):
    """Read every symbols.txt under a dsd arm9 config directory."""
    out = {}
    for dirpath, _dirs, names in os.walk(root):
        if "symbols.txt" not in names:
            continue
        module = os.path.relpath(dirpath, root).replace(os.sep, "/")
        if module == ".":
            module = "main"
        with open(os.path.join(dirpath, "symbols.txt"), encoding="utf-8") as fh:
            for line in fh:
                m = SYM.match(line.strip())
                if not m:
                    continue
                name, mode, size, addr = m.groups()
                out[int(addr, 16)] = {
                    "name": name,
                    "mode": mode,
                    "size": int(size, 16),
                    "module": module,
                    "named": not name.startswith(("func_", "data_")),
                }
    return out


def read_rtti(path):
    """Virtual function addresses, attributed to class and slot."""
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    out = {}
    for t in data["typeinfos"]:
        for v in t.get("vtables", []):
            for i, slot in enumerate(v["slots"]):
                if slot == 0:
                    # A pure virtual: this build stores 0, not a pointer to
                    # __cxa_pure_virtual. There is no function to attribute.
                    continue
                addr = slot & ~1
                rec = out.setdefault(addr, {
                    "mode": "thumb" if slot & 1 else "arm",
                    "owners": [],
                })
                rec["owners"].append("%s::virtual_%02d" % (t["name"], i))
    return out


def read_extra(path):
    """Entry points this project verified that the dsd config does not carry.

    A third source, and the only one whose rows were established by hand. It
    exists because the config's function list is not complete: `docs/37` found
    two **alternate entry points** -- runs of the ARM XOR-swap idiom that fall
    through into the function recorded at the next address -- which no source
    listed, and which were the last unexplained bytes in the ARM9's `.text`.

    Rows carry an `evidence` column. A claim without one does not belong here,
    because this file is the one place a wrong entry would be indistinguishable
    from a derived fact.
    """
    out = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as fh:
        fh.readline()
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) < 4:
                continue
            out[int(p[0], 16)] = {"mode": p[1], "size": int(p[2], 16),
                                  "name": p[3], "named": True,
                                  "evidence": p[4] if len(p) > 4 else ""}
    return out


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dsd", required=True)
    ap.add_argument("--rtti", required=True)
    ap.add_argument("--out")
    ap.add_argument("--extra", default=os.path.join("config", "extra_functions.tsv"),
                    help="hand-verified entry points the dsd config lacks")
    args = ap.parse_args()

    dsd = read_dsd(args.dsd)
    rtti = read_rtti(args.rtti)
    extra = read_extra(args.extra)
    if extra:
        print("extra functions: %d verified by hand (%s)"
              % (len(extra), args.extra))
        for a in sorted(extra):
            if a in dsd:
                print("   0x%08x already in dsd -- extra row ignored" % a)
        dsd.update({a: v for a, v in extra.items() if a not in dsd})
    print("dsd functions:  %d  (%d with a meaningful name)"
          % (len(dsd), sum(1 for v in dsd.values() if v["named"])))
    print("rtti functions: %d  (%d vtable references)"
          % (len(rtti), sum(len(v["owners"]) for v in rtti.values())))

    both = set(dsd) & set(rtti)
    only_rtti = set(rtti) - set(dsd)
    print()
    print("in both sources:        %d" % len(both))
    print("known only to the RTTI: %d" % len(only_rtti))
    print("known only to dsd:      %d" % (len(dsd) - len(both)))

    disagree = [a for a in both if dsd[a]["mode"] != rtti[a]["mode"]]
    print()
    print("ARM/Thumb agreement on the %d shared addresses: %d agree, %d DISAGREE"
          % (len(both), len(both) - len(disagree), len(disagree)))
    for a in sorted(disagree)[:20]:
        print("   0x%08x  dsd=%-5s rtti=%-5s  %s"
              % (a, dsd[a]["mode"], rtti[a]["mode"], rtti[a]["owners"][0]))

    # A vtable slot that dsd does not know about is either a function dsd's
    # analysis missed, or an address this walk misread. Report the split by
    # whether it lands inside a dsd function's extent.
    extents = sorted((a, a + v["size"]) for a, v in dsd.items() if v["size"])
    interior = 0
    for a in only_rtti:
        lo, hi = 0, len(extents)
        while lo < hi:
            mid = (lo + hi) // 2
            if extents[mid][0] <= a:
                lo = mid + 1
            else:
                hi = mid
        if lo and extents[lo - 1][1] > a:
            interior += 1
    print()
    print("of the %d RTTI-only addresses, %d fall inside a known dsd function "
          "(mid-function, so not new entry points) and %d are outside every "
          "one (genuinely new)" % (len(only_rtti), interior,
                                   len(only_rtti) - interior))

    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write("address\tmode\tsize\tsources\tname\towners\n")
            for a in sorted(set(dsd) | set(rtti)):
                d = dsd.get(a)
                r = rtti.get(a)
                sources = "+".join(s for s, present in
                                   (("dsd", d), ("rtti", r)) if present)
                if a in extra:
                    sources = sources.replace("dsd", "verified")
                fh.write("0x%08x\t%s\t%s\t%s\t%s\t%s\n" % (
                    a,
                    (d or r)["mode"],
                    "0x%x" % d["size"] if d else "",
                    sources,
                    d["name"] if d else "",
                    ";".join(r["owners"]) if r else "",
                ))
        print()
        print("wrote %s (%d entries)" % (args.out, len(set(dsd) | set(rtti))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
