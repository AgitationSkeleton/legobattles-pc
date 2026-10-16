#!/usr/bin/env python3
"""Compile the hand-written C in src/ and compare it to the ROM, byte for byte.

This is the tool that makes "decompilation %" in `docs/30` a measured number
rather than a permanent zero. Every function listed in `config/symbols.tsv` is
compiled, its bytes extracted, and compared against the same address range in
the cartridge. **Only exact matches count.**

## Why byte equality and not "looks right"

A decompilation is correct when the compiler turns the source back into the
original bytes. Anything weaker -- "equivalent", "close", "same behaviour" --
is an opinion, and this project's practice is that a value is measured or it
is unknown. So the tool reports three states and no others: MATCH, MISMATCH (with
a byte-level diff), or MISSING (the symbol was not produced).

## The compiler caveat, stated up front

LEGO Battles was built with **mwccarm** (Metrowerks), and this tool uses
**devkitARM's GCC**. Those two compilers do not generally agree, so:

  * a MATCH is still conclusive -- the bytes are the bytes, whatever produced
    them, and a function whose source reproduces the ROM exactly is decompiled;
  * a MISMATCH is **not** evidence the C is wrong. It may be right and simply
    compiled differently. Mismatches are reported, and are not counted either
    way.

That asymmetry is why this starts with two- and three-instruction accessors:
where only one encoding is sensible, compiler choice does not enter into it, so
a match is achievable and a mismatch is genuinely informative. Larger functions
will need mwccarm before mismatches mean anything at all.

## Extraction

Sources are compiled with `-ffunction-sections`, which puts each function in
its own `.text.<name>` section. The section's contents are the function's bytes,
with no need to parse a symbol table or guess where one function ends. Any
section carrying a relocation is **skipped and reported**, because a relocated
word's final value depends on the link address and comparing it to the ROM
would be comparing a placeholder.

Usage:
    python tools/lb_match.py
    python tools/lb_match.py --verbose      # show a diff for every mismatch
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

TSV_TAB = chr(9)
TSV_NL = chr(10)

#: DEVKITARM is often set to an MSYS-style path ("/opt/devkitpro/devkitARM")
#: that means nothing to native Python on Windows, so the environment is tried
#: first and a real install location is used as the fallback. Getting this
#: wrong reports "compiler not found" while the compiler is sitting right there.
_CANDIDATE_ROOTS = [
    os.environ.get("DEVKITARM", ""),
    r"C:\devkitPro\devkitARM",
    "/opt/devkitpro/devkitARM",
]


def _find_toolchain():
    for root in _CANDIDATE_ROOTS:
        if not root:
            continue
        cc = os.path.join(root, "bin", "arm-none-eabi-gcc.exe")
        if os.path.exists(cc):
            return root
        cc = os.path.join(root, "bin", "arm-none-eabi-gcc")
        if os.path.exists(cc):
            return root
    return _CANDIDATE_ROOTS[1]


DEVKITARM = _find_toolchain()
_EXE = ".exe" if os.name == "nt" else ""
CC = os.path.join(DEVKITARM, "bin", "arm-none-eabi-gcc" + _EXE)
OBJCOPY = os.path.join(DEVKITARM, "bin", "arm-none-eabi-objcopy" + _EXE)
READELF = os.path.join(DEVKITARM, "bin", "arm-none-eabi-readelf" + _EXE)

#: The ARM9 is an ARM946E-S. `-fno-builtin` stops GCC turning a struct copy
#: into a memcpy call the original does not have; `-ffunction-sections` is what
#: makes per-function extraction exact rather than approximate.
BASE_FLAGS = [
    "-c", "-O2", "-mcpu=arm946e-s", "-fno-builtin", "-fomit-frame-pointer",
    "-ffunction-sections", "-fno-common", "-std=gnu99", "-w",
]


def load_manifest(path):
    rows = []
    with open(path, encoding="utf-8") as fh:
        fh.readline()
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) < 5:
                continue
            rows.append({"symbol": p[0], "address": int(p[1], 16),
                         "size": int(p[2]), "mode": p[3], "source": p[4]})
    return rows


def compile_source(src_root, source, mode, out_dir):
    """Compile one source file. Returns the object path, or raises."""
    obj = os.path.join(out_dir, source.replace("/", "_").replace("\\", "_") + ".o")
    cmd = [CC] + BASE_FLAGS + ["-marm" if mode == "arm" else "-mthumb",
                               os.path.join(src_root, source), "-o", obj]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(res.stderr.strip() or "compile failed")
    return obj


def section_bytes(obj, name, out_dir):
    """Contents of one section, or None if absent."""
    out = os.path.join(out_dir, "sect.bin")
    res = subprocess.run([OBJCOPY, "-O", "binary", "--only-section", name,
                          obj, out], capture_output=True, text=True)
    if res.returncode != 0 or not os.path.exists(out):
        return None
    with open(out, "rb") as fh:
        return fh.read()


def has_relocations(obj, name):
    """True if the section carries relocations, so its bytes are not final."""
    res = subprocess.run([READELF, "-r", obj], capture_output=True, text=True)
    return (".rel" + name) in res.stdout


def main(argv=None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--manifest", default=os.path.join("config", "symbols.tsv"))
    ap.add_argument("--src", default="src")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--tsv", help="write per-symbol results for lb_progress.py")
    args = ap.parse_args(argv)

    if not os.path.exists(CC):
        print(f"compiler not found: {CC}")
        print("set DEVKITARM to the devkitARM install directory")
        return 2

    rows = load_manifest(args.manifest)
    space = lb_code.AddressSpace(args.extract, None, "arm9")
    tmp = tempfile.mkdtemp(prefix="lbmatch")
    objs = {}
    matched = mismatched = missing = 0
    matched_bytes = total_bytes = 0
    results = []
    hand_n = hand_b = 0

    try:
        for r in rows:
            total_bytes += r["size"]
            key = (r["source"], r["mode"])
            if key not in objs:
                try:
                    objs[key] = compile_source(args.src, r["source"], r["mode"], tmp)
                except RuntimeError as e:
                    print(f"  BUILD FAIL  {r['source']}: {e}")
                    objs[key] = None
            obj = objs[key]
            if obj is None:
                missing += 1
                continue

            sect = ".text." + r["symbol"]
            got = section_bytes(obj, sect, tmp)
            if got is None:
                print(f"  MISSING     {r['symbol']}  (no section {sect})")
                missing += 1
                continue
            if has_relocations(obj, sect):
                print(f"  SKIPPED     {r['symbol']}  (section has relocations; "
                      f"its bytes are not final until link)")
                missing += 1
                continue

            want = space.read(r["address"], r["size"])
            got = got[:r["size"]]
            if got == want:
                matched += 1
                matched_bytes += r["size"]
                results.append((r["symbol"], r["address"], r["size"],
                                r["source"], "MATCH"))
                if not r["source"].startswith("generated/"):
                    hand_n += 1
                    hand_b += r["size"]
                print(f"  MATCH       {r['symbol']}  0x{r['address']:08X}  "
                      f"{r['size']}b")
            else:
                mismatched += 1
                results.append((r["symbol"], r["address"], r["size"],
                                r["source"], "MISMATCH"))
                print(f"  MISMATCH    {r['symbol']}  0x{r['address']:08X}")
                if args.verbose or True:
                    print(f"       rom  {' '.join('%02x' % b for b in want)}")
                    print(f"       got  {' '.join('%02x' % b for b in got)}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    print(f"{matched} matched, {mismatched} mismatched, {missing} missing "
          f"of {len(rows)} claimed")
    print(f"{matched_bytes} of {total_bytes} claimed bytes match the ROM exactly")
    print(f"of those, {hand_n} functions ({hand_b} bytes) are HAND-WRITTEN with real")
    print(f"names and types; the rest are generated stubs with placeholder names")
    print(f"and untyped arguments -- matched bytes, not recovered meaning.")
    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(TSV_TAB.join(["symbol", "address", "size", "source",
                                   "result"]) + TSV_NL)
            for sym, addr, size, src, res in results:
                fh.write(TSV_TAB.join([sym, "0x%08X" % addr, str(size), src,
                                       res]) + TSV_NL)
        print(f"wrote {args.tsv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
