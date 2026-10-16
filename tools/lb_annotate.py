#!/usr/bin/env python3
"""Derive function annotations the ROM supports but no source lists.

`docs/30` reports annotation as functions carrying "a real symbol or class
attribution", and that figure has sat at ~26.8% since `docs/07`, because
essentially all of it came free from the shipped RTTI in one pass and nothing
had been added since.

This adds two derivations, kept in **separate tiers** because they are not
equally strong. The whole point of the annotation metric is that it distinguishes
what was cheap from what was earned; collapsing new tiers into it would destroy
the distinction it exists for.

## Tier S -- named from a referenced string

A function that loads exactly one distinct string literal is very often *about*
that string. `func_02001E70` references `"BP\\entities.ebp"` and nothing else; it
is the entities loader.

Restricted deliberately to **exactly one** distinct string. A function loading
five strings is a dispatcher or a formatter, and the strings say what it prints
rather than what it is. One string is a strong signal; three is a weak one, and
this takes only the strong case.

## Tier C -- attributed to a class by its callers

If **every** caller of a function is a method of one class, the function is that
class's private helper. `Battles::Sim::PatrolEntityCommand` has eleven such.

The rule is deliberately strict: *all* callers, not most. A function called by
two classes is shared infrastructure and naming it after either would be wrong.
A function with one caller is included, which is the weakest case in the tier
and is marked as such by its caller count.

**This gives an owner, not a name.** `Sim::PatrolEntityCommand::helper_3` says
where a function lives, not what it does -- the same limitation `docs/34` records
for the generated stubs, and it is reported the same way.

## Tier L -- the translation unit a function sits in

A compiler emits one translation unit's functions as a contiguous run. So a
function with no evidence of its own still sits *between* two functions whose
class is known, and if those two agree, everything between them is one file.

This is the largest tier by a wide margin and the weakest kind of claim, so it
is not asserted -- it is **measured**. `validate_locality` hides each anchor in
turn and asks whether the rule recovers its class:

    1,375 of 2,454 anchors have two agreeing neighbours
    1,330 correct                                     96.7% exact class
       45 wrong, of which
       27 name a class sharing a name stem            FireBallAnimation
                                                      vs FireBallEffect
       15 name a class in the same inheritance family per the RTTI
        3 name an unrelated class                     0.22% of cases

The classification of the misses matters more than the miss rate. **A rule that
says "wrong file" and a rule that says "right file, wrong one of the two classes
in it" are not the same rule**, and this is the second one: an effect and its
animation share a source file, and so do a class and its `VramTransferCallback`.

A negative result, recorded so it is not retried: **adding tiers V, A and C to
the anchor set makes this worse on both axes** -- precision falls from 96.7% to
96.2%, unrelated misses rise from 3 to 21, and net yield drops because those
functions were already claimed. The corpus's own class ownership is the anchor
set to use.

Usage:
    python tools/lb_annotate.py
    python tools/lb_annotate.py --tsv docs/data/annotations.tsv
"""

from __future__ import annotations

import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json  # noqa: E402
import struct  # noqa: E402

import lb_code  # noqa: E402

TEXT_LO, TEXT_HI = 0x02000000, 0x02125FC4

#: The C++ static-object initialiser pair. `docs/31` identified func_02121CBC
#: as the atexit push -- an intrusive list whose head sits four bytes below the
#: overlay load address -- and func_020F2BA0 is the constructor these
#: initialisers call before registering the destructor with it. A function that
#: calls both, in that order, and is small, is a static initialiser: it builds
#: one global and arranges for its teardown. See docs/41.
#: The atexit push alone is the signature -- NOT the constructor. The first
#: version of this detector also required func_020F2BA0, which is merely the
#: most common constructor (209 of 235). Fifteen initialisers use
#: func_020F13F0, and a dozen others use one-offs including
#: Battles::Vector2s16::Vector2s16. Hardcoding the common case cost 26
#: functions and would have quietly kept costing them.
STATIC_ATEXIT = 0x02121CBC
STATIC_MAX_SIZE = 64

#: A tail-call thunk: it loads a function pointer from its literal pool, jumps
#: to it with `bx <reg>`, and never returns to its own caller. `thunk_to_X` is
#: complete information -- a thunk has no behaviour beyond forwarding.
THUNK_MAX_SIZE = 24


def vtable_names(extract):
    """Map every vtable address to its class name.

    In the Itanium ABI a vtable is preceded by a pointer to its typeinfo, so a
    word in the image equal to a known typeinfo address marks the vtable four
    bytes later. `docs/07` recovered the typeinfos; this inverts them.

    A function that STORES one of these addresses is constructing an instance
    of that class -- the vptr assignment is the first thing a constructor does.
    """
    with open(os.path.join(extract, "rtti.json"), encoding="utf-8") as fh:
        data = json.load(fh)
    ti = {t["address"]: t["name"] for t in data["typeinfos"]}
    path = os.path.join(extract, "arm9", "section_static_02000000.bin")
    with open(path, "rb") as fh:
        img = fh.read()
    out = {}
    for i in range(0, len(img) - 4, 4):
        w = struct.unpack_from("<I", img, i)[0]
        if w in ti:
            out[TEXT_LO + i + 4] = ti[w]
    return out


def read_strings(extract):
    """NUL-terminated printable strings in the ARM9 static section."""
    path = os.path.join(extract, "arm9", "section_static_02000000.bin")
    with open(path, "rb") as fh:
        img = fh.read()
    out = {}
    for m in re.finditer(rb"[\x20-\x7e]{4,}\x00", img):
        out[TEXT_LO + m.start()] = m.group()[:-1].decode("latin-1")
    return out


def build(extract):
    space = lb_code.AddressSpace(extract, None, "arm9")
    funcs = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))
    arm, thumb = lb_code.make_engines()
    strs = read_strings(extract)

    refs = collections.defaultdict(set)
    callers = collections.defaultdict(set)
    statics = {}
    thunks = {}
    vtabs = vtable_names(extract)
    stores = {}
    for a, r in sorted(funcs.items()):
        if not r["size"] or not (TEXT_LO <= a < TEXT_HI):
            continue
        cs = thumb if r["mode"] == "thumb" else arm
        data = space.read(a, r["size"])
        if not data:
            continue
        for ins in cs.disasm(data, a):
            if ins.mnemonic in ("bl", "blx") and ins.op_str.startswith("#"):
                t = int(ins.op_str[1:], 0) & ~1
                if t in funcs:
                    callers[t].add(a)
                continue
            try:
                target = lb_code.literal_target(ins, r["mode"] == "thumb")
            except Exception:
                continue
            if target is None:
                continue
            word = space.u32(target)
            if word in strs:
                refs[a].add(strs[word])

        # static-initialiser detection needs the call list and the first
        # literal, so it is done in a second, cheap pass over the same bytes.
        if r["size"] <= STATIC_MAX_SIZE:
            calls, lits = [], []
            for ins in cs.disasm(data, a):
                if ins.mnemonic in ("bl", "blx") and ins.op_str.startswith("#"):
                    calls.append(int(ins.op_str[1:], 0) & ~1)
                try:
                    t = lb_code.literal_target(ins, r["mode"] == "thumb")
                except Exception:
                    continue
                if t is not None:
                    w = space.u32(t)
                    if w is not None:
                        lits.append(w)
            if STATIC_ATEXIT in calls and len(lits) >= 3:
                statics[a] = lits[0]

        if r["size"] <= THUNK_MAX_SIZE and a not in statics:
            target = None
            tail = returns = False
            for ins in cs.disasm(data, a):
                m = ins.mnemonic
                if m == "bx":
                    if "lr" in ins.op_str:
                        returns = True
                    else:
                        tail = True
                elif m.startswith("pop") and "pc" in ins.op_str:
                    returns = True
                try:
                    t = lb_code.literal_target(ins, r["mode"] == "thumb")
                except Exception:
                    continue
                if t is None:
                    continue
                w = space.u32(t)
                if w is not None and (w & ~1) in funcs and (w & ~1) != a:
                    target = w & ~1
            # `returns` must be false: a function that can return to its own
            # caller is doing work, not forwarding. And the tail branch is
            # looked for ANYWHERE in the function, not as the last decoded
            # instruction -- the literal pool disassembles as trailing garbage,
            # so requiring it last found zero thunks in a corpus that has 407.
            if target and tail and not returns:
                thunks[a] = target

        # Which class's vptr, if any, does this function assign?
        seen, stored = set(), set()
        for ins in cs.disasm(data, a):
            try:
                t = lb_code.literal_target(ins, r["mode"] == "thumb")
            except Exception:
                t = None
            if t is not None:
                w = space.u32(t)
                if w in vtabs:
                    seen.add(w)
            if ins.mnemonic.startswith("str") and seen:
                stored |= seen
        if len(stored) == 1:
            stores[a] = vtabs[next(iter(stored))]
    return funcs, refs, callers, statics, thunks, stores


#: Hand-written names, each citing the document that shows the evidence.
#: This is the only tier a human writes directly, and it is the strongest:
#: someone read the disassembly. It is also the smallest, and it stays that way
#: -- a row without a document that justifies it does not belong.
READ_FUNCTIONS = os.path.join("config", "read_functions.tsv")


def read_functions(path=READ_FUNCTIONS):
    """[(address, name, doc, evidence)] from the hand-written manifest."""
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip(chr(10)).rstrip(chr(13))
            if not line or line.startswith(">") or line.startswith("address"):
                continue
            p = line.split(chr(9))
            if len(p) < 4:
                continue
            try:
                out.append((int(p[0], 16), p[1], p[2], p[3]))
            except ValueError:
                continue
    return out


#: Suffixes a sibling class in the same translation unit tends to differ by.
#: Used only to CHARACTERISE tier L's misses, never to make an attribution.
SIBLING_SUFFIXES = ("Animation", "Effect", "Callback", "Command", "Impl",
                    "Blueprint", "State", "Data", "Item", "Plotter", "Base")


def inheritance(extract):
    """{class: set of ancestors} from the RTTI base pointers (`docs/07`)."""
    with open(os.path.join(extract, "rtti.json"), encoding="utf-8") as fh:
        data = json.load(fh)
    by_addr = {t["address"]: t for t in data["typeinfos"]}
    direct = {}
    for t in data["typeinfos"]:
        direct[t["name"]] = {by_addr[b["typeinfo"]]["name"]
                             for b in t.get("bases", [])
                             if b.get("typeinfo") in by_addr}
    out = {}
    for name in direct:
        seen, stack = set(), list(direct[name])
        while stack:
            n = stack.pop()
            if n in seen:
                continue
            seen.add(n)
            stack.extend(direct.get(n, ()))
        out[name] = seen
    return out


def related(a, b, anc):
    """Are two classes in the same inheritance family?"""
    if a == b:
        return True
    if b in anc.get(a, ()) or a in anc.get(b, ()):
        return True
    return bool(anc.get(a, set()) & anc.get(b, set()))


def _stem(name):
    """`Battles::FireBallAnimation` -> `FireBall`, for the miss analysis."""
    short = name.rsplit("::", 1)[-1]
    for suf in SIBLING_SUFFIXES:
        if short.endswith(suf) and len(short) > len(suf):
            return short[:-len(suf)]
    return short


def siblings(a, b):
    """Do two class names look like co-residents of one translation unit?"""
    sa, sb = _stem(a), _stem(b)
    if not sa or not sb:
        return False
    return sa in sb or sb in sa


def locality(addrs, anchors, holdout=None):
    """{address: class} for functions bracketed by two anchors of one class.

    Functions are emitted in translation-unit order, so a function with no
    other evidence still sits *between* two functions whose class is known. If
    those two agree, the run between them is one file's worth of code.

    This is an association, not a name -- exactly like tier C. It says which
    file a function lives in, and the validation below measures how often that
    is the same thing as which class it belongs to.
    """
    out = {}
    idx = {a: i for i, a in enumerate(addrs)}
    for a in addrs:
        if a in anchors and a != holdout:
            continue
        i = idx[a]
        before = after = None
        j = i - 1
        while j >= 0:
            b = addrs[j]
            if b in anchors and b != holdout:
                before = anchors[b]
                break
            j -= 1
        j = i + 1
        while j < len(addrs):
            b = addrs[j]
            if b in anchors and b != holdout:
                after = anchors[b]
                break
            j += 1
        if before is not None and before == after:
            out[a] = before
    return out


def validate_locality(addrs, anchors):
    """Hold out each anchor in turn and see whether the rule recovers it.

    Reported as a measured rate, not asserted. The misses are then split by
    whether the predicted class is a plausible co-resident of the true one --
    because a rule that says "wrong file" and a rule that says "right file,
    wrong one of two classes in it" are not the same rule.
    """
    idx = {a: i for i, a in enumerate(addrs)}
    right = 0
    misses = []
    cases = 0
    for a in sorted(anchors):
        i = idx[a]
        before = after = None
        j = i - 1
        while j >= 0:
            b = addrs[j]
            if b in anchors and b != a:
                before = anchors[b]
                break
            j -= 1
        j = i + 1
        while j < len(addrs):
            b = addrs[j]
            if b in anchors and b != a:
                after = anchors[b]
                break
            j += 1
        if before is None or before != after:
            continue
        cases += 1
        if before == anchors[a]:
            right += 1
        else:
            misses.append((a, anchors[a], before))
    return {"cases": cases, "right": right, "misses": misses,
            "anchors": len(anchors)}


def is_named(rec):
    return bool((rec["name"] and not rec["name"].startswith(("func_", "data_")))
                or rec["owners"])


def class_of(funcs, addr):
    rec = funcs.get(addr)
    if rec and rec["owners"]:
        return rec["owners"][0].rsplit("::", 1)[0]
    return None


def slug(text):
    """A C identifier fragment from a string literal."""
    s = re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_")
    return s[:40] or "str"


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

    funcs, refs, callers, statics, thunks, stores = build(args.extract)
    already = {a for a, r in funcs.items() if is_named(r)}

    tier_s = []
    for a, ss in refs.items():
        if a in already or len(ss) != 1:
            continue
        tier_s.append((a, next(iter(ss))))

    claimed = {a for a, _ in tier_s}
    tier_f = [(a, obj) for a, obj in sorted(statics.items())
              if a not in already and a not in claimed]
    claimed |= {a for a, _ in tier_f}
    tier_t = [(a, t) for a, t in sorted(thunks.items())
              if a not in already and a not in claimed]
    claimed |= {a for a, _ in tier_t}

    # A class with exactly one unnamed vptr-assigning function: that function
    # is its constructor, and Class::ctor is a name. A class with several has
    # its construction inlined at several sites, so "constructs a Class" is an
    # association like tier C, not a name. 24 functions assign
    # Sim::Continent's vptr; they are not 24 constructors.
    cand = collections.defaultdict(list)
    for a, cls in sorted(stores.items()):
        if a not in already and a not in claimed:
            cand[cls].append(a)
    tier_v = sorted((v[0], c) for c, v in cand.items() if len(v) == 1)
    tier_a = sorted((a, c) for c, v in cand.items() if len(v) > 1 for a in v)
    claimed |= {a for a, _ in tier_v} | {a for a, _ in tier_a}
    tier_c = []
    for callee, cs in callers.items():
        if callee in already or callee in claimed:
            continue
        classes = {class_of(funcs, c) for c in cs}
        classes.discard(None)
        if len(classes) == 1 and all(class_of(funcs, c) for c in cs):
            tier_c.append((callee, classes.pop(), len(cs)))

    claimed |= {a for a, _c, _n in tier_c}

    # Tier L -- the translation unit a function sits in. Anchored on the
    # classes the corpus already owns, validated by hold-out below, and taken
    # LAST so it only claims what no stronger tier reached.
    addrs = sorted(a for a, r in funcs.items()
                   if r["size"] and TEXT_LO <= a < TEXT_HI)
    anchors = {}
    for a in addrs:
        if a in already:
            c = class_of(funcs, a)
            if c:
                anchors[a] = c
    # Tier R is taken FIRST: a name someone read out of the disassembly beats
    # every derivation below it, and it should not be displaced by one.
    tier_r = [(a, n, d, e) for a, n, d, e in read_functions()
              if a in funcs and a not in already]
    claimed |= {a for a, _n, _d, _e in tier_r}

    val = validate_locality(addrs, anchors)
    tier_l = sorted((a, c) for a, c in locality(addrs, anchors).items()
                    if a not in already and a not in claimed)
    claimed |= {a for a, _ in tier_l}

    total = len(funcs)
    print(f"ARM9 corpus: {total:,} functions, {len(already):,} already annotated "
          f"({100.0 * len(already) / total:.1f}%)")
    print()
    print(f"tier S  named from a single referenced string : {len(tier_s):>4}")
    print(f"tier F  C++ static-object initialisers        : {len(tier_f):>4}")
    print(f"tier T  tail-call thunks                      : {len(tier_t):>4}")
    print(f"tier V  constructors, by the vptr they assign : {len(tier_v):>4}")
    print(f"tier A  constructs a class (several sites)    : {len(tier_a):>4}")
    print(f"tier C  attributed to a class by its callers  : {len(tier_c):>4}")
    print(f"tier L  the translation unit it sits in       : {len(tier_l):>4}")
    print(f"tier R  read by hand, name cited to a document : {len(tier_r):>4}")
    gained = (len(tier_s) + len(tier_f) + len(tier_t) + len(tier_v)
              + len(tier_a) + len(tier_c) + len(tier_l) + len(tier_r))
    print(f"                                        total : {gained:>4}")
    print(f"annotation {100.0 * len(already) / total:.1f}% -> "
          f"{100.0 * (len(already) + gained) / total:.1f}%")
    print()
    print()
    print(f"tier L, held out against its own {val['anchors']} anchors:")
    print(f"   both neighbours agree in {val['cases']} of {val['anchors']} cases")
    if val["cases"]:
        print(f"   correct {val['right']}, wrong {len(val['misses'])} "
              f"-> {100.0 * val['right'] / val['cases']:.1f}% exact-class")
        anc = inheritance(args.extract)
        by_stem = [m for m in val["misses"] if siblings(m[1], m[2])]
        by_inh = [m for m in val["misses"]
                  if not siblings(m[1], m[2]) and related(m[1], m[2], anc)]
        rest = [m for m in val["misses"]
                if not siblings(m[1], m[2]) and not related(m[1], m[2], anc)]
        print(f"   the {len(val['misses'])} misses, classified:")
        print(f"      {len(by_stem):>3} name a class sharing a name stem "
              f"(FireBallAnimation vs FireBallEffect)")
        print(f"      {len(by_inh):>3} name a class in the same inheritance "
              f"family, per the RTTI")
        print(f"      {len(rest):>3} name an unrelated class -- "
              f"{100.0 * len(rest) / val['cases']:.2f}% of all cases")
        for a, t, p in rest:
            print(f"         0x{a:08X}  {t.rsplit('::', 1)[-1]:<34}"
                  f" -> {p.rsplit('::', 1)[-1]}")

    by_class = collections.Counter(c for _, c, _ in tier_c)
    print("classes gaining the most private helpers:")
    for c, n in by_class.most_common(10):
        print(f"   {n:>3}  {c}")
    print()
    print("sample tier-S names:")
    for a, s in sorted(tier_s)[:10]:
        print(f"   0x{a:08X}  {slug(s)}")

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["address", "tier", "derived", "evidence"]) + nl)
            for a, s in sorted(tier_s):
                fh.write(tab.join(["0x%08X" % a, "S", slug(s),
                                   "sole string reference: " + s]) + nl)
            for a, obj in tier_f:
                fh.write(tab.join(["0x%08X" % a, "F",
                                   "static_ctor_%08X" % obj,
                                   "calls the ctor then registers a destructor "
                                   "via the atexit push, for the object at "
                                   "0x%08X" % obj]) + nl)
            for a, t in tier_t:
                rec = funcs.get(t)
                fh.write(tab.join(["0x%08X" % a, "T",
                                   "thunk_to_%08X" % t,
                                   "loads a pointer to 0x%08X and tail-calls it"
                                   % t]) + nl)
            for a, c in tier_v:
                fh.write(tab.join(["0x%08X" % a, "V", c + "::ctor",
                                   "assigns this class's vptr and is the only "
                                   "unnamed function that does"]) + nl)
            for a, c in tier_a:
                fh.write(tab.join(["0x%08X" % a, "A", "constructs_" + c,
                                   "assigns this class's vptr; several "
                                   "functions do, so construction is inlined"]) + nl)
            for a, n, d, e in tier_r:
                fh.write(tab.join(["0x%08X" % a, "R", n,
                                   "%s: %s" % (d, e)]) + nl)
            for a, c in tier_l:
                fh.write(tab.join([
                    "0x%08X" % a, "L", c.rsplit("::", 1)[-1] + "_tu",
                    "sits between two functions of this class"]) + nl)
            for a, c, n in sorted(tier_c):
                fh.write(tab.join(["0x%08X" % a, "C", c + "::helper",
                                   f"all {n} caller(s) are methods of this class"]) + nl)
        print(f"\nwrote {args.tsv} ({len(tier_s) + len(tier_c)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
