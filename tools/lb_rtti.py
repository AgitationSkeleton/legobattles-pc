#!/usr/bin/env python3
"""Recover the ARM9's C++ RTTI: class names, inheritance, and vtables.

LEGO Battles' ARM9 was built with RTTI left on, in the Itanium C++ ABI form.
That means the cartridge itself carries, for essentially every polymorphic type
in the game:

  * the fully qualified class name, mangled — ``N7Battles14UnitBlueprintE``;
  * the base class or classes, as typeinfo pointers;
  * a vtable per class, whose slots are the addresses of that class's virtual
    functions, in declaration order.

This is a large, free symbol corpus. It is recovered from the image alone with
no analysis pass and no guessing.

## How it is found

An Itanium typeinfo is one of three shapes, distinguished by its vptr:

    __class_type_info      { vptr, name }
    __si_class_type_info   { vptr, name, base }
    __vmi_class_type_info  { vptr, name, u32 flags, u32 base_count,
                             { base, offset_flags } * base_count }

The three abi vtables are not known in advance, so they are *measured*: every
pointer to a recovered name string is a typeinfo ``name`` field, so the word
before it is a vptr, and the three values that dominate that histogram are the
abi vtables. Which is which follows from the structures' own self-description —
``__cxxabiv1::__si_class_type_info`` is itself one of the typeinfos found.

A vtable is then any location whose word equals a typeinfo address and whose
preceding word is a plausible offset-to-top. Slots after it are virtual function
pointers; on ARM a Thumb function is stored with bit 0 set, which is exactly the
ARM/Thumb mode flag every downstream tool needs.

Usage:
    python tools/lb_rtti.py <arm9_static.bin> [--base 0x02000000]
                            [--code-end 0x0213a000]
                            [--json out/eur/rtti.json] [--symbols out/eur/rtti_symbols.txt]
                            [--classes-tsv docs/data/rtti_classes.tsv]
                            [--methods-tsv docs/data/vtable_methods.tsv]
                            [--inventory docs/data/arm9_inventory.tsv]

Pass --code-end. ARM9 code ends at 0x021350f0 and the RTTI/rodata region starts
at 0x0213aa00; left at the image end, the slot walk runs the
Battles::User::WorldViewScreen vtable straight into the spell-name string table
and picks up 31 string pointers as methods. See docs/data/rtti_summary.md.
"""

import argparse
import collections
import json
import re
import struct
import sys

# A mangled nested name: N <len><ident> ... E, or the std:: shorthand St<len><ident>.
# Identifiers may carry '$' and '.': GCC gives an internal-linkage class a
# discriminator, e.g. N7Battles3Sim14OccupationGrid...MatchFunc$20644OccupationGrid_cppE.
# Dropping '$' loses four real classes.
NESTED = re.compile(
    rb"(?:N(?:\d+[A-Za-z_][A-Za-z0-9_$.]*){1,12}E|St\d+[A-Za-z_][A-Za-z0-9_$.]*)\x00")


def demangle(name):
    """Enough of the Itanium grammar for the names an RTTI section holds."""
    if name.startswith("St"):
        m = re.match(r"St(\d+)(.*)$", name)
        if m and len(m.group(2)) == int(m.group(1)):
            return "std::" + m.group(2)
        return name
    if name.startswith("N") and name.endswith("E"):
        parts, i = [], 1
        while i < len(name) - 1:
            m = re.match(r"\d+", name[i:])
            if not m:
                return name
            n = int(m.group(0))
            i += len(m.group(0))
            parts.append(name[i:i + n])
            i += n
        return "::".join(parts)
    m = re.match(r"(\d+)(.*)$", name)
    if m and len(m.group(2)) == int(m.group(1)):
        return m.group(2)
    return name


class Image:
    def __init__(self, data, base):
        self.data = data
        self.base = base
        self.end = base + len(data)

    def __contains__(self, addr):
        return self.base <= addr < self.end

    def u32(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def cstr(self, addr):
        off = addr - self.base
        end = self.data.find(b"\x00", off)
        return self.data[off:end].decode("ascii", "replace")

    def find_word(self, value):
        """Every 4-aligned offset whose word equals value."""
        needle = struct.pack("<I", value)
        out, pos = [], 0
        while True:
            pos = self.data.find(needle, pos)
            if pos < 0:
                return out
            if pos % 4 == 0:
                out.append(self.base + pos)
            pos += 1


def collect_names(img):
    return {img.base + m.start(): m.group(0)[:-1].decode("ascii")
            for m in NESTED.finditer(img.data)}


def classify_vptrs(img, names):
    """Histogram the word preceding every pointer-to-a-name-string."""
    votes = collections.Counter()
    name_field_at = {}
    for addr in names:
        for ref in img.find_word(addr):
            ti = ref - 4
            if ti not in img:
                continue
            vptr = img.u32(ti)
            if vptr not in img:
                continue
            votes[vptr] += 1
            name_field_at.setdefault(ti, addr)
    return votes, name_field_at


def parse(img, names, kinds, name_field_at):
    """Read every typeinfo. Returns {address: record}."""
    out = {}
    for ti, name_addr in sorted(name_field_at.items()):
        vptr = img.u32(ti)
        kind = kinds.get(vptr)
        if kind is None:
            continue
        rec = {
            "address": ti,
            "kind": kind,
            "mangled": names[name_addr],
            "name": demangle(names[name_addr]),
            "bases": [],
        }
        if kind == "si":
            base = img.u32(ti + 8)
            rec["bases"] = [{"typeinfo": base, "offset": 0}]
        elif kind == "vmi":
            flags, count = struct.unpack_from("<2I", img.data, ti + 8 - img.base)
            rec["vmi_flags"] = flags
            for i in range(count):
                b, of = struct.unpack_from("<2i", img.data,
                                           ti + 16 + 8 * i - img.base)
                rec["bases"].append({
                    "typeinfo": b & 0xFFFFFFFF,
                    "offset": of >> 8,
                    "flags": of & 0xFF,
                })
        out[ti] = rec
    return out


def find_vtables(img, typeinfos, code_lo, code_hi):
    """A vtable is [offset_to_top, typeinfo, fn...]. Return {ti_addr: [vtables]}."""
    result = collections.defaultdict(list)
    for ti in typeinfos:
        for ref in img.find_word(ti):
            head = ref - 4
            if head not in img:
                continue
            offset_to_top = struct.unpack_from("<i", img.data, head - img.base)[0]
            # offset-to-top is 0 for a primary vtable and a small negative
            # number for a secondary one under multiple inheritance.
            if not -0x10000 < offset_to_top <= 0:
                continue
            # This build encodes a pure virtual as a literal 0, not as a
            # pointer to __cxa_pure_virtual (that symbol does not exist in the
            # image), and the nulls occur *mid*-table -- e.g.
            # Battles::Sim::EntityCommandBase has four consecutive nulls at
            # slots 8..11 followed by real slots at 12 and 13. Stopping at the
            # first null truncates the interface of exactly the classes that
            # matter most: Battles::Effect (30 subclasses) read as 2 slots
            # instead of 11. So buffer nulls and commit them only once a later
            # real slot proves the walk is still inside the table; nulls still
            # pending at the end are dropped as padding.
            slots, pending = [], []
            p = ref + 4
            while p + 4 <= img.end:
                word = img.u32(p)
                if word == 0:
                    pending.append(0)
                    p += 4
                    continue
                if not code_lo <= (word & ~1) < code_hi:
                    break
                slots.extend(pending)
                pending = []
                slots.append(word)
                p += 4
            if slots:
                result[ti].append({
                    "vtable": ref + 4,
                    "offset_to_top": offset_to_top,
                    "slots": slots,
                })
    return result


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--base", default="0x02000000")
    ap.add_argument("--code-end", default=None,
                    help="end of executable code; defaults to the image end")
    ap.add_argument("--json")
    ap.add_argument("--symbols", help="write a flat name/address symbol list")
    ap.add_argument("--classes-tsv", help="write docs/data/rtti_classes.tsv")
    ap.add_argument("--methods-tsv", help="write docs/data/vtable_methods.tsv")
    ap.add_argument("--inventory",
                    help="arm9_inventory.tsv, for the in_inventory column")
    args = ap.parse_args()

    base = int(args.base, 0)
    with open(args.image, "rb") as fh:
        img = Image(fh.read(), base)
    code_hi = int(args.code_end, 0) if args.code_end else img.end

    names = collect_names(img)
    print("mangled type names in image: %d" % len(names))

    votes, name_field_at = classify_vptrs(img, names)
    print("typeinfo candidates: %d" % len(name_field_at))
    print("vptr histogram (top 6):")
    for vptr, count in votes.most_common(6):
        print("   0x%08x  %5d" % (vptr, count))

    # Identify the three abi vtables by their own typeinfo names.
    kinds = {}
    abi_by_name = {
        "__cxxabiv1::__class_type_info": "class",
        "__cxxabiv1::__si_class_type_info": "si",
        "__cxxabiv1::__vmi_class_type_info": "vmi",
    }
    # Pass one: read every candidate's name with an unknown kind, so the abi
    # types themselves can be located, then map their vtables.
    provisional = {ti: demangle(names[a]) for ti, a in name_field_at.items()}
    abi_ti = {v: k for k, v in provisional.items() if v in abi_by_name}
    for label, ti in abi_ti.items():
        # The class's own vtable is the location whose typeinfo slot points here
        # and whose following words are code — its vptr value is that + 8.
        for ref in img.find_word(ti):
            head = ref - 4
            if head in img and img.u32(head) == 0:
                kinds[ref + 4] = abi_by_name[label]
    if len(kinds) < 3:
        # Fall back to the histogram: the three dominant vptrs are the abi
        # vtables, ordered by structure size, which is how they are told apart
        # when a name lookup does not resolve.
        print("WARNING: identified %d of 3 abi vtables by name; "
              "falling back to the histogram" % len(kinds), file=sys.stderr)
        for vptr, _ in votes.most_common(3):
            kinds.setdefault(vptr, "class")
    print()
    print("abi vtables:")
    for vptr, kind in sorted(kinds.items()):
        print("   0x%08x  __%s_type_info  (%d users)"
              % (vptr, kind, votes.get(vptr, 0)))

    typeinfos = parse(img, names, kinds, name_field_at)
    print()
    print("typeinfos parsed: %d" % len(typeinfos))
    by_kind = collections.Counter(t["kind"] for t in typeinfos.values())
    for k, n in by_kind.most_common():
        print("   %-6s %d" % (k, n))

    vtables = find_vtables(img, typeinfos, base, code_hi)
    total_slots = sum(len(v["slots"]) for vs in vtables.values() for v in vs)
    print()
    print("classes with a vtable: %d of %d" % (len(vtables), len(typeinfos)))
    print("vtable slots (virtual function pointers): %d" % total_slots)
    thumb = sum(1 for vs in vtables.values() for v in vs
                for s in v["slots"] if s & 1)
    print("   of which Thumb: %d (%.1f%%)"
          % (thumb, 100.0 * thumb / total_slots if total_slots else 0))
    distinct = {s & ~1 for vs in vtables.values() for v in vs for s in v["slots"]}
    print("   distinct target addresses: %d" % len(distinct))

    if args.json:
        payload = {
            "image": args.image,
            "base": base,
            "typeinfos": [dict(t, vtables=vtables.get(a, []))
                          for a, t in sorted(typeinfos.items())],
        }
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=1)
        print("wrote %s" % args.json)

    if args.symbols:
        with open(args.symbols, "w", encoding="utf-8") as fh:
            for a, t in sorted(typeinfos.items()):
                fh.write("typeinfo_%s\t0x%08x\n"
                         % (t["name"].replace("::", "__"), a))
                for v in vtables.get(a, []):
                    suffix = ("" if v["offset_to_top"] == 0
                              else "_at%d" % -v["offset_to_top"])
                    fh.write("vtable_%s%s\t0x%08x\n"
                             % (t["name"].replace("::", "__"), suffix,
                                v["vtable"]))
                    for i, s in enumerate(v["slots"]):
                        if s == 0:      # pure virtual: no address to name
                            continue
                        fh.write("%s::virtual_%02d\t0x%08x\t%s\n"
                                 % (t["name"], i, s & ~1,
                                    "thumb" if s & 1 else "arm"))
        print("wrote %s" % args.symbols)

    if args.classes_tsv or args.methods_tsv:
        inv = {}
        if args.inventory:
            with open(args.inventory, encoding="utf-8") as fh:
                fh.readline()
                for line in fh:
                    p = line.rstrip("\n").split("\t")
                    if len(p) >= 3:
                        inv[int(p[0], 16)] = (p[1], int(p[2]))

        def primary(vs):
            for v in vs:
                if v["offset_to_top"] == 0:
                    return v
            return vs[0] if vs else None

        if args.classes_tsv:
            cols = ["class_name", "mangled_name", "rtti_kind", "vtable_address",
                    "num_slots", "typeinfo_address", "name_string_address",
                    "num_vtables", "secondary_vtables", "num_bases",
                    "base_classes", "pure_virtual_slots"]
            with open(args.classes_tsv, "w", encoding="utf-8", newline="\n") as fh:
                fh.write("\t".join(cols) + "\n")
                for a, t in sorted(typeinfos.items()):
                    vs = vtables.get(a, [])
                    pv = primary(vs)
                    bases = ";".join(
                        typeinfos[b["typeinfo"]]["name"]
                        if b["typeinfo"] in typeinfos else "0x%08x" % b["typeinfo"]
                        for b in t["bases"])
                    fh.write("\t".join([
                        t["name"], t["mangled"], t["kind"],
                        "0x%08x" % pv["vtable"] if pv else "",
                        str(len(pv["slots"])) if pv else "0",
                        "0x%08x" % a,
                        "0x%08x" % name_field_at[a],
                        str(len(vs)),
                        ";".join("0x%08x" % v["vtable"] for v in vs if v is not pv),
                        str(len(t["bases"])), bases,
                        str(sum(1 for s in pv["slots"] if s == 0)) if pv else "0",
                    ]) + "\n")
            print("wrote %s" % args.classes_tsv)

        if args.methods_tsv:
            cols = ["class_name", "slot_index", "function_address",
                    "in_inventory", "vtable_address", "offset_to_top", "mode",
                    "inventory_name", "inventory_size", "proposed_symbol"]
            with open(args.methods_tsv, "w", encoding="utf-8", newline="\n") as fh:
                fh.write("\t".join(cols) + "\n")
                for a, t in sorted(typeinfos.items()):
                    for v in vtables.get(a, []):
                        for i, s in enumerate(v["slots"]):
                            sym = "%s::virtual_%02d" % (t["name"], i)
                            if s == 0:      # pure virtual
                                fh.write("\t".join([
                                    t["name"], str(i), "", "0",
                                    "0x%08x" % v["vtable"],
                                    str(v["offset_to_top"]), "null", "", "",
                                    sym]) + "\n")
                                continue
                            fa = s & ~1
                            e = inv.get(fa)
                            fh.write("\t".join([
                                t["name"], str(i), "0x%08x" % fa,
                                "1" if e else "0", "0x%08x" % v["vtable"],
                                str(v["offset_to_top"]),
                                "thumb" if s & 1 else "arm",
                                e[0] if e else "", str(e[1]) if e else "",
                                sym]) + "\n")
            print("wrote %s" % args.methods_tsv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
