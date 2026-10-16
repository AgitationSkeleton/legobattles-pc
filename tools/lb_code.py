#!/usr/bin/env python3
"""Read the game's machine code: an address space, a disassembler, and xrefs.

Ghidra is the eventual home for this work, but nearly every question that comes
up during format reverse engineering is of the form *"who reads this address?"*
or *"what does the function at X do?"*, and those are answerable in a second
from the image itself. Building that here rather than in Ghidra keeps the
answers scriptable, reproducible from the ROM, and available before any analysis
pass has been run.

The cross-reference index is the important half. On ARM, a function that touches
a global or a constant almost always loads it from a **literal pool** — a word
sitting just past the end of the function, reached by `LDR Rd, [PC, #imm]`.
Indexing every such load turns "find the code that parses `Entities.ebp`" from a
reading exercise into a lookup.

    python tools/lb_code.py map
    python tools/lb_code.py dis 0x020a3964 [--count 60]
    python tools/lb_code.py index                 # build/refresh the xref cache
    python tools/lb_code.py xref 0x0213aa24       # who references this value
    python tools/lb_code.py callers 0x020a3964
    python tools/lb_code.py strings --min 5       # strings, with their xrefs
"""

import argparse
import bisect
import collections
import json
import os
import re
import struct
import sys

try:
    import capstone
except ImportError:  # pragma: no cover
    sys.exit("capstone is required: python -m pip install capstone")

DEFAULT_EXTRACT = os.path.join("out", "eur")


# --------------------------------------------------------------- programs ----

class Program:
    def __init__(self, name, base, data, mode="arm"):
        self.name = name
        self.base = base
        self.data = data
        self.end = base + len(data)
        self.default_mode = mode

    def __repr__(self):
        return "<%s 0x%08x..0x%08x %d bytes>" % (self.name, self.base,
                                                 self.end, len(self.data))


class AddressSpace:
    """The programs that make up the game, keyed by runtime address.

    Overlays are deliberately *not* mapped by default: all 91 load at the same
    address, `0x0216A2E0`, so an address in that range is ambiguous unless the
    caller says which overlay it means.
    """

    def __init__(self, extract=DEFAULT_EXTRACT, overlay=None, cpu="arm9"):
        self.extract = extract
        self.cpu = cpu
        self.programs = []
        # One CPU at a time, deliberately. Each core's sections must be mapped at
        # the address they actually run from, and the two address spaces
        # OVERLAP: the ARM9's DTCM sits at 0x027E0000, and the ARM7 copies a
        # 96 KB block to that same address, where it is an ordinary mirror of
        # main RAM. Mapping both at once would silently answer ARM9 questions
        # with ARM7 bytes. See docs/14.
        d = os.path.join(extract, cpu)
        if os.path.isdir(d):
            for fn in sorted(os.listdir(d)):
                m = re.match(r"section_(\w+?)_([0-9a-f]{8})\.bin$", fn)
                if not m:
                    continue
                with open(os.path.join(d, fn), "rb") as fh:
                    self.programs.append(Program(m.group(1),
                                                 int(m.group(2), 16),
                                                 fh.read()))
        if overlay is not None and cpu == "arm9":
            path = os.path.join(extract, "overlays", "ov%03d.bin" % overlay)
            with open(path, "rb") as fh:
                self.programs.append(Program("ov%03d" % overlay, 0x0216A2E0,
                                             fh.read()))
        self.programs.sort(key=lambda p: p.base)
        self._bases = [p.base for p in self.programs]

    def find(self, addr):
        i = bisect.bisect_right(self._bases, addr) - 1
        if i < 0:
            return None
        p = self.programs[i]
        return p if p.base <= addr < p.end else None

    def read(self, addr, size):
        p = self.find(addr)
        if p is None:
            return None
        off = addr - p.base
        return p.data[off:off + size]

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b and len(b) == 4 else None

    def cstr(self, addr, limit=256):
        p = self.find(addr)
        if p is None:
            return None
        off = addr - p.base
        end = p.data.find(b"\x00", off, off + limit)
        if end < 0:
            return None
        return p.data[off:end].decode("latin-1")


# ----------------------------------------------------------------- corpus ----

def load_corpus(path):
    """The merged function list from tools/build_corpus.py."""
    funcs = {}
    if not os.path.exists(path):
        return funcs
    with open(path, encoding="utf-8") as fh:
        header = fh.readline()
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 6:
                continue
            addr = int(parts[0], 16)
            funcs[addr] = {
                "mode": parts[1],
                "size": int(parts[2], 16) if parts[2] else 0,
                "sources": parts[3],
                "name": parts[4],
                "owners": [o for o in parts[5].split(";") if o],
            }
    return funcs


def best_name(rec, addr):
    if rec:
        if rec["name"] and not rec["name"].startswith(("func_", "data_")):
            return rec["name"]
        if rec["owners"]:
            return rec["owners"][0]
        if rec["name"]:
            return rec["name"]
    return "sub_%08x" % addr


# ------------------------------------------------------------ disassembly ----

def make_engines():
    arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    thumb = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    for cs in (arm, thumb):
        cs.detail = True
        cs.skipdata = True
    return arm, thumb


def literal_target(insn, thumb):
    """Effective address of a PC-relative load, or None.

    The ARM PC reads two instructions ahead, and for a word load it is aligned
    down to four. Getting either wrong shifts every literal by a few bytes and
    produces addresses that look almost right, which is the worst kind of wrong.
    """
    for op in insn.operands:
        if op.type != capstone.arm.ARM_OP_MEM:
            continue
        if op.mem.base != capstone.arm.ARM_REG_PC:
            continue
        pc = insn.address + (4 if thumb else 8)
        return (pc & ~3) + op.mem.disp
    return None


LOAD_MNEMONICS = ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrd", "vldr")


def disassemble(space, funcs, addr, count=None, engines=None):
    """Yield (insn, thumb, annotation) for the function at addr."""
    arm_cs, thumb_cs = engines or make_engines()
    rec = funcs.get(addr)
    thumb = (rec or {}).get("mode") == "thumb"
    size = (rec or {}).get("size") or 0
    if not size:
        size = 4 * (count or 64)
    data = space.read(addr, size)
    if data is None:
        return
    cs = thumb_cs if thumb else arm_cs
    emitted = 0
    for insn in cs.disasm(data, addr):
        note = ""
        if insn.mnemonic.split(".")[0] in LOAD_MNEMONICS:
            target = literal_target(insn, thumb)
            if target is not None:
                word = space.u32(target)
                if word is not None:
                    note = "= 0x%08x" % word
                    s = space.cstr(word, 64)
                    if s and len(s) >= 3 and s.isprintable():
                        note += "  %r" % s
                    elif word in funcs or (word & ~1) in funcs:
                        note += "  %s" % best_name(
                            funcs.get(word) or funcs.get(word & ~1), word & ~1)
        elif insn.mnemonic.startswith(("bl", "b")) and insn.operands:
            op = insn.operands[0]
            if op.type == capstone.arm.ARM_OP_IMM:
                t = op.imm
                if t in funcs:
                    note = "-> %s" % best_name(funcs[t], t)
        yield insn, thumb, note
        emitted += 1
        if count and emitted >= count:
            return


# -------------------------------------------------------------- xref index ----

def build_index(space, funcs, out_path):
    """For every function, every literal-pool value and branch target it uses."""
    arm_cs, thumb_cs = make_engines()
    literal_refs = collections.defaultdict(list)   # value  -> [func addr]
    call_refs = collections.defaultdict(list)      # callee -> [caller addr]
    per_func = {}

    ordered = sorted(funcs)
    for i, addr in enumerate(ordered):
        rec = funcs[addr]
        size = rec["size"]
        if not size:
            nxt = ordered[i + 1] if i + 1 < len(ordered) else None
            size = min(nxt - addr, 0x400) if nxt else 0x40
        data = space.read(addr, size)
        if not data:
            continue
        thumb = rec["mode"] == "thumb"
        cs = thumb_cs if thumb else arm_cs
        lits, calls = [], []
        for insn in cs.disasm(data, addr):
            head = insn.mnemonic.split(".")[0]
            if head in LOAD_MNEMONICS:
                target = literal_target(insn, thumb)
                if target is not None:
                    word = space.u32(target)
                    if word is not None:
                        lits.append(word)
                        literal_refs[word].append(addr)
            elif head in ("bl", "blx", "b", "bx") and insn.operands:
                op = insn.operands[0]
                if op.type == capstone.arm.ARM_OP_IMM:
                    calls.append(op.imm)
                    call_refs[op.imm].append(addr)
        per_func[addr] = {"literals": sorted(set(lits)),
                          "calls": sorted(set(calls))}

    payload = {
        "functions": {"0x%08x" % a: v for a, v in per_func.items()},
        "literal_refs": {"0x%08x" % v: ["0x%08x" % a for a in sorted(set(fs))]
                         for v, fs in literal_refs.items()},
        "call_refs": {"0x%08x" % v: ["0x%08x" % a for a in sorted(set(fs))]
                      for v, fs in call_refs.items()},
    }
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh)
    print("functions indexed:   %d" % len(per_func))
    print("distinct literals:   %d" % len(literal_refs))
    print("distinct call sites: %d" % len(call_refs))
    print("wrote %s" % out_path)
    return payload


def load_index(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


# --------------------------------------------------------------------- cli ----

def cmd_map(args, space, funcs):
    for p in space.programs:
        n = sum(1 for a in funcs if p.base <= a < p.end)
        print("  %-8s 0x%08x .. 0x%08x  %8d bytes  %5d functions"
              % (p.name, p.base, p.end, len(p.data), n))
    if space.cpu == "arm9":
        print("  (overlays all load at 0x0216A2E0; pass --overlay N to map one)")
    print("  cpu=%s — pass --cpu arm7 for the other core" % space.cpu)
    return 0


def cmd_dis(args, space, funcs):
    addr = int(args.address, 0)
    rec = funcs.get(addr)
    print("; %s  @0x%08x  %s  size=0x%x"
          % (best_name(rec, addr), addr,
             (rec or {}).get("mode", "?"), (rec or {}).get("size", 0)))
    for o in (rec or {}).get("owners", [])[:8]:
        print(";   vtable slot: %s" % o)
    for insn, _thumb, note in disassemble(space, funcs, addr, args.count):
        print("  0x%08x  %-8s %-32s %s"
              % (insn.address, insn.mnemonic, insn.op_str, note))
    return 0


def cmd_index(args, space, funcs):
    build_index(space, funcs, args.index)
    return 0


def cmd_xref(args, space, funcs):
    idx = load_index(args.index)
    value = int(args.value, 0)
    key = "0x%08x" % value
    lit = idx["literal_refs"].get(key, [])
    call = idx["call_refs"].get(key, [])
    print("literal-pool references to 0x%08x: %d" % (value, len(lit)))
    for a in lit:
        ai = int(a, 16)
        print("   %s  %s" % (a, best_name(funcs.get(ai), ai)))
    print("direct branches to 0x%08x: %d" % (value, len(call)))
    for a in call:
        ai = int(a, 16)
        print("   %s  %s" % (a, best_name(funcs.get(ai), ai)))
    return 0


def cmd_callers(args, space, funcs):
    idx = load_index(args.index)
    addr = int(args.address, 0)
    out = set(idx["call_refs"].get("0x%08x" % addr, []))
    out |= set(idx["literal_refs"].get("0x%08x" % addr, []))
    out |= set(idx["literal_refs"].get("0x%08x" % (addr | 1), []))
    print("callers of %s (0x%08x): %d"
          % (best_name(funcs.get(addr), addr), addr, len(out)))
    for a in sorted(out):
        ai = int(a, 16)
        print("   %s  %s" % (a, best_name(funcs.get(ai), ai)))
    return 0


def cmd_strings(args, space, funcs):
    idx = load_index(args.index) if os.path.exists(args.index) else None
    pat = re.compile((r"[ -~]{%d,}\x00" % args.min).encode())
    rx = re.compile(args.pattern) if args.pattern else None
    for p in space.programs:
        for m in pat.finditer(p.data):
            addr = p.base + m.start()
            s = m.group(0)[:-1].decode("latin-1")
            if rx and not rx.search(s):
                continue
            refs = idx["literal_refs"].get("0x%08x" % addr, []) if idx else []
            print("0x%08x  %-3s %-60r %s"
                  % (addr, p.name, s[:58],
                     " ".join("%s(%s)" % (r, best_name(funcs.get(int(r, 16)),
                                                       int(r, 16)))
                              for r in refs[:4])))
    return 0


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=DEFAULT_EXTRACT)
    ap.add_argument("--corpus", default=os.path.join(DEFAULT_EXTRACT, "corpus.tsv"))
    ap.add_argument("--index", default=os.path.join(DEFAULT_EXTRACT, "xrefs.json"))
    ap.add_argument("--overlay", type=int)
    ap.add_argument("--cpu", choices=["arm9", "arm7"], default="arm9",
                    help="which core's address space to map; they overlap at "
                         "0x027E0000 and are never mapped together")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("map").set_defaults(func=cmd_map)

    d = sub.add_parser("dis")
    d.add_argument("address")
    d.add_argument("--count", type=int)
    d.set_defaults(func=cmd_dis)

    sub.add_parser("index").set_defaults(func=cmd_index)

    x = sub.add_parser("xref")
    x.add_argument("value")
    x.set_defaults(func=cmd_xref)

    c = sub.add_parser("callers")
    c.add_argument("address")
    c.set_defaults(func=cmd_callers)

    s = sub.add_parser("strings")
    s.add_argument("--min", type=int, default=5)
    s.add_argument("--pattern")
    s.set_defaults(func=cmd_strings)

    args = ap.parse_args()
    space = AddressSpace(args.extract, args.overlay, args.cpu)
    funcs = load_corpus(args.corpus)
    return args.func(args, space, funcs)


if __name__ == "__main__":
    sys.exit(main())
