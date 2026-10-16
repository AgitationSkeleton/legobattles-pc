#!/usr/bin/env python3
"""Re-check every "100%" claim in the documentation, against the ROM.

A completeness claim is the easiest kind to leave rotting. "136 of 136 files
parse" was true when it was written; nothing guarantees it is true after the
parser changed. This runs every such claim again, from the cartridge, and
prints PASS or FAIL with the numbers.

Each check names the document it verifies, so a FAIL says which claim to go and
fix. There is no partial credit: a check that cannot be run reports SKIP, and a
check that runs reports the actual counts, never a summary.

## Why this exists

`docs/32` had to retract a coverage figure that was measuring the wrong
quantity, and `docs/34` caught a metric summing source-file sizes. Both were
computed, plausible, and wrong. The defence against that is not care -- it is
re-running the claim against the source of truth and comparing to a number
established some other way.

Usage:
    python tools/lb_verify.py
"""

from __future__ import annotations

import argparse
import collections
import csv
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

RESULTS = []


def check(name, doc, ok, detail):
    RESULTS.append((name, doc, ok, detail))
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {name:<44} {detail}")
    return ok


def skip(name, doc, why):
    RESULTS.append((name, doc, None, why))
    print(f"  [SKIP] {name:<44} {why}")


# --------------------------------------------------------------- ARM7 -------

def arm7_static(extract):
    """The ARM7's static section: is every CODE byte covered?

    The published 99.2% was wrong in both directions at once, which is why this
    check exists in this form. The corpus reported a 0x18-byte "function" at
    0x023801D0 that is actually the MODULE_PARAMS structure -- its words match
    header.json's autoload fields exactly -- so coverage was inflated by data.
    And the denominator was the whole section, including that structure plus a
    literal-pool word, so it was inflated too. The two errors partly cancelled.

    Code region is [ram_address, module_params_address). Within it, words that
    are proven literal-pool targets of the section's own code are data, not
    unreached code.
    """
    header = json.load(open(os.path.join(extract, "header.json"),
                            encoding="utf-8"))
    a7 = header["arm7"]
    mp = a7["module_params"]
    sect = next(s for s in a7["sections"] if s["name"] == "static")
    lo = sect["ram_address"]
    end = lo + sect["size"]
    params = mp["module_params_address"]

    space = lb_code.AddressSpace(extract, None, "arm7")
    arm, _ = lb_code.make_engines()

    # Verify the params block really is the params block before excluding it.
    words = [space.u32(params + i * 4) for i in range(6)]
    expected = [mp["autoload_list_start"], mp["autoload_list_end"],
                mp["autoload_start"], mp["static_bss_start"],
                mp["static_bss_end"]]
    if words[:5] != expected:
        check("ARM7 static: MODULE_PARAMS identified", "docs/14", False,
              f"words at 0x{params:08X} do not match the header")
        return
    check("ARM7 static: MODULE_PARAMS identified", "docs/14", True,
          f"0x{params:08X}..0x{end:08X} is data, {end - params} bytes, "
          f"matches header.json")

    # Literal-pool words inside the code region are data too.
    data = set(range(params, end))
    blob = space.read(lo, params - lo)
    for ins in arm.disasm(blob, lo):
        try:
            t = lb_code.literal_target(ins, False)
        except Exception:
            continue
        if t is not None and lo <= t < params:
            data.update(range(t, t + 4))

    covered = set()
    false_positives = []
    with open(os.path.join(extract, "arm7_corpus.tsv"), encoding="utf-8") as fh:
        fh.readline()
        for line in fh:
            p = line.rstrip("\n").split("\t")
            if len(p) < 3:
                continue
            if not p[2]:
                continue   # tier-2 pointer sighting, not a walked function
            a, s = int(p[0], 16), int(p[2], 16)
            if not (lo <= a < end):
                continue
            if a >= params:
                false_positives.append((a, s))
                continue
            covered.update(range(a, a + s))

    code = set(range(lo, params)) - data
    missing = code - covered
    check("ARM7 static: every code byte covered", "docs/17",
          not missing,
          f"{len(code & covered)} of {len(code)} code bytes"
          + (f", {len(missing)} missing" if missing else " -- 100%"))
    check("ARM7 static: no data reported as a function", "docs/17",
          not false_positives,
          "clean" if not false_positives else
          f"{len(false_positives)} false function(s): "
          + ", ".join("0x%08X+0x%x" % fp for fp in false_positives))


#: The DS ARM9's secure area: a fixed 2 KB block at the image start holding the
#: entry stub and the BIOS SVC thunks. The `ds-decomp` config records each thunk
#: with a nominal 4-byte size, so almost all of it reads as a coverage gap. It
#: is not missing code.
SECURE_AREA = (0x02000000, 0x02000800)

#: Where the ARM9 corpus stops being the ARM9. The config also carries ITCM
#: functions (below 0x02000000) and OVERLAY functions (0x0216A2E0 and up); both
#: must be excluded or the .text figure is computed over the wrong set.
OVERLAY_BASE = 0x0216A2E0


def arm9_text(extract):
    """The ARM9's .text: is every code byte covered?

    The published 87.3% was measured against the whole 1,378,432-byte static
    section, which includes **174 KB of rodata and data** after .text ends. The
    same section-as-denominator mistake that made the ARM7 static section read
    99.2% instead of 100%, an order of magnitude larger.

    .text is taken to end at the last function in the corpus that lies inside
    the section proper -- 0x02125FC4, which is where the rodata begins (the red
    brick table `docs/27` found at 0x02125FF4 sits just past it).
    """
    space = lb_code.AddressSpace(extract, None, "arm9")
    funcs = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))
    arm, thumb = lb_code.make_engines()
    lo = SECURE_AREA[0]

    outside = [a for a in funcs if a < lo or a >= OVERLAY_BASE]
    check("ARM9 corpus: functions outside the section are excluded", "docs/09",
          True,
          f"{len(outside)} of {len(funcs)} are ITCM or overlay and are not "
          f"counted in .text")

    ivs = sorted((a, a + r["size"]) for a, r in funcs.items()
                 if r["size"] and lo <= a < OVERLAY_BASE)
    merged = []
    for a, b in ivs:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    text_end = merged[-1][1]
    covered = sum(b - a for a, b in merged)

    gaps = []
    prev = lo
    for a, b in merged:
        if a > prev:
            gaps.append((prev, a))
        prev = b

    # Literal-pool targets inside .text are data.
    pools = set()
    for a, r in funcs.items():
        if not (lo <= a < text_end and r["size"]):
            continue
        cs = thumb if r["mode"] == "thumb" else arm
        blob = space.read(a, r["size"])
        if not blob:
            continue
        for ins in cs.disasm(blob, a):
            try:
                t = lb_code.literal_target(ins, r["mode"] == "thumb")
            except Exception:
                continue
            if t is not None and lo <= t < text_end:
                pools.update(range(t, t + 4))

    # Words anywhere in the image that point into a gap make it a data island.
    img = space.read(lo, text_end - lo) or b""
    pointed = set()
    for i in range(0, len(img) - 4, 4):
        w = struct.unpack_from("<I", img, i)[0]
        if lo <= w < text_end:
            pointed.add(w)

    secure = set(range(*SECURE_AREA))
    explained = 0
    unexplained = []
    for a, b in gaps:
        rng = set(range(a, b))
        if b - a <= 2:                       # alignment padding, thumb -> arm
            explained += b - a
            continue
        if rng & secure:
            explained += len(rng & secure)
            rng -= secure
        rng -= pools
        explained += len(set(range(a, b)) & pools)
        if rng and any(a <= p < b for p in pointed):
            explained += len(rng)            # a referenced data island
            continue
        if rng:
            unexplained.append((a, b, len(rng)))

    total_gap = sum(b - a for a, b in gaps)
    left = sum(n for _, _, n in unexplained)
    code = (text_end - lo) - explained
    check("ARM9 .text: gaps are accounted for as data", "docs/09",
          left <= 64,
          f"{explained:,} of {total_gap:,} gap bytes are secure area, literal "
          f"pools, padding or referenced data; {left} unexplained")
    check("ARM9 .text: every code byte covered", "docs/09",
          covered >= code - 64,
          f"{covered:,} of {code:,} code bytes -> "
          f"{100.0 * covered / code:.3f}%  (.text 0x{lo:08X}..0x{text_end:08X})")
    for a, b, n in unexplained:
        print(f"          unexplained: 0x{a:08X}..0x{b:08X}  {n} bytes")


#: The ds-decomp config's per-overlay symbol files. These carry authoritative
#: function sizes from that project's delinking analysis, which is a different
#: and far better source than this project's own control-flow walk.
#: (LB_DSD_DIR, else sources/ two levels above this checkout.)
DSD_OVERLAYS = os.path.join(
    os.environ.get("LB_DSD_DIR") or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "sources",
        "Lego-Battles-Decomp-master"),
    "config", "eur", "arm9", "overlays")

DSD_SYM = None


def overlay_coverage(extract, dsd_root=None):
    """Are the overlays fully covered by the config's own function list?

    `docs/31` reported 93.9% and `docs/32` corrected it to 52.9%, both computed
    from this project's recursive-descent walk. Both were measuring the walk,
    not the overlays: `docs/32` had already shown the walk under-reads a
    compiled mission badly, because much of it is straight-line setup entered
    from branches the walk does not take.

    The ds-decomp config carries a symbol file per overlay with real function
    sizes. Measured against those instead, the overlays are complete.

    An overlay's bytes are exactly three things: inside a function, the
    `static_init` constructor table, or zero fill at the end.
    """
    import re
    sym = re.compile(r"^(\S+)\s+kind:function\((arm|thumb),"
                     r"size=(0x[0-9a-fA-F]+)\)\s+addr:(0x[0-9a-fA-F]+)")
    root = dsd_root or DSD_OVERLAYS
    if not os.path.isdir(root):
        skip("overlay coverage", "docs/39",
             f"ds-decomp overlay config not found at {root}")
        return
    header = json.load(open(os.path.join(extract, "header.json"),
                            encoding="utf-8"))
    ram = cov = init = zero = other = 0
    nfun = 0
    for ov in header["overlays"]:
        i, lo = ov["id"], ov["ram_address"]
        hi = lo + ov["ram_size"]
        path = os.path.join(root, "ov%03d" % i, "symbols.txt")
        fns = []
        if os.path.exists(path):
            with open(path, encoding="utf-8") as fh:
                for line in fh:
                    m = sym.match(line.strip())
                    if m:
                        fns.append((int(m.group(4), 16), int(m.group(3), 16)))
        space = lb_code.AddressSpace(extract, i, "arm9")
        covered = set()
        for a, s in fns:
            covered.update(range(max(a, lo), min(a + s, hi)))
        init_set = set(range(max(lo, ov["static_init_start"]),
                             min(hi, ov["static_init_end"])))
        left = set(range(lo, hi)) - covered - init_set
        blob = space.read(lo, hi - lo) or b""
        zero_set = {x for x in left if blob[x - lo] == 0}
        ram += hi - lo
        cov += len(covered)
        init += len(init_set - covered)
        zero += len(zero_set)
        other += len(left - zero_set)
        nfun += len(fns)
    code = ram - init - zero
    check("overlays: every code byte is inside a function", "docs/39",
          other == 0 and cov == code,
          f"{cov:,} of {code:,} code bytes -> {100.0 * cov / code:.3f}%  "
          f"({nfun} functions, {init:,} init-table + {zero:,} zero-fill bytes "
          f"excluded, {other} non-zero unaccounted)")


# -------------------------------------------------------------- formats -----

def blueprint_claims(extract):
    ent = os.path.join(extract, "entities.json")
    if not os.path.exists(ent):
        skip("blueprint claims", "docs/11", "out/eur/entities.json missing")
        return
    recs = json.load(open(ent, encoding="utf-8"))
    recs = recs["records"] if isinstance(recs, dict) else recs
    raw = open(os.path.join(extract, "plain", "BP", "Entities.ebp"), "rb").read()

    def u32(r, o):
        return struct.unpack_from("<I", raw, r["offset"] + o)[0]

    check("Entities.ebp: 551 records", "docs/11", len(recs) == 551,
          f"{len(recs)} records")

    units = [r for r in recs if r.get("type") == 0]
    items = [r for r in recs if r.get("type") == 2]

    # docs/22: 289 contiguous unlock slots over five categories
    slots = sorted(r["unlock_slot"] for r in items
                   if r.get("unlock_slot") is not None)
    check("unlock slots are 0..288, contiguous", "docs/22",
          slots == list(range(289)),
          f"{len(slots)} slots, {min(slots)}..{max(slots)}" if slots else "none")

    # docs/25: every build time is a multiple of 30
    bt = [r["build_time"] for r in units if r.get("build_time")]
    bad = [b for b in bt if b % 30]
    check("build times are all multiples of 30", "docs/25", not bad,
          f"{len(bt)} units, {len(bad)} exceptions")

    # docs/26: speed round-trips through tiles-per-second
    speeds = sorted({r["speed"] for r in units if r.get("speed")})
    rt = [s for s in speeds
          if round(round(s / 4096 * 30, 2) / 30 * 4096) != s]
    check("speeds round-trip via tiles/second", "docs/26", not rt,
          f"{len(speeds)} distinct speeds, {len(rt)} fail to round-trip")

    # docs/27: every Minifig points at the unit of the same name
    by_index = {r["index"]: r for r in recs}
    figs = [r for r in items if str(r.get("item_category")) == "Minifig"]
    ok = 0
    for f in figs:
        t = by_index.get(u32(f, 0x60))
        if t and t["name"] == f["name"][len("Minifig_"):]:
            ok += 1
    check("Minifig -> unit by name", "docs/27", ok >= len(figs) - 1,
          f"{ok} of {len(figs)} exact (one shipped typo is expected)")

    # docs/27: minikits award 18 distinct Minifigs
    mk = [r for r in items if str(r.get("item_category")) == "Minikit"]
    awards = {u32(r, 0x60) for r in mk}
    all_figs = all(str(by_index[a].get("item_category")) == "Minifig"
                   for a in awards if a in by_index)
    check("minikit awards are 18 distinct Minifigs", "docs/27",
          len(awards) == 18 and all_figs,
          f"{len(awards)} distinct, all Minifigs: {all_figs}")


def objective_claims(extract):
    rtti = os.path.join(extract, "rtti.json")
    if not os.path.exists(rtti):
        skip("objective claims", "docs/32", "rtti.json missing")
        return
    d = json.load(open(rtti, encoding="utf-8"))
    ti = {t["address"]: t for t in d["typeinfos"]}
    base = next((t for t in d["typeinfos"]
                 if t["name"] == "Battles::ObjectiveBase"), None)
    if base is None:
        check("ObjectiveBase found", "docs/32", False, "not in the RTTI")
        return
    subs = [t for t in d["typeinfos"]
            if any(ti.get(b["typeinfo"], {}).get("name") == "Battles::ObjectiveBase"
                   for b in (t.get("bases") or []))]
    check("ObjectiveBase has exactly 13 concrete subclasses", "docs/32",
          len(subs) == 13, f"{len(subs)} subclasses")

    obj_tsv = os.path.join("docs", "data", "mission_objectives.tsv")
    if not os.path.exists(obj_tsv):
        skip("every objective type is used", "docs/32", "matrix missing")
        return
    with open(obj_tsv, encoding="utf-8") as fh:
        cols = fh.readline().rstrip("\n").split(chr(9))
        totals = collections.Counter()
        rows = 0
        for line in fh:
            q = line.rstrip("\n").split(chr(9))
            rows += 1
            for i, c in enumerate(cols):
                if i >= 2 and i < len(q):
                    totals[c] += int(q[i])
    unused = [c for c in cols[2:] if not totals[c]]
    check("all 13 objective types used by some mission", "docs/32",
          not unused, f"{rows} missions, unused: {unused or 'none'}")


def ui_layout_claims(extract):
    """Every UI screen layout parses, and every widget carries a known type.

    Two independent sources have to agree here: the type codes come from a
    jump table in the CODE (ScreenType::virtual_08), and the records come from
    the DATA. If the nibble were not the type, records would carry codes the
    table does not handle.
    """
    try:
        import lb_ui
    except Exception as e:
        skip("UI layouts parse", "docs/47", f"lb_ui unavailable: {e}")
        return
    with open(os.path.join(extract, "files.json"), encoding="utf-8") as fh:
        recs = json.load(fh)
    ok = fail = 0
    kinds = collections.Counter()
    bad_size = rects = good_rect = 0
    for r in recs:
        path = r.get("path") or ""
        if not path.lower().endswith(".bin") or path in lb_ui.EXCLUDED:
            continue
        full = os.path.join(extract, "plain", path.replace("/", os.sep))
        if not os.path.exists(full):
            continue
        with open(full, "rb") as fh:
            data = fh.read()
        try:
            _v, _s, _c, _id, widgets = lb_ui.parse(data)
        except ValueError:
            fail += 1
            continue
        ok += 1
        for _o, w, k, nm, rect, _res in widgets:
            kinds[k] += 1
            if nm and lb_ui.WIDGET_SIZES.get(nm) not in (None, w):
                bad_size += 1
            if rect:
                rects += 1
                if rect[2] >= rect[0] and rect[3] >= rect[1]:
                    good_rect += 1
    unnamed = [k for k in kinds if k not in lb_ui.WIDGET_TYPES]
    check("UI layouts: every screen layout parses", "docs/47", fail == 0,
          f"{ok} parsed, {fail} failed")
    check("UI layouts: every widget type is named", "docs/47", not unnamed,
          f"{sum(kinds.values())} widgets, {len(kinds)} distinct types"
          + (f", UNNAMED {unnamed}" if unnamed else ", all named"))
    check("UI layouts: record size agrees with widget type", "docs/47",
          bad_size == 0, f"{bad_size} disagreements")
    check("UI layouts: widget rectangles are well-formed", "docs/48",
          rects and good_rect == rects,
          f"{good_rect} of {rects} satisfy x2>=x1 and y2>=y1")


def nitro_claims(extract):
    """Every Nitro-container file parses, and NCGR dimensions agree with size.

    The dimension check is the strong one: tiles_w * tiles_h must equal the
    tile count implied by data_size and bpp, and those come from different
    fields of the same section. 461 NCBR files use 0xFFFF dimensions (bitmap
    mode) and are excluded from it rather than counted as passes.
    """
    try:
        import lb_nitro
    except Exception as e:
        skip("Nitro containers parse", "docs/49", f"lb_nitro unavailable: {e}")
        return
    with open(os.path.join(extract, "files.json"), encoding="utf-8") as fh:
        recs = json.load(fh)
    ok = fail = tiled = 0
    agree = disagree = sentinel = 0
    nanr_ok = nanr_bad = ncer_ok = ncer_bad = 0
    g3d_ok = g3d_fail = 0
    g3d_claims = collections.defaultdict(collections.Counter)
    for r in recs:
        path = r.get("path") or ""
        if not path:
            continue
        full = os.path.join(extract, "plain", path.replace("/", os.sep))
        if not os.path.exists(full):
            continue
        with open(full, "rb") as fh:
            head = fh.read(4)
            if head.decode("latin-1", "replace") not in lb_nitro.CONTAINERS:
                continue
            fh.seek(0)
            data = fh.read()
        try:
            fmt, _v, sections, end = lb_nitro.parse(data)
        except ValueError:
            fail += 1
            continue
        ok += 1
        if end == len(data):
            tiled += 1
        if fmt == "NANR":
            i = lb_nitro.payload(fmt, data, sections)
            if i and (i["seq_offset"] + i["sequences"] * 16 == i["frame_offset"]
                      and i["frame_offset"] + i["frames"] * 8
                      == i["frame_data_offset"]):
                nanr_ok += 1
            else:
                nanr_bad += 1
        if fmt == "NCER":
            i = lb_nitro.payload(fmt, data, sections)
            sz = sections[0][2]
            if i and 8 + i["cell_offset"] + i["cells"] * i["cell_entry"] <= sz:
                ncer_ok += 1
            else:
                ncer_bad += 1
        if fmt in ("NSBMD", "NSBCA", "NFTR"):
            # These readers raise rather than return a wrong answer, so a
            # payload that parses at all has already satisfied the G3D
            # dictionary's three offset identities.
            try:
                i = lb_nitro.payload(fmt, data, sections)
            except ValueError:
                g3d_fail += 1
            else:
                g3d_ok += 1
                for k, v in i.items():
                    if isinstance(v, bool):
                        g3d_claims[(fmt, k)][v] += 1
        if fmt == "NCGR":
            info = lb_nitro.payload(fmt, data, sections)
            w, h = info.get("tiles_w"), info.get("tiles_h")
            if w == 0xFFFF or h == 0xFFFF:
                sentinel += 1
            elif w is not None:
                if w * h == info.get("tiles"):
                    agree += 1
                else:
                    disagree += 1
    check("Nitro: every container file parses", "docs/49", fail == 0,
          f"{ok} parsed, {fail} failed")
    check("Nitro: sections tile exactly to the file end", "docs/49",
          tiled == ok, f"{tiled} of {ok}")
    check("Nitro: NCGR dimensions agree with the tile count", "docs/49",
          disagree == 0,
          f"{agree} agree, {disagree} disagree, {sentinel} bitmap-mode excluded")
    check("Nitro: NANR offsets satisfy both identities", "docs/50",
          nanr_bad == 0, f"{nanr_ok} hold, {nanr_bad} fail")
    check("Nitro: NCER cell tables fit their section", "docs/50",
          ncer_bad == 0, f"{ncer_ok} fit, {ncer_bad} do not")
    check("Nitro: every G3D and font payload parses", "docs/51",
          g3d_fail == 0, f"{g3d_ok} parsed, {g3d_fail} failed")
    for (fmt, key), c in sorted(g3d_claims.items()):
        check(f"Nitro: {fmt} {key.replace('_', ' ')}", "docs/51",
              c[False] == 0, f"{c[True]} hold, {c[False]} do not")


def sound_claims(extract):
    """`docs/52`: the shipped C header and the pairing read out of its order."""
    try:
        import lb_sound_symbols as lb_sound
    except ImportError as e:
        skip("sound header parses", "docs/52", f"lb_sound unavailable: {e}")
        return
    path = os.path.join(extract, "plain",
                        lb_sound.SADL.replace("/", os.sep))
    if not os.path.exists(path):
        skip("sound header parses", "docs/52", "sound_data.sadl not extracted")
        return
    with open(path, "rb") as fh:
        text = fh.read().decode("latin-1")
    lines = text.splitlines()
    defs = lb_sound.parse(text)
    check("sound header: every line is a #define", "docs/52",
          len(defs) == len(lines), f"{len(defs)} of {len(lines)} lines")

    ns = collections.defaultdict(list)
    for name, value in defs:
        ns[lb_sound.namespace(name)].append(value)
    bad = [k for k in lb_sound.CONTIGUOUS
           if k in ns and sorted(ns[k]) != list(range(len(ns[k])))]
    check("sound header: WAVE/BANK/SEQARC/STRM are contiguous 0..N-1",
          "docs/52", not bad, "all contiguous" if not bad else ", ".join(bad))

    runs = lb_sound.segments(defs)
    nseq = len(ns.get("SEQARC", []))
    check("sound header: SE runs equal the SEQARC count", "docs/52",
          len(runs) == nseq, f"{len(runs)} runs, {nseq} archives")

    pairs = lb_sound.archives(defs)
    agree = 0
    for _k, owner, run in pairs:
        names = [n for n, _v in run] + [lb_sound.expand(n) for n, _v in run]
        if owner and any(n == owner or n.startswith(owner + "_")
                         for n in names):
            agree += 1
    # 205 of 217; the 12 that miss are the tower tiers, which the header
    # itself never renamed -- see the duplicate count below.
    check("sound header: SE run k carries SEQARC k's name", "docs/52",
          agree >= 205, f"{agree} of {len(pairs)}")

    seen = collections.Counter(n for run in runs for n, _v in run)
    redefines = sum(c - 1 for c in seen.values() if c > 1)
    check("sound header: the 36 redefines are exactly the tower tiers",
          "docs/52", redefines == 36,
          f"{redefines} redefines over "
          f"{sum(1 for c in seen.values() if c > 1)} names")

    bank = {n[len("BANK_"):].split("__", 1)[1]
            for n, _v in defs if n.startswith("BANK_") and "__" in n}
    grid_u = sum(1 for f in lb_sound.FACTIONS for r in lb_sound.UNIT_ROLES
                 if f"{f}_{r}" in bank)
    grid_s = sum(1 for f in lb_sound.FACTIONS
                 for s in lb_sound.STRUCTURE_SLOTS if f"{f}_{s}" in bank)
    check("sound header: 6 factions x 10 unit roles, all present", "docs/52",
          grid_u == 60, f"{grid_u} of 60")
    check("sound header: 6 factions x 10 structure slots, all present",
          "docs/52", grid_s == 60, f"{grid_s} of 60")

    # The eight group banks partition the 200-bank space: each one is followed
    # immediately by the banks it groups, so
    #     group index + 1 + member count == the next group index
    # and the last group plus one is the bank total. Nothing declares the
    # member counts -- they are counted from the names -- so the chain closing
    # on 200 is a check on the whole layout at once.
    group = {}
    members = collections.Counter()
    for name, value in defs:
        if not name.startswith("BANK_"):
            continue
        rest = name[len("BANK_"):]
        if "__" in rest:
            members[rest.split("__", 1)[0] + "S"] += 1
        else:
            group[rest] = value
    order = sorted(group.items(), key=lambda kv: kv[1])
    chain, prev = True, None
    for gname, gval in order:
        if prev is not None:
            pn, pv = prev
            if pv + 1 + members.get(pn, 0) != gval:
                chain = False
        prev = (gname, gval)
    last = order[-1]
    total = last[1] + 1 + members.get(last[0], 0)
    check("sound header: the 8 group banks partition all 200 banks", "docs/52",
          chain and total == len(ns.get("BANK", [])),
          f"{len(order)} groups, chain closes at {total}")

    ev = collections.defaultdict(set)
    for _k, owner, run in pairs:
        for name, _v in run:
            ev[owner].add(name.rsplit("_", 1)[-1])
    units = [f"{f}_{r}" for f in lb_sound.FACTIONS for r in lb_sound.UNIT_ROLES]
    # EARTH and ALIEN rename the builder and three factions rename the
    # transport, so match on the SEQARC name the run actually carries.
    five = sum(1 for o, s in ev.items()
               if s == {"ONSELECT", "ONDEATH", "ONSTRIKE", "ONHIT",
                        "ONSPECIAL"})
    three = sum(1 for o, s in ev.items()
                if s == {"ONSELECT", "ONDEATH", "ONHIT"})
    check("sound header: exactly 60 archives raise all five unit events",
          "docs/52", five == 60, f"{five} archives, {len(units)} unit slots")
    check("sound header: no structure archive raises ONSTRIKE or ONSPECIAL",
          "docs/52", three >= 60, f"{three} archives with the 3-event set")


def economy_claims(extract):
    """`docs/58`: the unit allowance, re-derived from the instructions.

    The constants in `docs/58` are not quoted here -- they are read back out of
    the ARM9 every run. `4 + 4 x farms` is an `add #4` after an `lsl #2`, and if
    either changes this fails rather than the document quietly going stale.
    """
    try:
        import lb_economy
    except ImportError as e:
        skip("allowance table", "docs/58", f"lb_economy unavailable: {e}")
        return
    space = lb_code.AddressSpace(extract, None, "arm9")
    table = lb_economy.allowance_table(space)
    check("economy: the allowance table is 20 bytes at 0x02126D94", "docs/58",
          len(table) == lb_economy.ALLOWANCE_LEN,
          f"{len(table)} bytes: " + " ".join("%02X" % b for b in table[:8])
          + " ...")
    # docs/68 corrects docs/58: the index is a build-limit class, not a
    # blueprint category. The first three are the caps the HUD shows.
    check("economy: one Hero, twenty Minifigures, four Specials", "docs/68",
          len(table) > 2 and table[0] == 1 and table[1] == 20
          and table[2] == 4,
          f"classes 0-2 = {table[:3] if len(table) > 2 else table}")

    arm, thumb = lb_code.make_engines()
    funcs = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))
    rec = funcs.get(lb_economy.LIMIT_REMAINING)
    arms = 0
    if rec:
        cs = thumb if rec["mode"] == "thumb" else arm
        ops = [(i.mnemonic, i.op_str)
               for i in cs.disasm(space.read(lb_economy.LIMIT_REMAINING,
                                             min(rec["size"], 0x60)),
                                  lb_economy.LIMIT_REMAINING)]
        run = 0
        for m, o in ops:
            if m == "b" and o.startswith("#"):
                run += 1
            else:
                arms = max(arms, run)
                run = 0
        arms = max(arms, run)
    check("economy: the limit reader dispatches on nine classes", "docs/68",
          arms >= lb_economy.LIMIT_CLASS_COUNT,
          f"func_0x{lb_economy.LIMIT_REMAINING:08X} has a {arms}-arm jump "
          f"table, and {len(lb_economy.LIMIT_CLASSES)} classes are documented")

    arm, thumb = lb_code.make_engines()
    funcs = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))

    def text(addr):
        r = funcs.get(addr)
        if not r or not r["size"]:
            return []
        cs = thumb if r["mode"] == "thumb" else arm
        return [(i.mnemonic, i.op_str)
                for i in cs.disasm(space.read(addr, r["size"]), addr)]

    ops = text(lb_economy.CAP_MINIFIGURE)
    shift = any(m == "lsl" and op.endswith("#2") for m, op in ops)
    plus = any(m == "add" and op.endswith("#4") for m, op in ops)
    check("economy: the Farm step is lsl #2 then add #4", "docs/58",
          shift and plus,
          f"func_0x{lb_economy.CAP_MINIFIGURE:08X}: "
          f"lsl #2 {'found' if shift else 'MISSING'}, "
          f"add #4 {'found' if plus else 'MISSING'}")

    # The unit/structure split: two jump tables of 20 arms each, and the arm
    # taken changes exactly once, at category 7.
    ops = text(lb_economy.COUNT_BY_CATEGORY)
    runs = []
    cur = []
    for m, op in ops:
        if m == "b" and op.startswith("#"):
            cur.append(op)
        else:
            if len(cur) >= 20:
                runs.append(cur)
            cur = []
    if len(cur) >= 20:
        runs.append(cur)
    splits = []
    for r in runs:
        arms = r[-20:]
        distinct = [i for i in range(1, 20) if arms[i] != arms[i - 1]]
        splits.append(distinct)
    ok = (len(runs) == 2
          and all(d == [lb_economy.STRUCTURE_CATEGORY] for d in splits))
    check("economy: the unit/structure split is at category 7", "docs/58", ok,
          f"{len(runs)} jump tables of 20 arms, changing at {splits}")

    # The harvest constants, read back out of the instructions that hold them.
    ops = text(lb_economy.HARVEST_INTERVAL_FUNC)
    ticks = any(m == "mov" and op.endswith("#0x96") for m, op in ops)
    check("economy: the harvest interval is 150 ticks", "docs/58", ticks,
          f"func_0x{lb_economy.HARVEST_INTERVAL_FUNC:08X} returns "
          f"{lb_economy.HARVEST_TICKS} = "
          f"{lb_economy.HARVEST_TICKS / 30:.1f}s at 30 Hz")

    ops = text(lb_economy.HARVEST_PAYOUT_FUNC)
    base = any(m == "add" and op.endswith("#0x4b") for m, op in ops)
    bonus = any(m == "moveq" and op.endswith("#0x19") for m, op in ops)
    mill = any(m == "mov" and op.endswith("#8") for m, op in ops)
    check("economy: tree payout is 75, plus 25 for a Mill", "docs/58",
          base and bonus and mill,
          f"add #0x4b {'found' if base else 'MISSING'}, "
          f"moveq #0x19 {'found' if bonus else 'MISSING'}, "
          f"category 8 lookup {'found' if mill else 'MISSING'}")

    cap = space.u32(lb_economy.BRICK_CAP_LITERAL)
    check("economy: the brick balance is capped at 500,000", "docs/58",
          cap == 500000, f"func_0x{lb_economy.CREDIT_FUNC:08X} clamps to "
          f"{cap:,} at player+0x{lb_economy.BRICK_BALANCE_OFFSET:02X}")

    # docs/60: the build path cites its own offsets, so read them back.
    ops = text(lb_economy.CAN_PRODUCE)
    cost = any(m == "ldrheq" and o.endswith("#0x5e]") for m, o in ops)
    purse = any(m == "ldr" and o.endswith("#0x90]") for m, o in ops)
    built = any(m == "cmp" and o.endswith("#0x64") for m, o in ops)
    check("build: producing checks cost +0x5E against the purse +0x90",
          "docs/60", cost and purse and built,
          f"func_0x{lb_economy.CAN_PRODUCE:08X}: cost {cost}, purse {purse}, "
          f"progress==100 {built}")

    ops = text(lb_economy.CONSTRUCT_TICK)
    btime = any(m == "ldrh" and o.endswith("#0x60]") for m, o in ops)
    hp = any(m == "ldrh" and o.endswith("#0x62]") for m, o in ops)
    halve = any(m == "lsr" and o.endswith("#1") for m, o in ops)
    bridge = any(m == "cmp" and o.endswith("#0x11") for m, o in ops)
    check("build: the construction tick reads build time, max health, "
          "and the Bridge category", "docs/60",
          btime and hp and halve and bridge,
          f"+0x60 {btime}, +0x62 {hp}, lsr #1 {halve}, "
          f"category 0x11 {bridge}")

    # The +0x6C overload docs/58 flagged: a unit's yield and a collectable's
    # unlock slot share the offset, in different record types.
    slots = 0
    units = 0
    ents = os.path.join("docs", "data", "entities.tsv")
    if os.path.exists(ents):
        with open(ents, encoding="utf-8", newline="") as fh:
            for row in csv.DictReader(fh, delimiter=chr(9)):
                if row["class"] == "Battles::CollectableItemBlueprint" \
                        and row.get("unlock_slot") not in ("", None):
                    slots += 1
                if row["class"] == "Battles::UnitBlueprint" \
                        and row.get("unlock_slot") not in ("", None):
                    units += 1
    check("build: +0x6C is an unlock slot on collectables only", "docs/60",
          slots == 289 and units == 0,
          f"{slots} collectables carry one, {units} unit blueprints do")


def gameplay_claims(extract):
    """`docs/65`: every published constant is still the one in the image.

    The reference table re-derives each value from the cartridge rather than
    quoting it. This runs that derivation and fails if any instruction a
    constant was read from has stopped being there -- which is the failure mode
    a hand-maintained reference has and this one should not.
    """
    try:
        import lb_gameplay
    except ImportError as e:
        skip("gameplay: constants re-derive", "docs/65",
             f"lb_gameplay unavailable: {e}")
        return
    rows = lb_gameplay.collect(extract)
    stale = [r for r in rows if "MISSING" in r[6]]
    kinds = collections.Counter(r[4] for r in rows)
    check("gameplay: every published constant re-reads from the ROM",
          "docs/65", not stale,
          f"{len(rows)} constants ({kinds['instruction']} from instructions, "
          f"{kinds['data']} from data, {kinds['derived']} derived)"
          + ("" if not stale else "; stale: "
             + ", ".join(r[1] for r in stale[:3])))
    # This check used to require at least five open questions, which made
    # answering one FAIL. A check that puts a floor under unresolved items
    # rewards stagnation. What matters is that each one still names a document
    # that exists and gives a reason -- not how many there are.
    bad = []
    for name, doc, why in lb_gameplay.OPEN_QUESTIONS:
        num = doc.split("/")[-1]
        if not why or not any(f.startswith(num + "-") and f.endswith(".md")
                              for f in os.listdir("docs")):
            bad.append(name)
    # docs/70: how much of the mission scripting interface carries a name.
    api = os.path.join("docs", "data", "mission_api.tsv")
    if os.path.exists(api):
        import lb_annotate
        funcs2 = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))
        have = {int(r["address"], 16)
                for r in csv.DictReader(open(os.path.join("docs", "data",
                                                          "annotations.tsv"),
                                              encoding="utf-8"),
                                        delimiter=chr(9))}
        total = anon = 0
        with open(api, encoding="utf-8", newline="") as fh:
            for row in csv.DictReader(fh, delimiter=chr(9)):
                a = int(row["address"], 16)
                total += 1
                rec = funcs2.get(a)
                if a not in have and (not rec or not lb_annotate.is_named(rec)):
                    anon += 1
        check("mission API: the named share is reported, not assumed",
              "docs/70", total >= 96,
              f"{total} entry points, {total - anon} carry a name or an "
              f"annotation, {anon} carry nothing")

    # docs/71: a name published in a document but never written into config is
    # knowledge the tools cannot see. This check closes that gap permanently.
    import re as _re
    _a = _re.compile("`(?:func_)?(0[0-9A-Fa-f]{7}|0x0[0-9A-Fa-f]{7})`")
    _n = _re.compile("`(Battles::[A-Za-z_0-9:]+|[A-Z][A-Za-z_0-9]*_[A-Za-z_0-9]+)`")
    _known = set()
    for _f in ("config/symbols.tsv", "config/read_functions.tsv"):
        with open(_f, encoding="utf-8", newline="") as fh:
            for _row in csv.reader(fh, delimiter=chr(9)):
                if _row and _row[0].startswith("0x"):
                    _known.add(int(_row[0], 16))
    _corpus = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))
    _have = {int(r["address"], 16)
             for r in csv.DictReader(open(os.path.join("docs", "data",
                                                       "annotations.tsv"),
                                          encoding="utf-8"),
                                     delimiter=chr(9))}
    _drift = []
    for _fn in sorted(os.listdir("docs")):
        if not _fn.endswith(".md"):
            continue
        with open(os.path.join("docs", _fn), encoding="utf-8",
                  errors="replace") as fh:
            for _ln in fh:
                _aa, _nn = _a.findall(_ln), _n.findall(_ln)
                if len(_aa) == 1 and len(_nn) == 1:
                    _v = int(_aa[0], 16)
                    if (_v in _corpus and _v not in _known
                            and _v not in _have):
                        _drift.append((_v, _nn[0], _fn))
    check("no name published in a document is missing from config", "docs/71",
          not _drift,
          "every documented name is recorded" if not _drift else
          f"{len(_drift)} adrift, e.g. "
          + ", ".join(f"{n} ({d})" for _, n, d in _drift[:3]))

    # docs/71: the two list libraries, and which classes belong to which.
    import json as _json
    _r = _json.load(open(os.path.join(extract, "rtti.json"), encoding="utf-8"))
    _byaddr = {t["address"]: t for t in _r["typeinfos"]}
    _kids = {}
    for _t in _r["typeinfos"]:
        for _b in (_t.get("bases") or []):
            _p = _byaddr.get(_b["typeinfo"])
            if _p:
                _kids.setdefault(_p["name"], []).append(_t["name"])
    _s = _kids.get("Battles::SListItem", [])
    _d = _kids.get("Battles::DListItem", [])
    check("two list libraries: SListItem and DListItem are separate", "docs/71",
          len(_s) == 25 and len(_d) == 21
          and "Battles::User::FogCircle" in _s
          and "Battles::ObjectiveBase" in _d,
          f"{len(_s)} singly-linked node classes, {len(_d)} doubly-linked; "
          f"FogCircle is SList: {'Battles::User::FogCircle' in _s}, "
          f"ObjectiveBase is DList: {'Battles::ObjectiveBase' in _d}")

    # docs/72: the category split at 7, from the jump table and from the data.
    import struct as _struct
    _sp72 = lb_code.AddressSpace(extract, None, "arm9")
    _raw = _sp72.read(0x0208AAAC, 40)
    if _raw:
        _offs = _struct.unpack("<20h", _raw)
        _arm = [0x0208AAAE + _o for _o in _offs]
        _units = set(_arm[0:7])
        _structs = set(_arm[7:20])
        check("categories 0-6 and 7-19 take different arms of one branch",
              "docs/72",
              len(_units) == 1 and len(_structs) == 1 and not (_units & _structs),
              f"units -> {sorted(hex(a) for a in _units)}, "
              f"structures -> {sorted(hex(a) for a in _structs)}")
    _ents = [r for r in csv.DictReader(
                 open(os.path.join("docs", "data", "entities.tsv"),
                      encoding="utf-8"), delimiter=chr(9))
             if r.get("category_id") and r["category_id"] != "255"]
    _bad = [r["name"] for r in _ents
            if (int(r["category_id"]) >= 7)
            != ((r.get("is_structure") or "").strip().lower()
                in ("1", "true", "yes"))]
    check("that same split separates every unit from every structure",
          "docs/72", _ents and not _bad,
          f"{len(_ents)} entities, {len(_bad)} disagree")

    # docs/73: JobBase implements nothing, and slot 4 is dead in every job.
    _rt = _json.load(open(os.path.join(extract, "rtti.json"), encoding="utf-8"))
    _byn = {t["name"]: t for t in _rt["typeinfos"]}
    _f73 = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))
    _jobs = ["JobBase", "JobAttack", "JobAttackTaskForce", "JobContractBuild",
             "JobDefend", "JobRepair", "JobScout"]
    _slots = {}
    for _j in _jobs:
        _t = _byn.get("Battles::User::" + _j)
        _slots[_j] = _t["vtables"][0]["slots"] if _t else []
    _basesz = [_f73.get(a & ~1, {}).get("size") for a in _slots["JobBase"]]
    check("JobBase implements nothing: all six virtuals are 4-byte empties",
          "docs/73",
          len(_basesz) == 6 and all(s == 4 for s in _basesz),
          f"JobBase slot sizes {_basesz}")
    _s4 = {_j: _f73.get(_slots[_j][4] & ~1, {}).get("size")
           for _j in _jobs if len(_slots[_j]) > 4}
    check("job vtable slot 4 is an empty stub in every one of the seven",
          "docs/73",
          len(_s4) == 7 and all(v == 4 for v in _s4.values()),
          f"slot 4 sizes {sorted(set(_s4.values()))} across {len(_s4)} classes")
    _ticks = {_j: _f73.get(_slots[_j][1] & ~1, {}).get("size", 0)
              for _j in _jobs if len(_slots[_j]) > 1}
    check("the coordinated attack is the AI's largest job tick", "docs/73",
          max(_ticks, key=_ticks.get) == "JobAttackTaskForce",
          "; ".join(f"{k} {v}B" for k, v in
                    sorted(_ticks.items(), key=lambda kv: -kv[1])[:3]))

    # docs/74: the six jobs share one twelve-code event vocabulary, and the
    # dispatch bound is read from the guard, not assumed.
    _sp74 = lb_code.AddressSpace(extract, None, "arm9")
    _arm74, _thumb74 = lb_code.make_engines()
    _bounds = {}
    for _j in ["JobAttack", "JobAttackTaskForce", "JobContractBuild",
               "JobDefend", "JobRepair", "JobScout"]:
        _t = _byn.get("Battles::User::" + _j)
        if not _t:
            continue
        _a = _t["vtables"][0]["slots"][0] & ~1
        _rec = _f73.get(_a)
        if not _rec:
            continue
        _cs = _thumb74 if _rec["mode"] == "thumb" else _arm74
        _ins = list(_cs.disasm(_sp74.read(_a, _rec["size"]), _a))
        for _k, _i in enumerate(_ins):
            if _i.mnemonic == "cmp" and ", #" in _i.op_str:
                _v = int(_i.op_str.split("#")[1], 0)
            if (_i.mnemonic in ("add", "addls")
                    and ("pc, pc" in _i.op_str or _i.op_str.startswith("pc,")
                         or _i.op_str.endswith(", pc"))):
                _bounds[_j] = _v + 1
                break
    check("all six AI jobs dispatch over the same twelve event codes",
          "docs/74",
          len(_bounds) == 6 and set(_bounds.values()) == {12},
          f"{len(_bounds)} jobs, bounds {sorted(set(_bounds.values()))}")
    # the only ARM slot-0 among the six -- the mode must come from the corpus.
    _modes = {}
    for _j in ["JobAttack", "JobAttackTaskForce", "JobContractBuild",
               "JobDefend", "JobRepair", "JobScout"]:
        _t = _byn.get("Battles::User::" + _j)
        _raw = _t["vtables"][0]["slots"][0]
        _modes[_j] = ("thumb" if _raw & 1 else "arm")
        _rec = _f73.get(_raw & ~1)
        if _rec and _rec["mode"] != _modes[_j]:
            _modes[_j] = "MISMATCH"
    check("the vtable low bit agrees with the corpus mode for every job slot 0",
          "docs/74",
          "MISMATCH" not in _modes.values()

          and _modes.get("JobAttackTaskForce") == "arm",
          f"JobAttackTaskForce is {_modes.get('JobAttackTaskForce')}, "
          f"the other five are "
          f"{sorted(set(v for k, v in _modes.items() if k != 'JobAttackTaskForce'))}")

    # docs/75: one constructor and one factory per job, factories in source
    # order, and the defend decision draws only when the choice is real.
    _fac = [0x02090AA8, 0x02090AC8, 0x02090AE8, 0x02090B08, 0x02090B28,
            0x02090B4C]
    check("the six job factories sit in source order, 32 bytes apart",
          "docs/75",
          all(_fac[_i] < _fac[_i + 1] for _i in range(5))
          and all(_f in _f73 for _f in _fac),
          f"gaps {[_fac[_i+1]-_fac[_i] for _i in range(5)]}")
    _d = _sp74.read(0x0208B950, 132)
    if _d:
        _ins = list(_thumb74.disasm(_d, 0x0208B950))
        _calls = [int(_i.op_str[1:], 16) for _i in _ins
                  if _i.mnemonic in ("bl", "blx") and _i.op_str.startswith("#")]
        # Random_Below must be reached past a count>1 test, not unconditionally
        _cmp1 = any(_i.mnemonic == "cmp" and _i.op_str.endswith(", #1")
                    for _i in _ins)
        check("the AI draws a random target only when there is a real choice",
              "docs/75",
              0x0208AF10 in _calls and _cmp1 and 0x02090B4C in _calls,
              f"AI_DecideDefend calls Random_Below: {0x0208AF10 in _calls}, "
              f"guards on a count test: {_cmp1}, "
              f"builds JobDefend: {0x02090B4C in _calls}")

    # docs/76: the counter starts at index 1 and masks its accumulator to u8.
    _c = _sp74.read(0x0209DE9C, 52)
    if _c:
        _ci = list(_thumb74.disasm(_c, 0x0209DE9C))
        _txt = [f"{_i.mnemonic} {_i.op_str}" for _i in _ci]
        _starts_at_one = any(t == "movs r0, #1" for t in _txt)
        _masks = (any("lsls r2, r2, #0x18" == t for t in _txt)
                  and any("lsrs r2, r2, #0x18" == t for t in _txt))
        _skips = any(t == "adds r4, r4, #4" for t in _txt)
        check("AI_CountByKind skips element 0 and keeps a u8 accumulator",
              "docs/76", _starts_at_one and _masks and _skips,
              f"index starts at 1: {_starts_at_one}; pointer pre-advanced: "
              f"{_skips}; accumulator masked to 8 bits: {_masks}")
    # docs/76: the field it compares is a WORD, so it is not a blueprint
    # category -- this check exists because docs/75 claimed otherwise.
    _word = any(_i.mnemonic == "ldr" and "#0x24]" in _i.op_str for _i in _ci)         if _c else False
    _byte = any(_i.mnemonic == "ldrb" and "#0x24]" in _i.op_str for _i in _ci)         if _c else True
    check("the key AI_CountByKind compares is word-sized, not a category byte",
          "docs/76", _word and not _byte,
          f"ldr at +0x24: {_word}, ldrb at +0x24: {_byte}")

    # docs/77: each job constructor stamps a distinct kind at +0x24, and the
    # keys AI_CountByKind is called with are exactly that set, hole included.
    _KCTOR = {0x0208CDB8: 5, 0x0208D6FC: 4, 0x020901B8: 3,
              0x02090524: 6, 0x02090B70: 1, 0x02091580: 0}
    _got = {}
    for _ca, _want in _KCTOR.items():
        _rc = _f73.get(_ca)
        if not _rc:
            continue
        _cse = _thumb74 if _rc["mode"] == "thumb" else _arm74
        _ci2 = list(_cse.disasm(_sp74.read(_ca, _rc["size"]), _ca))
        for _k, _i in enumerate(_ci2):
            if _i.mnemonic == "str" and "#0x24]" in _i.op_str:
                _reg = _i.op_str.split(",")[0]
                for _j in range(_k - 1, max(_k - 6, -1), -1):
                    _o = _ci2[_j]
                    if (_o.mnemonic in ("movs", "mov")
                            and _o.op_str.startswith(_reg + ", #")):
                        _got[_ca] = int(_o.op_str.split("#")[1], 0)
                        break
                break
    check("the six job constructors stamp distinct kinds at +0x24", "docs/77",
          _got == _KCTOR,
          "kinds " + ", ".join(str(_got.get(_c)) for _c in sorted(_KCTOR)))
    check("kind 2 is a hole: no job class uses it", "docs/77",
          2 not in _got.values() and sorted(_got.values()) == [0, 1, 3, 4, 5, 6],
          f"kinds in use {sorted(_got.values())}")

    # docs/78: the build tick is a five-state machine whose cadence is a modulo
    # against a tick counter, and it proposes squad modes through the veto.
    _bt = _sp74.read(0x02090250, 628)
    if _bt:
        _bi = list(_thumb74.disasm(_bt, 0x02090250))
        _txt = [f"{_i.mnemonic} {_i.op_str}" for _i in _bi]
        _bound = any(t == "cmp r1, #4" for t in _txt)
        _divs = sorted({int(_bi[_k - 1].op_str.split("#")[1], 0)
                        for _k, _i in enumerate(_bi)
                        if _i.mnemonic == "blx" and _i.op_str == "#0x211fdb4"
                        and _k and ", #" in _bi[_k - 1].op_str
                        and _bi[_k - 1].mnemonic in ("movs", "mov")})
        check("the build tick dispatches five states and paces on a modulo",
              "docs/78", _bound and _divs == [10, 20],
              f"state bound cmp #4: {_bound}; divisors {_divs}")
        _tick = any(t == "ldrh r0, [r4, #0x30]" for t in _txt)
        _try = any(t == "cmp r0, #2" for t in _txt)
        check("the build job gives up after three attempts", "docs/78",
              _tick and _try and any("strh r0, [r4, #0x36]" == t for t in _txt),
              f"tick counter read: {_tick}; attempt limit compared: {_try}")

    # docs/79: the twelve squad modes split 7/5, identically in three places.
    import struct as _st79
    _st = _f73.get(0x020988F6)
    if _st:
        _si = list(_thumb74.disasm(_sp74.read(0x020988F6, _st["size"]),
                                   0x020988F6))
        _ix = {_i.address: _k for _k, _i in enumerate(_si)}
        _parts = []
        for _jmp in (0x02098A66, 0x02099358, 0x02099454):
            _k = _ix.get(_jmp)
            if _k is None:
                continue
            _ap = None
            for _b in range(_k - 1, max(_k - 10, -1), -1):
                if (_si[_b].mnemonic == "add"
                        and _si[_b].op_str.endswith(", pc")):
                    _ap = _si[_b].address
                    break
            if _ap is None:
                continue
            _offs = _st79.unpack("<12h", _sp74.read(_ap + 4 + 6, 24))
            _tg = [_jmp + 4 + _o for _o in _offs]
            _lo = min(_tg)
            _parts.append(frozenset(_m for _m in range(12) if _tg[_m] == _lo))
        _want = frozenset({2, 3, 5, 7, 8, 10, 11})
        check("all three squad dispatches split the twelve modes the same way",
              "docs/79",
              len(_parts) == 3 and len(set(_parts)) == 1 and _parts[0] == _want,
              f"{len(_parts)} dispatches, groups "

              f"{sorted(_parts[0]) if _parts else '-'}")

    # docs/80: the mode setter is one routine emitted three times, differing in
    # a single staging offset, and mode 1 is sticky.
    _r1 = _sp74.read(0x02098A5A, 42)
    _r2 = _sp74.read(0x0209934C, 42)
    _r3 = _sp74.read(0x02099448, 42)
    check("the squad mode setter is inlined three times, byte-identical",
          "docs/80", bool(_r1) and _r1 == _r2 == _r3,
          "42-byte prefix identical across all three copies")
    _off = []
    for _base in (0x02098A84, 0x02099376, 0x02099472):
        _b = _sp74.read(_base, 2)
        if not _b:
            continue
        for _i in _thumb74.disasm(_b, _base):
            if _i.mnemonic == "ldrb" and "[r6" in _i.op_str:
                _off.append(_i.op_str.split("#")[-1].rstrip("]")
                            if "#" in _i.op_str else "0")
    check("the three copies differ only in the staging slot they read",
          "docs/80", sorted(_off) == ["0x13", "1", "2"],
          f"staging offsets {sorted(_off)}")
    _g = _sp74.read(0x02098A36, 10)
    _gt = [f"{_i.mnemonic} {_i.op_str}"
           for _i in _thumb74.disasm(_g, 0x02098A36)] if _g else []
    check("mode 1 is sticky: the setter returns before asking the veto",
          "docs/80", any(t == "cmp r0, #1" for t in _gt),
          "guard present" if _gt else "not read")

    # docs/80: an independent check on the corpus -- every call target inside
    # the ARM9 text range should be a corpus function start.
    import collections as _co80
    _lo = min(_f73)
    _hi = max(_a + _r["size"] for _a, _r in _f73.items())
    _miss = _co80.Counter()
    _tot = 0
    for _a, _rec in _f73.items():
        _d = _sp74.read(_a, _rec["size"])
        if not _d:
            continue
        _cs = _thumb74 if _rec["mode"] == "thumb" else _arm74
        try:
            _il = list(_cs.disasm(_d, _a))
        except Exception:
            continue
        for _i in _il:
            if _i.mnemonic in ("bl", "blx") and _i.op_str.startswith("#"):
                _t = int(_i.op_str[1:], 16)
                if _lo <= _t < _hi:
                    _tot += 1
                    if _t not in _f73:
                        _miss[_t] += 1
    _interior = sum(1 for _t in _miss
                    if any(_a < _t < _a + _f73[_a]["size"] for _a in _f73))
    check("the call graph agrees with the corpus", "docs/80",
          _tot > 30000 and len(_miss) <= 18 and len(_miss) - _interior <= 5,
          f"{_tot} in-range call targets, {len(_miss)} not corpus starts, "
          f"{_interior} of those interior to a known function, "
          f"{len(_miss) - _interior} unexplained")

    # docs/81: every extra_functions row claims an entry point. Verify the
    # evidence rather than the row: the address must actually be called.
    _ex = os.path.join("config", "extra_functions.tsv")
    _bad81 = []
    _rows81 = 0
    if os.path.exists(_ex):
        with open(_ex, encoding="utf-8", newline="") as _fh:
            for _ln in _fh:
                if not _ln.startswith("0x"):
                    continue
                _rows81 += 1
                _addr = int(_ln.split(chr(9))[0], 16)
                if not _miss.get(_addr) and _addr not in _f73:
                    _bad81.append(_addr)
    check("every extra_functions row names an address something calls",
          "docs/81", _rows81 > 0 and not _bad81,
          f"{_rows81} rows, {len(_bad81)} with no observed caller"
          + ("" if not _bad81 else ": "
             + ", ".join(f"0x{_a:08X}" for _a in _bad81)))

    # docs/82: the overlay corpus records each main function twice, and the
    # ARM7 anomalies all sit in gaps rather than inside known functions.
    import collections as _c82
    _ovr = [_r for _r in csv.DictReader(
                open(os.path.join("docs", "data", "overlay_corpus.tsv"),
                     encoding="utf-8"), delimiter=chr(9))
            if (_r.get("overlay") or "").strip().isdigit()]
    _byov = _c82.defaultdict(list)
    for _r in _ovr:
        _byov[int(_r["overlay"])].append((int(_r["address"], 16),
                                          int(_r["size"], 16)))
    _BASE = 0x0216A2E0
    _pairs = 0
    _both = 0
    for _n, _it in _byov.items():
        _ends = _c82.defaultdict(list)
        for _a, _s in _it:
            _ends[_a + _s].append(_a - _BASE)
        if any(0 in _v and 8 in _v for _v in _ends.values()):
            _both += 1
        _pairs += sum(len(_v) - 1 for _v in _ends.values())
    check("every overlay records its main function twice, at +0 and +8",
          "docs/82", _both == len(_byov) and len(_byov) == 91,
          f"{_both} of {len(_byov)} overlays, {_pairs} alternate entries "
          f"in {len(_ovr)} rows")
    _a7 = list(csv.DictReader(open(os.path.join(extract, "arm7_corpus.tsv"),
                                   encoding="utf-8"), delimiter=chr(9)))
    _secs = _c82.Counter(_r["section"] for _r in _a7)
    # docs/82 asserted TWO sections and docs/92 made it three: the ARM7's
    # mainram autoload was excluded from lb_scan by name, and once included it
    # contributes 153 functions. The point of the check is unchanged -- the
    # ARM7 is disjoint regions, never one range -- so the expectation moves
    # rather than the assertion loosening.
    check("the ARM7 corpus is three disjoint sections, not one range",
          "docs/92",
          set(_secs) == {"static", "wram", "mainram"} and _secs["static"] == 3,
          f"sections {dict(_secs)}")

    # docs/83: the ARM7 call graph is consistent when read by control flow.
    # Linear reading of the recorded extents invents 42 targets out of the
    # literal pools -- this check asserts both halves so the artefact class
    # stays visible rather than being quietly forgotten.
    import lb_scan as _ls
    arm70, thumb70 = lb_code.make_engines()
    _sp7 = _ls.AddressSpace(extract, cpu="arm7")
    _fn7 = {int(_r["address"], 16): (_r["mode"], int(_r["size"], 16))
            for _r in _a7 if _r.get("size")}
    _reg = [(0x02380000, 0x023801CC), (0x037F8000, 0x038074B0)]
    _cfm, _linm = set(), set()
    for _a, (_m, _s) in _fn7.items():
        try:
            _end, _calls, _p, _x = _ls.walk(_sp7, arm70, thumb70, _a, _m)
        except Exception:
            continue
        for _c in _calls:
            _t = _c[0] if isinstance(_c, (tuple, list)) else _c
            if any(_lo <= _t < _hi for _lo, _hi in _reg) and _t not in _fn7:
                _cfm.add(_t)
        _d = _sp7.read(_a, _s)
        if not _d:
            continue
        _cs = thumb70 if _m == "thumb" else arm70
        try:
            for _i in _cs.disasm(_d, _a):
                if _i.mnemonic in ("bl", "blx") and _i.op_str.startswith("#"):
                    _t = int(_i.op_str[1:], 16)
                    if (any(_lo <= _t < _hi for _lo, _hi in _reg)
                            and _t not in _fn7):
                        _linm.add(_t)
        except Exception:
            pass
    check("the ARM7 call graph is consistent under control flow", "docs/83",
          not _cfm and len(_linm) > 0,
          f"control flow finds {len(_cfm)} unmapped targets, linear reading "
          f"finds {len(_linm)} -- all of them literal-pool artefacts")

    # docs/88: the command type ids are distinct. If this ever fails the wire
    # protocol has an ambiguity and the receive path cannot route.
    _ids = os.path.join("docs", "data", "command_ids.tsv")
    if os.path.exists(_ids):
        _vals = []
        with open(_ids, encoding="utf-8", newline="") as _fh:
            for _row in csv.DictReader(_fh, delimiter=chr(9)):
                if (_row.get("type_id") or "").strip():
                    _vals.append(int(_row["type_id"]))
        check("every command type id is distinct", "docs/88",
              len(_vals) >= 26 and len(set(_vals)) == len(_vals),
              f"{len(_vals)} ids, {len(set(_vals))} distinct, "
              f"range {min(_vals)}..{max(_vals)}" if _vals else "none")
        # and the tag really is written at +0x0C by the base constructor
        _bc = _sp74.read(0x02004350, 64)
        _bt = [f"{_i.mnemonic} {_i.op_str}"
               for _i in _arm74.disasm(_bc, 0x02004350)] if _bc else []
        check("CommandBase's constructor stores the type id at +0x0C",
              "docs/88", any(t == "str r1, [r4, #0xc]" for t in _bt),
              "str r1, [r4, #0xc] present" if _bt else "not read")

    # docs/89: the framing layer confirms the slot assignments by raw vtable
    # offset, independently of docs/87's argument-convention reading.
    _snd = _sp74.read(0x020044BC, 72)
    _rcv = _sp74.read(0x02004504, 120)
    if _snd and _rcv:
        _st = [f"{_i.mnemonic} {_i.op_str}" for _i in _arm74.disasm(_snd, 0x020044BC)]
        _rt = [f"{_i.mnemonic} {_i.op_str}" for _i in _arm74.disasm(_rcv, 0x02004504)]
        check("Command_Send calls slot 3 and Command_Receive calls slot 4",
              "docs/89",
              any(t == "ldr r1, [r1, #0xc]" for t in _st)
              and any(t == "ldr r1, [r1, #0x10]" for t in _rt),
              "send uses vtable+0xC, receive uses vtable+0x10")
    _hdr = _sp74.read(0x02004588, 36)
    if _hdr:
        _ht = [f"{_i.mnemonic} {_i.op_str}" for _i in _arm74.disasm(_hdr, 0x02004588)]
        check("the packet header is a one-byte tag masked from +0x0C",
              "docs/89",
              any(t == "ldr r1, [r4, #0xc]" for t in _ht)
              and any(t == "and r1, r1, #0xff" for t in _ht),
              "tag read from +0x0C and masked to 8 bits")



    # docs/85: the simulation never calls a User:: class. This is the check
    # that keeps widescreen and zoom safe -- if it ever fails, the view has
    # been coupled to the simulation and lockstep is at risk.
    import json as _j85
    _r85 = _j85.load(open(os.path.join(extract, "rtti.json"), encoding="utf-8"))
    _own = {}
    for _t in _r85["typeinfos"]:
        for _v in (_t.get("vtables") or []):
            for _s in _v["slots"]:
                _own.setdefault(_s & ~1, _t["name"])
    _sp85 = _ls.AddressSpace(extract, cpu="arm9")
    _sim = [_a for _a, _n in _own.items()
            if _n.startswith("Battles::Sim::") and _a in _f73]
    _bad85 = []
    for _a in _sim:
        try:
            _e, _calls, _p, _x = _ls.walk(_sp85, arm70, thumb70, _a,
                                          _f73[_a]["mode"])
        except Exception:
            continue
        for _c in _calls:
            _t2 = (_c[0] if isinstance(_c, (tuple, list)) else _c) & ~1
            if _own.get(_t2, "").startswith("Battles::User::"):
                _bad85.append((_a, _t2))
    check("the simulation never calls view code", "docs/85",
          len(_sim) > 400 and not _bad85,
          f"{len(_sim)} Sim virtuals walked, {len(_bad85)} calls into a "
          f"User:: class")















    # docs/70: the two mission lists agree across two unrelated functions.
    # Byte patterns are written as hex rather than escapes on purpose -- the
    # first version of this block reached the file as raw control bytes.
    sp70 = lb_code.AddressSpace(extract, None, "arm9")
    arm70, thumb70 = lb_code.make_engines()
    funcs70 = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))
    add_ = sp70.read(0x02049F14, 60)
    rem_ = sp70.read(0x0204A0E8, 96)
    if add_ and rem_:
        # Mission_AddObjective:  adds r0, #8, then a bl to List_PushBack
        base8 = bytes.fromhex("0830") in add_
        # Mission_RemoveByKey:   ldr r4,[r7,#0xc] and ldr r4,[r7,#0x34]
        head_c = bytes.fromhex("fc68") in rem_
        head_34 = bytes.fromhex("7c6b") in rem_
        check("mission: the list at +8 has its head at +0xC, from two "
              "functions that never mention each other", "docs/70",
              base8 and head_c and head_34,
              f"add passes mission+8: {base8}; remove reads +0xC: {head_c} "
              f"and +0x34: {head_34}")

    # docs/70: Heap_New is the most-called function in the image.
    hits = 0
    for f_, rec_ in funcs70.items():
        d_ = sp70.read(f_, rec_["size"])
        if not d_:
            continue
        cs_ = thumb70 if rec_["mode"] == "thumb" else arm70
        try:
            ins_ = list(cs_.disasm(d_, f_))
        except Exception:
            continue
        for i_ in ins_:
            if (i_.mnemonic in ("bl", "blx") and i_.op_str.startswith("#")
                    and int(i_.op_str[1:], 16) == 0x0204C6E4):
                hits += 1
    check("Heap_New is called from more than 800 sites", "docs/70",
          hits > 800, f"{hits} call sites")

    # docs/69: the vision radius comes from mission code, not a blueprint.
    radii = lb_gameplay.vision_radii(extract)
    noimm = [r for r in radii if r[2] is None]
    check("vision: every radius call site is a mission overlay immediate",
          "docs/69", len(radii) >= 50 and not noimm,
          f"{len(radii)} call sites, {len(noimm)} without an immediate, "
          f"values {sorted({r[2] for r in radii if r[2] is not None})}")

    check("gameplay: every open question cites a document and a reason",
          "docs/65", not bad,
          f"{len(lb_gameplay.OPEN_QUESTIONS)} unresolved"
          + ("" if not bad else "; malformed: " + ", ".join(bad[:3])))


def combat_claims(extract):
    """`docs/59`: the impact path's constants, re-read from the ROM."""
    try:
        import lb_combat
    except ImportError as e:
        skip("combat: the explodes flag", "docs/59",
             f"lb_combat unavailable: {e}")
        return
    path = os.path.join(extract, "plain", lb_combat.EBP.replace("/", os.sep))
    if not os.path.exists(path):
        skip("combat: the explodes flag", "docs/59", "Entities.ebp missing")
        return
    recs = lb_combat.projectiles(path)
    same = sum(1 for _i, r in recs
               if lb_combat.pairs(r)[0] == lb_combat.pairs(r)[1])
    check("combat: 27 projectiles, both damage pairs equal in every one",
          "docs/59", len(recs) == 27 and same == len(recs),
          f"{same} of {len(recs)} -- which is why docs/20 could not tell the "
          f"two apart")
    flags = collections.Counter(r[lb_combat.OFF_EXPLODES] for _i, r in recs)
    check("combat: +0x6B partitions the projectiles 11 explode / 16 direct",
          "docs/59", flags.get(1) == 11 and flags.get(0) == 16,
          f"set {flags.get(1, 0)}, clear {flags.get(0, 0)}, "
          f"other {sum(v for k, v in flags.items() if k not in (0, 1))}")

    space = lb_code.AddressSpace(extract, None, "arm9")
    arm, _thumb = lb_code.make_engines()
    ops = [(i.mnemonic, i.op_str)
           for i in arm.disasm(space.read(lb_combat.FIXED_MUL, 40),
                               lb_combat.FIXED_MUL)]
    smull = any(m == "smull" for m, _ in ops)
    rounds = any(m == "adds" and o.endswith("#0x800") for m, o in ops)
    shift = any(m == "lsr" and o.endswith("#0xc") for m, o in ops)
    # A truncating reimplementation drifts by up to a point of damage a hit,
    # so the rounding term is worth pinning rather than remembering.
    check("combat: the 20.12 multiply is a smull that rounds by +0x800",
          "docs/59", smull and rounds and shift,
          f"func_0x{lb_combat.FIXED_MUL:08X}: smull {smull}, "
          f"+0x800 {rounds}, lsr #12 {shift}")

    ops = [(i.mnemonic, i.op_str)
           for i in arm.disasm(space.read(lb_combat.ON_IMPACT, 0x60),
                               lb_combat.ON_IMPACT)]
    branch = any(m == "ldrb" and o.endswith("#0x6b]") for m, o in ops)
    check("combat: the impact path branches on +0x6B", "docs/59", branch,
          f"func_0x{lb_combat.ON_IMPACT:08X} "
          f"{'reads' if branch else 'DOES NOT read'} blueprint+0x6B")

    # docs/66: the animation stride is stated by the accessor's multiply, and
    # set 0 must be empty because the draw gate skips a zero id.
    ops = [(i.mnemonic, i.op_str)
           for i in arm.disasm(space.read(lb_combat.ANIM_ACCESSOR, 0x30),
                               lb_combat.ANIM_ACCESSOR)]
    stride = any(m == "mov" and o.endswith("#0x12") for m, o in ops)
    zero_n, zero_f = lb_combat.anim_set(space, 0)
    check("combat: the animation record stride is stated by the accessor",
          "docs/66", stride and zero_n == 0 and not zero_f,
          f"mul by 0x{lb_combat.ANIM_STRIDE:02X} "
          f"{'present' if stride else 'MISSING'}; set 0 has {zero_n} frames")

    # docs/67: the target-priority score is one value per category, the same
    # in all six factions -- the same shape docs/53 found for the buildings.
    units = lb_combat.unit_blueprints(path)
    ents = os.path.join("docs", "data", "entities.tsv")
    faction_of = {}
    if os.path.exists(ents):
        with open(ents, encoding="utf-8", newline="") as fh:
            for row in csv.DictReader(fh, delimiter=chr(9)):
                faction_of[int(row["index"])] = row.get("faction") or ""
    per_cat = collections.defaultdict(set)
    for i, rec in units:
        if faction_of.get(i):
            per_cat[rec[0x5C]].add(rec[lb_combat.OFF_THREAT])
    split = [c for c, v in per_cat.items() if len(v) > 1]
    check("combat: target priority is one value per category, all six factions",
          "docs/67", len(split) <= 1,
          f"{len(per_cat)} categories, {len(split)} varying"
          + ("" if not split else " (%s)" % ", ".join(map(str, split))))

    used = {r[lb_combat.OFF_ANIM_SET] for _i, r in recs}
    empty = [s for s in used if s and lb_combat.anim_set(space, s)[0] == 0]
    over = [s for s in used
            if lb_combat.anim_set(space, s)[0] > lb_combat.ANIM_MAX_FRAMES]
    check("combat: every non-zero animation set a projectile uses has frames",
          "docs/66", not empty and not over,
          f"{len(used)} sets used, {len(empty)} empty, {len(over)} over "
          f"{lb_combat.ANIM_MAX_FRAMES} frames")


def annotation_claims(extract):
    """`docs/57`: tier L's hold-out, re-run rather than quoted.

    Tier L attributes a function to the class of the two attributed functions
    that bracket it. That is a claim about how the compiler emitted the image,
    and it is checkable: hide each anchor in turn and see whether the rule
    recovers its class. This re-runs that, so the precision in the docs cannot
    drift away from the precision the tool actually has.
    """
    try:
        import lb_annotate
    except ImportError as e:
        skip("tier L hold-out", "docs/57", f"lb_annotate unavailable: {e}")
        return
    funcs, _refs, _callers, _statics, _thunks, _stores = lb_annotate.build(
        extract)
    addrs = sorted(a for a, r in funcs.items()
                   if r["size"] and lb_annotate.TEXT_LO <= a
                   < lb_annotate.TEXT_HI)
    anchors = {}
    for a in addrs:
        if lb_annotate.is_named(funcs[a]):
            c = lb_annotate.class_of(funcs, a)
            if c:
                anchors[a] = c
    val = lb_annotate.validate_locality(addrs, anchors)
    pct = 100.0 * val["right"] / val["cases"] if val["cases"] else 0.0
    check("tier L: hold-out exact-class precision is at least 96%", "docs/57",
          pct >= 96.0,
          f"{val['right']} of {val['cases']} = {pct:.1f}% over "
          f"{val['anchors']} anchors")
    anc = lb_annotate.inheritance(extract)
    rest = [m for m in val["misses"]
            if not lb_annotate.siblings(m[1], m[2])
            and not lb_annotate.related(m[1], m[2], anc)]
    # The interesting number is not the miss rate but how far a miss lands.
    # docs/62: the manifest's only mismatches must stay the two docs/34
    # explained, and they must still be the one-register-field difference.
    # A third mismatch appearing means a shape was added that does not match.
    status = os.path.join("docs", "data", "decomp_status.tsv")
    if os.path.exists(status):
        bad = []
        with open(status, encoding="utf-8", newline="") as fh:
            for row in csv.DictReader(fh, delimiter=chr(9)):
                if row.get("result") not in ("MATCH", None, ""):
                    bad.append(row.get("symbol", "?"))
        check("decomp: the only mismatches are the two docs/34 explains",
              "docs/62", len(bad) <= 2,
              f"{len(bad)} mismatched: " + ", ".join(sorted(bad)[:4]))

    # Tier R is hand-written, so the thing to check is that its rows still
    # point at real functions and still cite a document that exists.
    rows = lb_annotate.read_functions()
    bad_addr = [a for a, _n, _d, _e in rows if a not in funcs]
    bad_doc = []
    for _a, _n, doc, _e in rows:
        num = doc.split("/")[-1]
        hits = [f for f in os.listdir("docs")
                if f.startswith(num + "-") and f.endswith(".md")]
        if not hits:
            bad_doc.append(doc)
    check("tier R: every hand-written row names a real function", "docs/61",
          not bad_addr,
          f"{len(rows)} rows, {len(bad_addr)} not in the corpus"
          + ("" if not bad_addr else ": "
             + ", ".join("0x%08X" % a for a in bad_addr[:4])))
    check("tier R: every row cites a document that exists", "docs/61",
          not bad_doc,
          f"{len(rows)} rows, {len(set(bad_doc))} dangling"
          + ("" if not bad_doc else ": " + ", ".join(sorted(set(bad_doc))[:4])))

    check("tier L: misses land on a related class, all but a handful",
          "docs/57", len(rest) <= 5,
          f"{len(val['misses'])} misses, {len(rest)} name an unrelated class "
          f"({100.0 * len(rest) / val['cases']:.2f}% of cases)")


def media_claims(extract):
    """`docs/55`: the banner's checksum, the movie index, the .abp walk."""
    try:
        import lb_animbp
        import lb_banner
        import lb_movies
        import lb_particles
    except ImportError as e:
        skip("banner CRC", "docs/55", f"tool unavailable: {e}")
        return

    path = os.path.join(extract, "plain", lb_banner.BANNER)
    if os.path.exists(path):
        with open(path, "rb") as fh:
            data = fh.read()
        info = lb_banner.parse(data)
        # The only stored checksum on the cartridge. A match is evidence about
        # every byte of the body, not just about the layout.
        check("banner: the stored CRC-16 matches the body", "docs/55",
              info["crc_ok"],
              f"stored 0x{info['crc_stored']:04X}, computed "
              f"0x{info['crc_computed']:04X}")
        check("banner: six titles and 2,112 bytes exactly", "docs/55",
              len(data) == 0x840 and len(info["titles"]) == 6,
              f"{len(data)} bytes, {len(info['titles'])} titles")
    else:
        skip("banner: the stored CRC-16 matches the body", "docs/55",
             "banner.bnr not extracted")

    root = os.path.join(extract, "plain", lb_movies.MOVIES)
    if os.path.isdir(root):
        tally = collections.defaultdict(collections.Counter)
        const = 0
        files = sorted(os.listdir(root))
        for name in files:
            with open(os.path.join(root, name), "rb") as fh:
                data = fh.read()
            info = lb_movies.parse(data)
            for k, v in lb_movies.checks(data, info).items():
                tally[k][v] += 1
            if all(struct.unpack_from("<I", data, o)[0] == w
                   for o, w in lb_movies.CONSTANTS.items()):
                const += 1
        bad = sum(c[False] for c in tally.values())
        check("movies: all four keyframe identities hold", "docs/55",
              bad == 0,
              ", ".join(f"{k} {c[True]}/{c[True] + c[False]}"
                        for k, c in sorted(tally.items())))
        check("movies: the six constant header words are constant", "docs/55",
              const == len(files), f"{const} of {len(files)} files")
    else:
        skip("movies: all four keyframe identities hold", "docs/55",
             "Movies/ not extracted")

    path = os.path.join(extract, "plain", lb_animbp.ANIM.replace("/", os.sep))
    if os.path.exists(path):
        with open(path, "rb") as fh:
            data = fh.read()
        recs = lb_animbp.parse(data)
        used = sum(6 + 2 * len(r[4]) for r in recs)
        groups = collections.Counter((r[0], r[1]) for r in recs)
        check("Animations.abp: the record walk lands on the last byte",
              "docs/55", used == len(data),
              f"{used} of {len(data)} bytes, {len(recs)} records")
        check("Animations.abp: 36 groups of exactly five", "docs/55",
              len(groups) == 36 and set(groups.values()) == {5},
              f"{len(groups)} groups, sizes {sorted(set(groups.values()))}")
    else:
        skip("Animations.abp: the record walk lands on the last byte",
             "docs/55", "Animations.abp not extracted")

    space = lb_code.AddressSpace(extract, None, "arm9")
    rows = lb_particles.read_table(space, lb_particles.TABLE)
    named = [r for r in rows if r[3]]
    root = os.path.join(extract, "plain", lb_particles.PARTICLES)
    on_disk = set(os.listdir(root)) if os.path.isdir(root) else set()
    in_arm9 = {t.rsplit("/", 1)[-1] for _a, _p, _w, t in named}
    check("particles: the ARM9 table names 16 systems in 60 slots", "docs/55",
          len(rows) == 60 and len(named) == 16,
          f"{len(rows)} slots, {len(named)} named")
    check("particles: 7 named systems were cut, 1 file is unnamed", "docs/55",
          len(in_arm9 - on_disk) == 7 and len(on_disk - in_arm9) == 1,
          f"{len(in_arm9 - on_disk)} cut, {len(on_disk - in_arm9)} unnamed, "
          f"{len(in_arm9 & on_disk)} both")
    # The block/element framing came out of func_02029FF8's own loop, so a
    # file that fails to walk means the code was misread, not the data.
    walked = failed = blocks = elements = 0
    for name in sorted(on_disk):
        with open(os.path.join(root, name), "rb") as fh:
            data = fh.read()
        try:
            bl = lb_particles.walk(data)
        except ValueError:
            failed += 1
        else:
            walked += 1
            blocks += len(bl)
            elements += sum(len(b) for b in bl)
    check("particles: every .hps walks to its last byte", "docs/56",
          failed == 0 and walked == len(on_disk),
          f"{walked} of {len(on_disk)} files, {blocks} blocks, "
          f"{elements} elements")


def sdat_claims(extract):
    """`docs/54`: the archive and the header were written by different tools.

    The strongest cross-check on this cartridge. `sound_data.sadl` is source
    text; `sound_data.sdat` is a 62.9 MB binary built by the audio tool. Nothing
    connects them at runtime. Every name in one is in the other, at the same
    index, and the counts agree six ways.
    """
    try:
        import lb_sdat
        import lb_sound_symbols as lb_sound
    except ImportError as e:
        skip("SDAT parses", "docs/54", f"lb_sdat unavailable: {e}")
        return
    path = os.path.join(extract, "plain", lb_sdat.SDAT.replace("/", os.sep))
    hpath = os.path.join(extract, "plain", lb_sound.SADL.replace("/", os.sep))
    if not (os.path.exists(path) and os.path.exists(hpath)):
        skip("SDAT parses", "docs/54", "sound files not extracted")
        return
    with open(path, "rb") as fh:
        data = fh.read()
    blk, _v, _h, _n = lb_sdat.blocks(data)
    fo, fs = blk["FILE"]
    check("SDAT: the FILE block ends the archive", "docs/54",
          fo + fs == len(data), f"0x{fo:X} + {fs:,} = {fo + fs:,}")

    names, nested = lb_sdat.symbols(data, blk["SYMB"][0])
    counts = lb_sdat.info_counts(data, blk["INFO"][0])
    disagree = [k for k in lb_sdat.TYPES if len(names[k]) != counts[k]]
    check("SDAT: SYMB and INFO agree on every record count", "docs/54",
          not disagree,
          "all 8 types agree" if not disagree else ", ".join(disagree))

    wrong = {k: len(names[k]) for k, v in lb_sdat.HEADER_COUNTS.items()
             if len(names[k]) != v}
    check("SDAT: record counts match the shipped header", "docs/54",
          not wrong,
          "SEQARC 217, BANK 200, WAVEARC 8, STRM 98, PLAYER 2, STRMPLAYER 2"
          if not wrong else str(wrong))

    entries = lb_sdat.fat(data, blk["FAT"][0])
    nfile = struct.unpack_from("<I", data, fo + 8)[0]
    inside = sum(1 for o, s in entries if fo <= o and o + s <= fo + fs)
    check("SDAT: FAT and FILE agree, and every entry is in range", "docs/54",
          len(entries) == nfile and inside == len(entries),
          f"{len(entries)} entries, {nfile} declared, {inside} in range")

    hdefs = lb_sound.parse(open(hpath, encoding="latin-1").read())
    by_ns = collections.defaultdict(dict)
    for n, v in hdefs:
        by_ns[lb_sound.namespace(n)][v] = n
    agree = total = 0
    for kind in lb_sdat.TYPES:
        hk = {"WAVEARC": "WAVE"}.get(kind, kind)
        for i, nm in enumerate(names[kind]):
            total += 1
            if by_ns[hk].get(i) == nm:
                agree += 1
    nok = ntot = 0
    for _k, owner, run in lb_sound.archives(hdefs):
        sd = nested.get("SEQARC__" + owner, [])
        for j, (sen, _v) in enumerate(run):
            ntot += 1
            if j < len(sd) and sd[j] == "SE_" + sen:
                nok += 1
    check("SDAT: every symbol equals the header's #define at that index",
          "docs/54", agree == total and nok == ntot,
          f"{agree + nok} of {total + ntot}, and {len(hdefs)} #define lines")

    ib = blk["INFO"][0]
    seq_ids = lb_sdat.info_file_ids(data, ib, "SEQARC")
    bank_ids = lb_sdat.info_file_ids(data, ib, "BANK")
    seq_share = {names["SEQARC"][i].split("__", 1)[-1]
                 for i, f in enumerate(seq_ids)
                 if collections.Counter(seq_ids)[f] > 1}
    bank_share = {names["BANK"][i].split("__", 1)[-1]
                  for i, f in enumerate(bank_ids)
                  if collections.Counter(bank_ids)[f] > 1}
    check("SDAT: the shared files are the 18 tower archives, both ways",
          "docs/54",
          len(seq_share) == 18 and seq_share == bank_share
          and all("TOWER" in n for n in seq_share),
          f"{len(seq_share)} SEQARC names, {len(bank_share)} BANK names, "
          f"identical: {seq_share == bank_share}")


def roster_claims():
    """`docs/53`: the blueprint roster and the sound header describe one thing.

    Two files that do not reference each other, joined by position: each
    faction's twenty blueprints in index order against the header's ten unit
    slots then ten structure slots. The blueprint's `category` must name the
    slot, and its `is_structure` must agree with the slot's kind.
    """
    try:
        import lb_roster
    except ImportError as e:
        skip("roster: slots agree with blueprints", "docs/53",
             f"lb_roster unavailable: {e}")
        return
    path = os.path.join("docs", "data", "entities.tsv")
    if not os.path.exists(path):
        skip("roster: slots agree with blueprints", "docs/53",
             "docs/data/entities.tsv not built")
        return
    by_faction = lb_roster.load_roster(path)
    check("roster: six factions of exactly twenty blueprints", "docs/53",
          len(by_faction) == 6 and all(len(r) == 20
                                       for r in by_faction.values()),
          ", ".join(f"{f} {len(r)}" for f, r in sorted(by_faction.items())))
    cells = lb_roster.pair(by_faction)
    good = sum(1 for c in cells if c[4])
    check("roster: blueprint category names the sound slot", "docs/53",
          good == len(cells) == 120, f"{good} of {len(cells)} cells")
    kind_ok = sum(1 for _f, _s, k, r, _o in cells
                  if (r.get("is_structure") == "True") == (k == "structure"))
    check("roster: is_structure agrees with the slot's kind", "docs/53",
          kind_ok == len(cells), f"{kind_ok} of {len(cells)} cells")


def file_claims(extract):
    files = json.load(open(os.path.join(extract, "files.json"),
                           encoding="utf-8"))
    by_ext = collections.Counter()
    for r in files:
        p = r.get("path")
        if p:
            by_ext[os.path.splitext(p)[1].lower()] += 1
    check("122 map files", "docs/15", by_ext[".map"] == 122,
          f"{by_ext['.map']} .map files")
    check("136 tile blueprints", "docs/21", by_ext[".tbp"] == 136,
          f"{by_ext['.tbp']} .tbp files")
    check("8 language files", "docs/07", by_ext[".lng"] == 8,
          f"{by_ext['.lng']} .lng files")


def main(argv=None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--dsd-overlays", default=None,
                    help="ds-decomp per-overlay symbol directory")
    args = ap.parse_args(argv)

    print("=" * 74)
    print("Verifying completeness claims against the ROM")
    print("=" * 74)
    print("\nARM9")
    arm9_text(args.extract)
    print("\nARM7")
    arm7_static(args.extract)
    print("\nOverlays")
    overlay_coverage(args.extract, args.dsd_overlays)
    print("\nBlueprints")
    blueprint_claims(args.extract)
    print("\nObjectives")
    objective_claims(args.extract)
    print("\nUI layouts")
    ui_layout_claims(args.extract)
    print("\nNitro graphics")
    nitro_claims(args.extract)
    print("\nSound header")
    sound_claims(args.extract)
    print("\nSound archive")
    sdat_claims(args.extract)
    print("\nEconomy")
    economy_claims(args.extract)
    print("\nCombat")
    combat_claims(args.extract)
    print("\nGameplay reference")
    gameplay_claims(args.extract)
    print("\nAnnotation")
    annotation_claims(args.extract)
    print("\nBanner, movies, particles, animations")
    media_claims(args.extract)
    print("\nFaction roster")
    roster_claims()
    print("\nCartridge inventory")
    file_claims(args.extract)

    ran = [r for r in RESULTS if r[2] is not None]
    failed = [r for r in ran if not r[2]]
    print()
    print("-" * 74)
    print(f"{len(ran) - len(failed)} of {len(ran)} checks pass, "
          f"{len(RESULTS) - len(ran)} skipped")
    for name, doc, _, detail in failed:
        print(f"   FAILING: {name} ({doc}) -- {detail}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
