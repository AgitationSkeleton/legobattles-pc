#!/usr/bin/env python3
"""Inspect and import LEGO Battles save files, without ever risking the original.

The cartridge save is 512 bytes: a 16-byte tag, then two 242-byte `CardPage`
records (docs/176). The tag is `XXX_BATTLE_SAVES`, and the three-character prefix
is a **compile-time constant** in the game binary -- `func_0204b1d4` writes 'A',
'F', 'E' as literal immediates and appends the 13-character string. It is not
derived from the console's region at runtime.

That matters when importing a save from a real cartridge: a save written by a
build whose constant differs (e.g. `AFN`) is REJECTED by a build expecting `AFE`,
and the game silently shows empty profile slots. Verified both ways -- the same
save loads as profile "Tester" once the tag is rewritten, with nothing else changed,
which also shows the checksum does not cover the tag.

    python tools/lb_save.py inspect <file.sav>
    python tools/lb_save.py import <source.sav> <dest.sav> [--tag AFE]

`import` never opens the source for writing, and refuses to clobber a dest that
already exists unless --force is given -- the runner overwrites its save file
even when it could not read it, so pointing it at an original is data loss.
"""

import argparse
import os
import shutil
import sys

SAVE_SIZE = 512
TAG_LEN = 16
SUFFIX = b"_BATTLE_SAVES"
REC_BASE = 0x10          # docs/176: record 1 starts here
REC_LEN = 242
UNLOCK_A = 0xB4          # docs/181: the unlock bit stream
UNLOCK_B = 0xD8


def read_save(path):
    data = open(path, "rb").read()
    if len(data) != SAVE_SIZE:
        raise ValueError("%s is %d bytes, expected %d" % (path, len(data), SAVE_SIZE))
    return data


def describe(data):
    tag = data[:TAG_LEN]
    out = {"tag": tag, "prefix": tag[:3].decode("latin-1", "replace"),
           "tag_ok": tag[3:3 + len(SUFFIX)] == SUFFIX}
    for slot in (0, 1):
        base = REC_BASE + slot * REC_LEN
        if base + REC_LEN > len(data):
            break
        a = data[base + UNLOCK_A:base + UNLOCK_A + 24]
        b = data[base + UNLOCK_B:base + UNLOCK_B + 24]
        out["slot%d_unlock_bits" % slot] = sum(bin(x).count("1") for x in a)
        out["slot%d_fieldB_bits" % slot] = sum(bin(x).count("1") for x in b)
        out["slot%d_checksum" % slot] = data[base:base + 4].hex()
    return out


def cmd_inspect(args):
    data = read_save(args.path)
    info = describe(data)
    print("%s" % args.path)
    print("  tag            %r%s" % (info["tag"],
          "" if info["tag_ok"] else "   <- not a LEGO Battles save tag"))
    print("  prefix         %s" % info["prefix"])
    for slot in (0, 1):
        k = "slot%d_unlock_bits" % slot
        if k not in info:
            continue
        print("  record %d       checksum %s, %d unlock bits set, %d in field B"
              % (slot, info["slot%d_checksum" % slot], info[k],
                 info["slot%d_fieldB_bits" % slot]))
    return 0


def cmd_import(args):
    data = read_save(args.source)          # opened read-only, never written
    info = describe(data)
    if not info["tag_ok"]:
        print("refusing: %s does not carry a *_BATTLE_SAVES tag (%r)"
              % (args.source, info["tag"]), file=sys.stderr)
        return 2

    if os.path.exists(args.dest) and not args.force:
        print("refusing: %s already exists (use --force to replace)" % args.dest,
              file=sys.stderr)
        return 2

    want = args.tag.encode("ascii")
    if len(want) != 3:
        print("--tag must be exactly three characters", file=sys.stderr)
        return 2

    out = bytearray(data)
    if info["prefix"] != args.tag:
        # Rewriting only the prefix. The checksum sits inside each record and
        # does not cover the tag -- established by loading a save whose prefix
        # was changed and nothing else.
        out[0:3] = want
        print("  tag %s -> %s (the build's constant)" % (info["prefix"], args.tag))
    else:
        print("  tag already %s" % args.tag)

    os.makedirs(os.path.dirname(os.path.abspath(args.dest)) or ".", exist_ok=True)
    if os.path.exists(args.dest):
        backup = args.dest + ".bak"
        shutil.copy2(args.dest, backup)
        print("  existing dest backed up to %s" % backup)
    with open(args.dest, "wb") as fh:
        fh.write(bytes(out))
    print("  wrote %s (%d bytes); source untouched" % (args.dest, len(out)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("inspect")
    i.add_argument("path")
    i.set_defaults(func=cmd_inspect)
    m = sub.add_parser("import")
    m.add_argument("source")
    m.add_argument("dest")
    m.add_argument("--tag", default="AFE",
                   help="three-character prefix the target build expects "
                        "(default AFE, this ROM's constant)")
    m.add_argument("--force", action="store_true")
    m.set_defaults(func=cmd_import)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
