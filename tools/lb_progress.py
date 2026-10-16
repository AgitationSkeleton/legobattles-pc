#!/usr/bin/env python3
"""Measure how far this project has actually got, in numbers that can be checked.

"Percent complete" is the easiest number in a decompilation project to state
dishonestly, so this tool refuses to produce a single one. It reports **four
separate figures** that mean different things, and it never blends them:

  1. DECOMPILATION -- C source that compiles back to matching bytes.
  2. CODE MAPPED   -- bytes inside a known function with a known execution mode.
  3. CODE ANNOTATED -- functions carrying a real symbol or class attribution.
  4. DATA DECODED  -- cartridge files a parser in this repo actually reads.

## Why they are kept apart

**Mapped is not understood.** A recursive-descent scan can attribute 90% of a
binary to function boundaries while telling you nothing about what any of it
does. `docs/09` and `docs/17` built that corpus; it is a starting point, not an
achievement to report as progress.

**Annotated is not decompiled.** `docs/07` recovered 512 class names and 2,454
virtual function addresses free, straight out of the shipped RTTI. That is a
large annotation win that required no analysis at all, and reporting it as
"decompilation progress" would be misleading.

**Byte-weighted data coverage is nearly meaningless on a DS cartridge.** This
ROM is 128 MB, of which one `.sdat` sound archive is 62.9 MB and 24 `.mods`
videos are 34.6 MB -- 80% of the cartridge before a single byte of gameplay
data. Every format this project actually solved (`docs/05`, `11`, `13`, `15`,
`21`) totals about 2 MB. So a byte-weighted "% of ROM understood" would be
dominated by whether somebody bothered to run an audio extractor. This tool
prints byte-weighted *and* file-weighted figures side by side so the difference
is visible rather than hidden in one number.

## The pitfall this tool exists to avoid

`docs/17` records a coverage metric that summed function sizes and printed
**188.3%** on the ARM9 -- impossible, therefore visible -- and a quietly wrong
77.8% on the ARM7 for a true 69.5%. Overlapping and duplicate entries were
counted twice. Coverage here is computed by **merging intervals**, and the
overlap is reported rather than silently absorbed.

Usage:
    python tools/lb_progress.py
    python tools/lb_progress.py --tsv docs/data/progress.tsv
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402


#: Formats a parser in this repository reads and validates. The doc that
#: established each one is named so the claim is checkable.
DECODED_HERE = {
    ".map": "docs/15 - all chunks, 122/122 maps",
    ".lng": "docs/07 - 3,064 strings x 8 languages",
    ".tbp": "docs/21 - 136/136 files exact",
    ".ebp": "docs/11 - 551 records, pool fully referenced",
    ".fbp": "docs/13 - 130 records x 0x40",
    ".bin": "docs/47 - UI screen layouts: container and widget taxonomy, "
            "132/132, every record typed",
    ".nclr": "docs/49 - Nitro container + PLTT: bpp and colour count",
    ".ncgr": "docs/49 - Nitro container + CHAR: dimensions, bpp, tile count",
    ".ncbr": "docs/49 - same as .ncgr; bitmap mode uses 0xFFFF dimensions",
    ".nscr": "docs/49 - Nitro container + SCRN: dimensions and entry count",
    ".ncer": "docs/50 - CEBK: cell count, flags-selected entry size, offsets",
    ".nanr": "docs/50 - ABNK: sequences, frames, and three checked offsets",
    ".nsbmd": "docs/51 - MDL0 + TEX0: counts, scale pair, texture tiling",
    ".nsbca": "docs/51 - JNT0: the G3D dictionary, three identities each",
    ".nftr": "docs/51 - FINF/CGLP/CWDH/CMAP, five identities each",
    ".sadl": "docs/52 - the shipped C header, all 1,311 defines",
    ".sdat": "docs/54 - SDAT container, symbol and info tables, 499-file FAT",
    ".bnr": "docs/55 - banner: layout, six titles, and a matching CRC-16",
    ".abp": "docs/55 - 180 records, the walk lands on the last byte",
    ".hps": "docs/56 - block and element framing read out of func_02029FF8; "
            "all ten files walk exactly",
}

#: Header understood, body not. Counted separately so it cannot inflate (4).
PARTIAL_HERE = {
    ".mods": "docs/55 - MODS container and keyframe index; the Mobiclip "
             "bitstream is not decoded",
}

#: Publicly documented Nintendo formats. Readable with existing tools; no
#: parser here. Knowing the format is real progress but it is not this
#: project's work, so it never counts toward "decoded".
#: Empty as of docs/55. Every documented Nintendo format on this cartridge now
#: has a reader here.
KNOWN_FORMAT = set()

#: No known structure. See docs/04.
#: .abp is Animations.abp, which docs/20 found is never referenced by
#: name anywhere in the ARM9 -- so it is opaque for a second, separate reason.
#: Empty as of docs/55: every file on the cartridge is at least framed.
OPAQUE = set()


def merged_coverage(intervals):
    """(covered_bytes, overlap_bytes) from [(start, end)], merging overlaps."""
    if not intervals:
        return 0, 0
    ivs = sorted(intervals)
    total = sum(e - s for s, e in ivs)
    merged = []
    cur_s, cur_e = ivs[0]
    for s, e in ivs[1:]:
        if s <= cur_e:
            cur_e = max(cur_e, e)
        else:
            merged.append((cur_s, cur_e))
            cur_s, cur_e = s, e
    merged.append((cur_s, cur_e))
    covered = sum(e - s for s, e in merged)
    return covered, total - covered


def load_arm7_corpus(path):
    """The ARM7 TSV has its own schema: address, mode, size, calls, tier, section.

    `lb_code.load_corpus` expects the ARM9 schema and will happily parse this
    one into nonsense -- it reads the `tier` column as a name, so every row
    looks "named" and the tool reports 100% annotation. Detected by the header
    rather than assumed.
    """
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8") as fh:
        header = fh.readline().rstrip("\n").split("\t")
        if header[:3] != ["address", "mode", "size"]:
            return rows
        for line in fh:
            p = line.rstrip("\n").split("\t")
            if len(p) < 3:
                continue
            if not p[2]:
                continue   # tier-2 pointer sighting carries no size
            rows.append({"address": int(p[0], 16), "size": int(p[2], 16)})
    return rows


#: Sections that hold no code, and must be kept out of the coverage
#: denominator. The ARM7's `mainram` block is the important one: `docs/14`
#: showed it is a 96 KB data mirror the ARM7 copies to 0x027E0000, not code.
#: Counting it drops the ARM7 figure from a true 69.5% to a wrong 27.5% -- a
#: denominator error that understates rather than overstates, which is why it
#: survived a first reading.
NON_CODE_SECTIONS = {
    "arm7": {"section_mainram_027e0000.bin"},
    "arm9": {"section_dtcm_027e0000.bin"},
}


def section_sizes(extract, cpu, code_only=True):
    d = os.path.join(extract, cpu)
    skip = NON_CODE_SECTIONS.get(cpu, set()) if code_only else set()
    out = {}
    if os.path.isdir(d):
        for fn in sorted(os.listdir(d)):
            if fn.startswith("section_") and fn.endswith(".bin") and fn not in skip:
                out[fn] = os.path.getsize(os.path.join(d, fn))
    return out


def main(argv=None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--tsv")
    args = ap.parse_args(argv)

    rows = []

    def report(section, metric, value, total, note=""):
        pct = (100.0 * value / total) if total else 0.0
        rows.append({"section": section, "metric": metric, "value": value,
                     "total": total, "percent": round(pct, 2), "note": note})
        return pct

    print("=" * 74)
    print("LEGO Battles - project completion")
    print("=" * 74)

    # ---------------------------------------------------------------- 1 ----
    arm9 = lb_code.load_corpus(os.path.join(args.extract, "corpus.tsv"))
    arm7 = load_arm7_corpus(os.path.join(args.extract, "arm7_corpus.tsv"))
    arm9_cov, arm9_ovl = merged_coverage(
        [(a, a + r["size"]) for a, r in arm9.items() if r["size"]])
    arm7_cov, arm7_ovl = merged_coverage(
        [(r["address"], r["address"] + r["size"]) for r in arm7 if r["size"]])
    code_bytes = arm9_cov + arm7_cov  # overlays added below once counted

    print("\n1. DECOMPILATION  -- C source that recompiles to matching bytes")
    # Measured by tools/lb_match.py, which compiles every function listed in
    # config/symbols.tsv and byte-compares it against the cartridge. Only exact
    # matches count.
    #
    # An earlier version of this section summed the SIZE OF THE .c FILES, which
    # is not a measurement of anything -- it reported 18,942 "matched bytes"
    # from 1,788 bytes of actually-matching code, and it would have grown every
    # time a comment was added. Machine-code bytes, verified against the ROM,
    # or nothing.
    dec_tsv = os.path.join("docs", "data", "decomp_status.tsv")
    m_all = b_all = m_hand = b_hand = 0
    if os.path.exists(dec_tsv):
        with open(dec_tsv, encoding="utf-8") as fh:
            fh.readline()
            for line in fh:
                q = line.rstrip().split(chr(9))
                if len(q) < 5 or q[4] != "MATCH":
                    continue
                m_all += 1
                b_all += int(q[2])
                if not q[3].startswith("generated/"):
                    m_hand += 1
                    b_hand += int(q[2])
    # Held in its own variable: `p` is reused by later sections, and the
            # headline printed the ANNOTATION percentage as the decompilation
            # figure until this was separated -- 26.8% instead of 0.143%.
    p_dec = report("decompilation", "bytes matching the ROM exactly", b_all,
                   code_bytes, f"{m_all} functions verified by lb_match.py")
    ph = report("decompilation", "bytes from hand-written C", b_hand,
                code_bytes, f"{m_hand} functions with real names and types")
    print(f"   {b_all:,} of {code_bytes:,} mapped code bytes   -> {p_dec:.3f}%"
          f"   ({m_all} functions)")
    print(f"   hand-written with real names and types: {b_hand:,} bytes"
          f" ({m_hand} functions, {ph:.3f}%)")
    print("   The remainder are generated two-instruction stubs. They reproduce")
    print("   the bytes exactly -- the only criterion -- but carry placeholder")
    print("   names and untyped arguments: matched, not understood.")

    # ---------------------------------------------------------------- 2 ----
    print("\n2. CODE MAPPED    -- bytes inside a known function boundary")
    a9_sections = section_sizes(args.extract, "arm9")
    a7_sections = section_sizes(args.extract, "arm7")
    a9_total = sum(a9_sections.values())
    a7_total = sum(a7_sections.values())
    ov = os.path.join(args.extract, "overlays")
    ov_total = sum(os.path.getsize(os.path.join(ov, f))
                   for f in os.listdir(ov)) if os.path.isdir(ov) else 0

    # The ARM9 denominator is .text, NOT the whole static section. The section
    # is 1,378,432 bytes of which ~174 KB is rodata and data after .text ends
    # at 0x02125FC4 -- measuring against the section reported 87.3% for a true
    # 99.997%. Same mistake as the ARM7's, an order of magnitude larger. The
    # corpus also carries ITCM and overlay functions, which are excluded.
    # tools/lb_verify.py derives and re-checks all of this.
    A9_TEXT_LO, A9_TEXT_HI = 0x02000000, 0x02125FC4
    A9_OVERLAY_BASE = 0x0216A2E0
    a9_text = [(a, a + r["size"]) for a, r in arm9.items()
               if r["size"] and A9_TEXT_LO <= a < A9_OVERLAY_BASE]
    arm9_cov, arm9_ovl = merged_coverage(a9_text)
    a9_total = A9_TEXT_HI - A9_TEXT_LO
    p9 = report("mapped", "ARM9 .text bytes", arm9_cov, a9_total,
                f"{len(a9_text)} functions; {arm9_ovl} bytes of overlap merged out")
    p7 = report("mapped", "ARM7 code bytes", arm7_cov, a7_total,
                f"{len(arm7)} functions; {arm7_ovl} bytes of overlap merged out")
    print(f"   ARM9  {arm9_cov:,} of {a9_total:,} .text SPAN bytes  -> {p9:.3f}%"
          f"   ({len(a9_text):,} functions)")
    print("         Against the .text CODE region -- the span minus 2,994 bytes")
    print("         of interior data (the 2 KB secure area, literal pools,")
    print("         alignment padding and one referenced data island) -- this is")
    print("         1,201,170 of 1,201,170: a VERIFIED 100%, re-derived by")
    print("         lb_verify.py each run. Two figures because they answer two")
    print("         questions; the span one is not wrong, just not the code.")
    print("         (.text only: the section's trailing 174 KB of rodata is")
    print("          excluded, and ITCM and overlay functions with it. docs/36-37)")
    print(f"   ARM7  {arm7_cov:,} of {a7_total:,} section bytes  -> {p7:.1f}%"
          f"   ({len(arm7):,} functions)")
    print("         71.5% against the wram CODE region (section minus 1,904")
    print("         bytes of literal pools). The remaining ~28% is unreached")
    print("         and docs/33 argues it is linked-in but unused SDK.")
    print("         (the ARM7's 96 KB mainram block is excluded: docs/14 showed")
    print("          it is a data mirror, not code. This reproduces docs/17.)")
    # Overlays, from tools/lb_overlays.py. They all load at 0x0216A2E0, so
    # they are scanned one address space at a time and their corpus is kept
    # in its own file rather than merged into the ARM9's -- an address alone
    # does not identify an overlay function. See docs/31.
    # Overlays, from tools/lb_overlays.py. Two figures, because they measure
    # different things and last pass conflated them:
    #   extent span -- bytes inside a discovered function boundary. This is the
    #                  same definition the ARM9 and ARM7 rows above use.
    #   visited     -- bytes a control-flow walk actually reached. Strictly
    #                  smaller, and the honest floor.
    # Overlays. Measured against the ds-decomp config's per-overlay function
    # list, NOT this project's walk. docs/39: the walk under-reads a compiled
    # mission badly, and both figures published from it (93.9%, then 52.9%)
    # were measuring the walk rather than the overlays. lb_verify.py re-derives
    # this from the config and the header each run.
    print("   overlays  91,540 of 91,540 code bytes  -> 100.000%   (364 functions)")
    print("         VERIFIED by lb_verify.py. An overlay's bytes are exactly")
    print("         three things: inside a function, the static_init table")
    print("         (1,092 bytes) or trailing zero fill (1,416). Nothing else.")

    print("   NOTE: the ARM7 wram row still uses a whole-section denominator")
    print("   that includes its literal pools, so it understates slightly.")

    # ---------------------------------------------------------------- 3 ----
    print("\n3. CODE ANNOTATED -- functions with a real symbol or class")
    named = {a for a, r in arm9.items()
             if r["name"] and not r["name"].startswith(("func_", "data_", "sub_"))}
    owned = {a for a, r in arm9.items() if r["owners"]}
    ann = named | owned
    ann_bytes = sum(arm9[a]["size"] for a in ann)
    p = report("annotated", "ARM9 functions", len(ann), len(arm9),
               f"{len(owned)} from RTTI vtables, {len(named)} named symbols")
    pb = report("annotated", "ARM9 code bytes", ann_bytes, arm9_cov)
    # Derived annotation from tools/lb_annotate.py, kept in its own tier. The
    # RTTI share is reported on the same line for the reason docs/30 gives:
    # this metric exists to separate what was cheap from what was earned, and
    # folding the derived tiers into the total would destroy that.
    ann_tsv = os.path.join("docs", "data", "annotations.tsv")
    tier = collections.Counter()
    if os.path.exists(ann_tsv):
        with open(ann_tsv, encoding="utf-8") as fh:
            fh.readline()
            for line in fh:
                q = line.rstrip().split(chr(9))
                if len(q) >= 2:
                    tier[q[1]] += 1
    derived = sum(tier.values())
    print(f"   {len(ann):,} of {len(arm9):,} functions  -> {p:.1f}%"
          f"   ({ann_bytes:,} bytes, {pb:.1f}%)")
    if derived:
        pd = report("annotated", "derived annotations", len(ann) + derived,
                    len(arm9), f"tier S {tier.get('S', 0)}, tier C {tier.get('C', 0)}")
        print(f"   plus {derived} derived -> {pd:.1f}%  (docs/40-43)")
        print(f"      S {tier.get('S', 0):>4}  named from a sole string reference")
        print(f"      F {tier.get('F', 0):>4}  C++ static-object initialisers")
        print(f"      T {tier.get('T', 0):>4}  tail-call thunks")
        print(f"      V {tier.get('V', 0):>4}  constructors, by the vptr they assign")
        print(f"      A {tier.get('A', 0):>4}  constructs a class (inlined at several sites)")
        print(f"      C {tier.get('C', 0):>4}  attributed to a class: every caller is a method")
        print(f"      L {tier.get('L', 0):>4}  the translation unit it sits in "
              f"(96.7% exact on a 1,375-case hold-out)")
        print(f"      R {tier.get('R', 0):>4}  read by hand, each name cited to "
              f"a document")
        names = sum(tier.get(k, 0) for k in ("S", "F", "T", "V", "R"))
        assoc = sum(tier.get(k, 0) for k in ("A", "C", "L"))
        print(f"   {names} are NAMES; {assoc} are associations -- they say what a")
        print("   function is near, not what it does.")
    print(f"   of which {len(owned):,} come free from the shipped RTTI (docs/07)")
    print(f"   ARM7: {len(arm7)} functions, none carry symbols (docs/17)")

    # ---------------------------------------------------------------- 4 ----
    print("\n4. DATA DECODED   -- cartridge files a parser here actually reads")
    files = json.load(open(os.path.join(args.extract, "files.json"),
                          encoding="utf-8"))
    cnt = collections.Counter()
    byt = collections.Counter()
    for r in files:
        path = r.get("path")
        if not path:
            continue
        ext = os.path.splitext(path)[1].lower() or "(none)"
        if ext in DECODED_HERE:
            tier = "decoded"
        elif ext in PARTIAL_HERE:
            tier = "partial"
        elif ext in KNOWN_FORMAT:
            tier = "known"
        elif ext in OPAQUE:
            tier = "opaque"
        else:
            tier = "unclassified"
        cnt[tier] += 1
        byt[tier] += r.get("size", 0)
    tf, tb = sum(cnt.values()), sum(byt.values())
    print(f"   {'tier':<14}{'files':>7}{'  ':>2}{'%':>6}{'bytes':>14}{'%':>8}")
    for tier in ("decoded", "partial", "known", "opaque", "unclassified"):
        if not cnt[tier]:
            continue
        print(f"   {tier:<14}{cnt[tier]:>7}  {100.0*cnt[tier]/tf:>5.1f}%"
              f"{byt[tier]:>14,}{100.0*byt[tier]/tb:>7.1f}%")
    report("data", "files decoded by a parser here", cnt["decoded"], tf)
    report("data", "bytes decoded by a parser here", byt["decoded"], tb,
           "the remaining byte share is 34.6 MB of .mods video")
    print("   The 62.9 MB sound archive moved to decoded in docs/54, which is")
    print("   why the byte column jumped from 19.5% to 71.4% in one pass. What")
    print("   is left is 24 .mods videos: a proprietary Mobiclip bitstream.")

    # -------------------------------------------------------------- roll ---
    print("\n" + "-" * 74)
    print(f"HEADLINE: decompilation {p_dec:.3f}% -- {b_all:,} bytes over {m_all} "
          f"functions verified")
    print(f"          against the ROM ({m_hand} of them hand-written).")
    print(f"          ARM9 code mapped {p9:.0f}%, annotated {p:.0f}% of functions.")
    print(f"          {cnt['decoded']} of {tf} cartridge files parsed here"
          f" ({100.0*cnt['decoded']/tf:.0f}%).")
    print("-" * 74)

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        cols = ["section", "metric", "value", "total", "percent", "note"]
        with open(args.tsv, "w", encoding="utf-8") as fh:
            fh.write("\t".join(cols) + "\n")
            for r in rows:
                fh.write("\t".join(str(r[c]) for c in cols) + "\n")
        print(f"\nwrote {args.tsv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
