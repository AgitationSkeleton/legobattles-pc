#!/usr/bin/env python3
"""Generate C for the ROM's trivial two-instruction functions, and manifest them.

## What this does, and what it does not

The ARM9 contains hundreds of functions that are exactly two instructions: a
single load or immediate move, then `bx lr`. Accessors and constant returns.
For these there is only one sensible encoding, so a compiler's choices barely
enter into it and hand-written C can reproduce the bytes exactly.

This walks the corpus, recognises four shapes, emits C for each, and appends
rows to `config/symbols.tsv` so `tools/lb_match.py` can verify them against the
cartridge.

    mov  r0, #imm  ; bx lr        ->  int f(void) { return imm; }
    ldr  r0, [r0, #n] ; bx lr     ->  int f(void *p) { return *(int *)((char *)p + n); }
    ldrb r0, [r0, #n] ; bx lr     ->  unsigned char variant
    ldrh r0, [r0, #n] ; bx lr     ->  unsigned short variant
    add  r0, r0, #n ; bx lr       ->  void *f(void *p) { return (char *)p + n; }

## Be clear about what this is worth

**A matched accessor is real but shallow.** The bytes are reproduced, which is
the only criterion for a matching decompilation, and that is not nothing. But
the generated function has an **invented name** and an **untyped argument**, so
it carries none of the understanding that the word "decompiled" usually
implies. It is the same distinction `docs/30` already draws between *annotated*
and *analysed*: a large number that was cheap to obtain.

The honest accounting is therefore to count these bytes -- they do match -- and
to report separately how many matched functions carry a real name and a real
type. `docs/34` does that.

Generated sources are written to `src/generated/`, which is regenerable output:
do not hand-edit it, edit this tool.

Usage:
    python tools/lb_stubs.py                # report what it would generate
    python tools/lb_stubs.py --write        # write the sources and manifest rows
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

C_TYPE = {
    "ldr": ("int", "int"),
    "ldrb": ("unsigned char", "unsigned char"),
    "ldrh": ("unsigned short", "unsigned short"),
}


def classify(insns):
    """Return (kind, operand) for a recognised trivial function.

    Only shapes that need **no temporary register** are accepted. That is the
    whole selection criterion, and it comes from docs/34: where the original
    used a scratch register GCC picks a different one (mwccarm reuses r0, GCC
    routes through r3) and the bytes cannot match however correct the C is. A
    setter that stores its own argument, a shift of its own argument, a
    negation of a constant -- none of these has a register to disagree about.
    """
    # The empty function: `bx lr` and nothing else. 263 ARM and 142 Thumb
    # functions in this image are exactly that -- a virtual method that does
    # nothing, or a body the original compiler folded away. In Thumb the ROM
    # pads the 2-byte instruction to 4 with a zero halfword, which disassembles
    # as `movs r0, r0`; that padding is not part of the function.
    if insns and insns[0].mnemonic == "bx" and insns[0].op_str == "lr":
        rest = insns[1:]
        if all(i.mnemonic in ("movs", "mov") and
               i.op_str.replace(" ", "") in ("r0,r0", "r8,r8", "r1,r1")
               for i in rest):
            return "empty", 0
    # A load followed by a shift of the loaded value. Three instructions, but
    # everything stays in r0, so the rule still holds. `docs/62` measured this
    # one as matching where the neighbouring three-instruction shapes do not.
    if len(insns) == 3:
        a0, a1, a2 = insns
        if (a2.mnemonic == "bx" and a2.op_str == "lr"
                and a0.mnemonic in C_TYPE
                and a0.op_str.startswith("r0, [r0")
                and a1.mnemonic in ("lsl", "lsls", "lsr", "lsrs", "asr",
                                    "asrs")
                and a1.op_str.startswith("r0, r0, #")):
            inner = a0.op_str.split("[", 1)[1].rstrip("]")
            parts = [x.strip() for x in inner.split(",")]
            try:
                off = 0 if len(parts) == 1 else int(parts[1][1:], 0)
                sh = int(a1.op_str.split("#", 1)[1], 0)
            except (ValueError, IndexError):
                return None
            return "shift", (a0.mnemonic, off, a1.mnemonic.rstrip("s"), sh)
    if len(insns) != 2:
        return None
    i0, i1 = insns
    if not (i1.mnemonic == "bx" and i1.op_str == "lr"):
        return None
    op = i0.op_str
    if i0.mnemonic in C_TYPE and op.startswith("r0, [r0"):
        inner = op.split("[", 1)[1].rstrip("]")
        parts = [x.strip() for x in inner.split(",")]
        if len(parts) == 1:
            return i0.mnemonic, 0
        if len(parts) == 2 and parts[1].startswith("#"):
            try:
                return i0.mnemonic, int(parts[1][1:], 0)
            except ValueError:
                return None
        return None
    if i0.mnemonic in ("add", "adds") and op.startswith("r0, r0, #"):
        try:
            return "add", int(op.split("#", 1)[1], 0)
        except ValueError:
            return None
    if i0.mnemonic in ("mov", "movs") and op.startswith("r0, #"):
        try:
            return "mov", int(op.split("#", 1)[1], 0)
        except ValueError:
            return None
    if i0.mnemonic == "mvn" and op.startswith("r0, #"):
        try:
            return "mvn", int(op.split("#", 1)[1], 0)
        except ValueError:
            return None
    if i0.mnemonic in ("lsl", "lsls") and op.startswith("r0, r0, #"):
        try:
            return "lsl", int(op.split("#", 1)[1], 0)
        except ValueError:
            return None
    # setters: the value is already in r1, so no scratch register is involved
    for mn, kind in (("str", "set"), ("strb", "setb"), ("strh", "seth")):
        if i0.mnemonic == mn and op.startswith("r1, [r0"):
            inner = op.split("[", 1)[1].rstrip("]")
            parts = [x.strip() for x in inner.split(",")]
            if len(parts) == 1:
                return kind, 0
            if len(parts) == 2 and parts[1].startswith("#"):
                try:
                    return kind, int(parts[1][1:], 0)
                except ValueError:
                    return None
            return None
    return None


SET_TYPE = {"set": "int", "setb": "unsigned char", "seth": "unsigned short"}


def emit(kind, value, name):
    if kind == "shift":
        load, off, op, sh = value
        ctype = C_TYPE[load][0]
        expr = "*(%s *)((char *)p + %d)" % (ctype, off)
        if op == "asr":
            expr = "(int)" + expr
            rhs = "%s >> %d" % (expr, sh)
        elif op == "lsr":
            rhs = "(unsigned)%s >> %d" % (expr, sh)
        else:
            rhs = "%s << %d" % (expr, sh)
        return ("int %s(void *p)%s{%s    return %s;%s}%s"
                % (name, chr(10), chr(10), rhs, chr(10), chr(10)))
    if kind == "empty":
        # The bytes say "return immediately". They do not say what the
        # signature was: `void f(void)`, `void f(void *)` and an int-returning
        # function whose result is never used all compile to the same `bx lr`.
        # The name and the prototype are inventions; the bytes are the claim.
        return "void %s(void)%s{%s}%s" % (name, chr(10), chr(10), chr(10))
    if kind in SET_TYPE:
        t = SET_TYPE[kind]
        return (f"void {name}(void *p, {t} v)\n"
                f"{{\n    *({t} *)((char *)p + {value}) = v;\n}}\n")
    if kind == "mvn":
        return (f"int {name}(void)\n"
                f"{{\n    return ~{value};\n}}\n")
    if kind == "lsl":
        return (f"int {name}(int x)\n"
                f"{{\n    return x << {value};\n}}\n")
    if kind == "mov":
        return (f"int {name}(void)\n"
                f"{{\n    return {value};\n}}\n")
    if kind == "add":
        return (f"void *{name}(void *p)\n"
                f"{{\n    return (char *)p + {value};\n}}\n")
    ctype = C_TYPE[kind][0]
    return (f"{ctype} {name}(void *p)\n"
            f"{{\n    return *({ctype} *)((char *)p + {value});\n}}\n")


def main(argv=None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--out", default=os.path.join("src", "generated"))
    ap.add_argument("--manifest", default=os.path.join("config", "symbols.tsv"))
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args(argv)

    space = lb_code.AddressSpace(args.extract, None, "arm9")
    funcs = lb_code.load_corpus(os.path.join(args.extract, "corpus.tsv"))
    engines = lb_code.make_engines()

    bodies = {"arm": [], "thumb": []}
    rows = []
    for addr in sorted(funcs):
        rec = funcs[addr]
        if rec["size"] not in (2, 4, 8, 12):
            continue
        try:
            insns = [t[0] for t in lb_code.disassemble(space, funcs, addr,
                                                       engines=engines)]
        except Exception:
            continue
        got = classify(insns)
        if not got:
            continue
        kind, value = got
        mode = rec["mode"]
        # A Thumb `bx lr` occupies two bytes; the corpus records four because
        # of the alignment padding after it. Claim what the function is, not
        # what the slot is, or the comparison is against two bytes of padding
        # the compiler has no reason to emit.
        size = 2 if (kind == "empty" and mode == "thumb") else rec["size"]
        name = "sub_%08X" % addr
        bodies[mode].append(emit(kind, value, name))
        rows.append((name, addr, size, mode,
                     "generated/accessors_%s.c" % mode))

    print(f"{len(rows)} trivial functions recognised "
          f"({len(bodies['arm'])} arm, {len(bodies['thumb'])} thumb)")
    if not args.write:
        print("(dry run; pass --write to emit)")
        return 0

    os.makedirs(args.out, exist_ok=True)
    header = ("/* GENERATED by tools/lb_stubs.py -- do not edit.\n"
              " *\n"
              " * Two-instruction functions recovered from the ROM. Names are\n"
              " * placeholders and arguments are untyped: these match bytes, not\n"
              " * meaning. See docs/34.\n"
              " */\n\n")
    for mode in ("arm", "thumb"):
        path = os.path.join(args.out, "accessors_%s.c" % mode)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(header)
            fh.write("\n".join(bodies[mode]))
        print(f"wrote {path} ({len(bodies[mode])} functions)")

    # Rewrite the manifest: keep every hand-written row, replace generated ones.
    kept = []
    with open(args.manifest, encoding="utf-8") as fh:
        for line in fh:
            if "generated/" not in line:
                kept.append(line.rstrip("\n"))
    with open(args.manifest, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(kept) + "\n")
        for name, addr, size, mode, src in rows:
            fh.write("\t".join([name, "0x%08X" % addr, str(size), mode, src]) + "\n")
    print(f"manifest now lists {len(kept) - 1 - sum(1 for k in kept if k.startswith('#'))}"
          f" hand-written + {len(rows)} generated rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
