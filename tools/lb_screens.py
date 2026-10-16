#!/usr/bin/env python3
"""Which UI screens did a run actually reach?

The port has no way to see its own window, so "the game got to the title
screen" has to be measured rather than eyeballed. Every `Battles::User::*`
screen is a class with a vtable, and the only code that writes a vtable pointer
into an object is that class's constructor. So:

    screen reached  <=>  its constructor executed

The runner's coverage manifest (`--coverage-manifest out.json`) records every
4 KB code page it executed. Locating each screen's constructor and asking
whether its page is in that set gives a yes/no per screen.

**The resolution is a 4 KB page, not a function.** Screens whose constructors
share a page cannot be told apart, and this tool says so rather than implying a
precision it does not have.

    python tools/lb_screens.py <coverage.json> [--rtti rtti.json]

`--rtti` defaults to regenerating from the ARM9 image, which takes a few
seconds.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lb_code  # noqa: E402

# The front-end flow, in the order a player meets it. Reaching a later one
# implies the earlier ones worked.
FLOW = [
    ("SplashScreen", "publisher / title splash"),
    ("StartScreen", "the title screen"),
    ("ProfileSelect", "choose a save profile"),
    ("CreateNewProfile", "NAME ENTRY"),
    ("ProfileCard", "profile summary"),
    ("SkirmishSetup", "skirmish / multiplayer setup"),
    ("StatsScreen", "stats"),
    ("CheatKeyboardScreen", "the game's own cheat-code entry"),
]


def screen_ctors(rtti_path, workdir):
    """Map screen name -> set of constructor addresses."""
    with open(rtti_path, encoding="utf-8") as fh:
        rtti = json.load(fh)
    funcs = lb_code.load_corpus(os.path.join(workdir, "out/eur/corpus.tsv"))
    space = lb_code.AddressSpace(os.path.join(workdir, "out/eur"), None, "arm9")
    base = 0x02000000
    image = space.read(base, 0x00160000)

    def owner(addr):
        for start, row in funcs.items():
            if start <= addr < start + row["size"]:
                return start
        return None

    wanted = {"Battles::User::" + name for name, _ in FLOW}
    out = {}
    for entry in rtti["typeinfos"]:
        if entry["name"] not in wanted:
            continue
        found = set()
        for vtable in entry.get("vtables", []):
            needle = struct.pack("<I", vtable["vtable"])
            at = 0
            while True:
                at = image.find(needle, at)
                if at < 0:
                    break
                who = owner(base + at)
                if who is not None:
                    found.add(who)
                at += 4
        out[entry["name"].split("::")[-1]] = found
    return out


def covered_pages(manifest_path):
    with open(manifest_path, encoding="utf-8") as fh:
        manifest = json.load(fh)
    pages = manifest.get("pages", {})
    out = set()
    for entry in pages.get("entries", []):
        try:
            out.add(int(entry["addr"], 16) & ~0xFFF)
        except (KeyError, ValueError):
            continue
    return out, manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest")
    ap.add_argument("--rtti", default=None)
    ap.add_argument("--workdir", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), ".."))
    args = ap.parse_args()
    workdir = os.path.abspath(args.workdir)

    rtti = args.rtti
    if not rtti:
        rtti = os.path.join(workdir, "out", "eur", "rtti.json")
        if not os.path.isfile(rtti):
            subprocess.run(
                [sys.executable, os.path.join(workdir, "tools", "lb_rtti.py"),
                 os.path.join(workdir, "out/eur/arm9/section_static_02000000.bin"),
                 "--base", "0x02000000", "--code-end", "0x02125fc4",
                 "--json", rtti],
                check=True, capture_output=True)

    ctors = screen_ctors(rtti, workdir)
    pages, manifest = covered_pages(args.manifest)

    print("run: %s pages executed, build %s"
          % (manifest.get("pages", {}).get("captured", "?"),
             manifest.get("build_id", "?")))
    print()

    # Which screens share a page? Say so instead of pretending to resolve them.
    page_owners = {}
    for name, addrs in ctors.items():
        for a in addrs:
            page_owners.setdefault(a & ~0xFFF, set()).add(name)

    reached = 0
    for name, blurb in FLOW:
        addrs = ctors.get(name) or set()
        if not addrs:
            print("  ?    %-20s %-34s (constructor not located)" % (name, blurb))
            continue
        hit = sorted(a for a in addrs if (a & ~0xFFF) in pages)
        shared = set()
        for a in addrs:
            shared |= page_owners.get(a & ~0xFFF, set())
        shared.discard(name)
        note = ""
        if hit and shared:
            note = "  [page shared with %s]" % ", ".join(sorted(shared))
        if hit:
            reached += 1
            print("  YES  %-20s %-34s ctor %08X%s"
                  % (name, blurb, hit[0], note))
        else:
            print("  no   %-20s %-34s" % (name, blurb))

    print()
    print("  %d of %d front-end screens reached" % (reached, len(FLOW)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
