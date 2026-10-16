#!/usr/bin/env python3
"""LEGO Battles `LANG` string tables (`LOC/*.lng`).

Layout:

    0x00  char[4]  'LANG'
    0x04  u32      unknown, constant across every language in the ROM
    0x08  u32      always zero here
    0x0C  u32[N]   byte offset of string ID i, from the start of the file
                   N is implied: the table runs up to the first offset
          char[]   NUL-terminated strings

The table is an ID map, not a list: offsets are **not** monotonic and many IDs
share one string. In the European ROM there are 3,064 IDs over 1,767 distinct
strings, so roughly 42% of the ID space points at something shared — usually a
placeholder for a slot that language does not use.

Encoding is Latin-1 for the European languages. Japanese is not Latin-1 and is
decoded as Shift-JIS; pass --encoding to override.

Usage:
    python tools/lb_lang.py dump  <file.lng> [--encoding latin-1]
    python tools/lb_lang.py grep  <file.lng> <pattern>
    python tools/lb_lang.py table <locdir>            # every language, aligned
"""

import argparse
import json
import os
import re
import struct
import sys

MAGIC = b"LANG"
DEFAULT_ENCODING = {"Japanese": "shift-jis"}


class LangError(Exception):
    pass


def load(path, encoding=None):
    """Return (header, [str or None per ID])."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != MAGIC:
        raise LangError("%s is not a LANG file (magic %r)" % (path, data[:4]))
    if encoding is None:
        stem = os.path.splitext(os.path.basename(path))[0]
        encoding = DEFAULT_ENCODING.get(stem, "latin-1")

    field_04, field_08 = struct.unpack_from("<2I", data, 4)
    first = struct.unpack_from("<I", data, 0x0C)[0]
    if not 0x0C < first <= len(data):
        raise LangError("first offset 0x%x is not inside the file" % first)
    count = (first - 0x0C) // 4
    offsets = struct.unpack_from("<%dI" % count, data, 0x0C)

    strings = []
    for off in offsets:
        if off >= len(data):
            strings.append(None)
            continue
        end = data.find(b"\x00", off)
        if end < 0:
            end = len(data)
        strings.append(data[off:end].decode(encoding, "replace"))

    header = {
        "path": path,
        "encoding": encoding,
        "field_04": field_04,
        "field_08": field_08,
        "count": count,
        "strings_start": first,
        "file_size": len(data),
        "distinct": len(set(offsets)),
    }
    return header, list(strings)


def cmd_dump(args):
    header, strings = load(args.file, args.encoding)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"header": header, "strings": strings}, fh,
                      indent=1, ensure_ascii=False)
        print("wrote %s (%d ids)" % (args.json, header["count"]))
        return 0
    print("# %s  ids=%d distinct=%d encoding=%s field_04=0x%x"
          % (os.path.basename(args.file), header["count"], header["distinct"],
             header["encoding"], header["field_04"]))
    for i, s in enumerate(strings):
        if args.unique_only and i and strings[i - 1] == s:
            continue
        print("%5d\t%s" % (i, "" if s is None else s.replace("\n", "\\n")))
    return 0


def cmd_grep(args):
    header, strings = load(args.file, args.encoding)
    rx = re.compile(args.pattern, 0 if args.case_sensitive else re.IGNORECASE)
    hits = 0
    for i, s in enumerate(strings):
        if s and rx.search(s):
            print("%5d\t%s" % (i, s.replace("\n", "\\n")))
            hits += 1
    print("# %d of %d ids matched" % (hits, header["count"]), file=sys.stderr)
    return 0


def cmd_table(args):
    """Print every language side by side for the IDs that differ."""
    files = sorted(f for f in os.listdir(args.locdir) if f.lower().endswith(".lng"))
    loaded = {}
    for name in files:
        try:
            header, strings = load(os.path.join(args.locdir, name), args.encoding)
        except LangError as exc:
            print("skip %s: %s" % (name, exc), file=sys.stderr)
            continue
        loaded[os.path.splitext(name)[0]] = (header, strings)

    print("language          ids  distinct  bytes  field_04")
    for name, (header, _s) in loaded.items():
        print("%-16s %5d  %8d  %6d  0x%x"
              % (name, header["count"], header["distinct"],
                 header["file_size"], header["field_04"]))

    counts = {h["count"] for h, _ in loaded.values()}
    print()
    print("id-space sizes across languages: %s  -> %s"
          % (sorted(counts), "identical" if len(counts) == 1 else "DIFFERENT"))
    return 0


def main():
    # These files carry bytes no Windows console codepage can render, so force
    # the stream rather than letting a print() abort a dump halfway through.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--encoding")
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("dump")
    d.add_argument("file")
    d.add_argument("--json")
    d.add_argument("--unique-only", action="store_true",
                   help="skip an id whose string repeats the previous id's")
    d.set_defaults(func=cmd_dump)

    g = sub.add_parser("grep")
    g.add_argument("file")
    g.add_argument("pattern")
    g.add_argument("-s", "--case-sensitive", action="store_true")
    g.set_defaults(func=cmd_grep)

    t = sub.add_parser("table")
    t.add_argument("locdir")
    t.set_defaults(func=cmd_table)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
