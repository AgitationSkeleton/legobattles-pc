#!/usr/bin/env python3
"""`UI/*.bin` — the screen layout container.

## What is decoded

The container and the widget taxonomy, validated on all 132 screen layouts:
header, widget count, screen id, the offset table giving every record's extent,
and **each record's type**, taken from the high nibble of its first byte and
named from the class the engine constructs for it.

The **individual fields inside a record are still not named.** The type and the
extent are known; what the bytes mean within a `ButtonWidget` is not. That is a
smaller gap than it sounds -- a consumer can now identify and iterate widgets --
but it is a gap, and this file does not pretend otherwise.

## How the format was obtained

Not by guessing. `docs/44` records three structural hypotheses tested against
all 134 files, all three wrong, and the trap that made two of them look 31%
right: the population contains 41 empty files where any `size == 16` rule is
trivially true.

The format came from reading `Battles::User::ScreenType::virtual_08`, the base
implementation every screen calls before wiring up its own widgets:

    if (buf == 0) { this->field_14 = 0; return; }
    this->field_1C = buf;              // used IN PLACE, never copied
    id = buf[3];
    if (id < 0x62) screen_registry[id] = this;
    count = buf[2];
    base  = buf + 0x0C + count * 4;    // widget records follow the table

That is the whole container, and `buf[2]` being the count explains an
observation `docs/44` had recorded without understanding: byte 1 equals byte 2
in 132 of 134 files.

## The layout

    0x00  u8    version -- 6 in 107 files, 4 in 25
    0x01  u8    equals the count in every screen layout
    0x02  u8    widget count
    0x03  u8    screen id; indexes a 98-entry registry of live screens
    0x04  u8[8] not read by the base parser
    0x0C  u32[count]  widget offsets, relative to 0x0C + count*4
                      offsets[0] is always 0

Each record's first byte carries the widget type in its high nibble, and the
record's size is a function of that type.

`UI/FE/UIMgrData.bin` and `UI/Game/UIMgrData.bin` are **not** screen layouts.
They are 58 KB and 65 KB, carry version byte `0xFD`, and are excluded -- a
manager blob, not a screen.

Usage:
    python tools/lb_ui.py
    python tools/lb_ui.py --tsv docs/data/ui_layouts.tsv
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

#: Widget classes, by the type code in the HIGH NIBBLE of a record's byte 0.
#: Recovered from the 14-arm jump table in ScreenType::virtual_08 at
#: 0x020E1958: each arm's constructor stores its class's vtable pointer, and
#: docs/07's typeinfos name it. Codes 0, 2, 7 and 10 dispatch to the default
#: arm and are unused -- and no record in the cartridge carries one, which is
#: the check that the nibble really is the type. See docs/47.
WIDGET_TYPES = {
    1: "ButtonWidget",
    3: "ComboWidget",
    4: "PaletteButtonWidget",
    5: "SpriteWidget",
    6: "SlidingSpriteWidget",
    8: "ScrollWidget",
    9: "RadioButtonWidget",
    11: "TextWidget",
    12: "BackgroundWidget",
    13: "ListWidget",
}

#: Record size is a function of type, established from the data and agreeing
#: with the jump table derived from the code -- two independent sources.
#: TextWidget and SpriteWidget share 32 bytes; every other size is unique.
WIDGET_SIZES = {
    "BackgroundWidget": 28, "TextWidget": 32, "SpriteWidget": 32,
    "ScrollWidget": 36, "ListWidget": 40, "ButtonWidget": 52,
    "RadioButtonWidget": 56,
}

#: Not screen layouts: a UI-manager blob with its own version byte.
EXCLUDED = {"UI/FE/UIMgrData.bin", "UI/Game/UIMgrData.bin"}

HEADER = 0x0C
MAX_SCREEN_ID = 0x62

#: The 32-byte header every widget record begins with, read field-by-field by
#: each widget constructor -- ButtonWidget's is func_020E53A8, which copies it
#: into a 0x60-byte block verbatim. The four s16 at +0x04 are a bounding
#: rectangle: x2 >= x1 and y2 >= y1 holds on 1062 of 1062 records, where a
#: random 8-byte field would satisfy both about a quarter of the time.
#:
#: Coordinates run -64..376 vertically. The DS's two 256x192 screens stacked
#: give a 0..383 space, so a widget can sit on either screen or be staged off
#: the edge; this is NOT a bug in the reading.
#: The rectangle ends at +0x0C and the resource id at +0x1A, so both fit
#: inside even the smallest record (BackgroundWidget, 28 bytes). Requiring the
#: full 0x20 header excluded exactly those 81 records from the rectangle check
#: -- a self-inflicted blind spot that made the sample look like 981 of 1062.
WIDGET_RECT_END = 0x0C
WIDGET_RES_END = 0x1A

#: 0xFFFF at +0x18 means "none" -- ButtonWidget's constructor tests for it
#: explicitly before allocating. 498 of 1062 records carry the sentinel.
NO_RESOURCE = 0xFFFF


def parse(data):
    """Return (version, count, screen_id, [(offset, size), ...]) or raise."""
    if len(data) < HEADER:
        raise ValueError("shorter than the header")
    version, second, count, screen_id = data[0], data[1], data[2], data[3]
    if len(data) < HEADER + 4 * count:
        raise ValueError(f"count {count} overruns the file")
    offs = struct.unpack_from("<%dI" % count, data, HEADER) if count else ()
    if count:
        if offs[0] != 0:
            raise ValueError(f"offsets[0] is {offs[0]}, expected 0")
        if any(offs[i] >= offs[i + 1] for i in range(count - 1)):
            raise ValueError("offsets are not strictly increasing")
    base = HEADER + 4 * count
    if count and base + offs[-1] >= len(data):
        raise ValueError("last widget starts past the end of the file")
    widgets = []
    for i, o in enumerate(offs):
        start = base + o
        end = base + offs[i + 1] if i + 1 < count else len(data)
        kind = (data[start] >> 4) & 0xF
        rect = None
        res = None
        if end - start >= WIDGET_RECT_END:
            rect = struct.unpack_from("<4h", data, start + 4)
        if end - start >= WIDGET_RES_END:
            res = struct.unpack_from("<H", data, start + 0x18)[0]
        widgets.append((start, end - start, kind, WIDGET_TYPES.get(kind),
                        rect, res))
    return version, second, count, screen_id, widgets


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

    with open(os.path.join(args.extract, "files.json"), encoding="utf-8") as fh:
        recs = json.load(fh)
    rows, failed, skipped = [], [], []
    widths = {}
    for r in sorted(recs, key=lambda x: x.get("path") or ""):
        path = r.get("path") or ""
        if not path.lower().endswith(".bin"):
            continue
        if path in EXCLUDED:
            skipped.append(path)
            continue
        full = os.path.join(args.extract, "plain", path.replace("/", os.sep))
        if not os.path.exists(full):
            continue
        with open(full, "rb") as fh:
            data = fh.read()
        try:
            version, second, count, screen_id, widgets = parse(data)
        except ValueError as e:
            failed.append((path, str(e)))
            continue
        rows.append({"path": path, "size": len(data), "version": version,
                     "count": count, "screen_id": screen_id,
                     "second_matches_count": second == count,
                     "widgets": widgets})
        for _, w, _k, _n, _r, _s in widgets:
            widths[w] = widths.get(w, 0) + 1

    total = len(rows) + len(failed)
    print(f"{len(rows)} of {total} screen layouts parse "
          f"({len(skipped)} excluded: {', '.join(skipped)})")
    for p, why in failed:
        print(f"   FAIL {p}: {why}")
    print(f"byte 1 equals the widget count in "
          f"{sum(1 for r in rows if r['second_matches_count'])} of {len(rows)}")
    ids = [r["screen_id"] for r in rows]
    print(f"screen ids: {len(set(ids))} distinct, "
          f"{min(ids)}..{max(ids)}, all below 0x{MAX_SCREEN_ID:02X}: "
          f"{all(i < MAX_SCREEN_ID for i in ids)}")
    dup = [i for i in set(ids) if ids.count(i) > 1]
    print(f"screen ids used by more than one file: {len(dup)}")
    print(f"total widgets: {sum(r['count'] for r in rows)}")
    import collections as _c
    kinds = _c.Counter(k for r in rows for _, _, k, _n, _r, _s in r["widgets"])
    unnamed = [k for k in kinds if k not in WIDGET_TYPES]
    print(f"widget sizes: {len(widths)} distinct")
    print(f"widget types present: {len(kinds)}; every one is named: "
          f"{not unnamed}" + (f" (unnamed: {unnamed})" if unnamed else ""))
    for k, n in sorted(kinds.items()):
        print(f"   {k:>3}  {n:>5}  {WIDGET_TYPES.get(k, '*** UNMAPPED ***')}")
    bad = [(r["path"], nm, w) for r in rows for _, w, _k, nm, _r, _s in r["widgets"]
           if nm and WIDGET_SIZES.get(nm) not in (None, w)]
    print(f"records whose size disagrees with their type: {len(bad)}")
    rects = [rc for r in rows for _, _, _k, _n, rc, _s in r["widgets"] if rc]
    wf = sum(1 for x1, y1, x2, y2 in rects if x2 >= x1 and y2 >= y1)
    print(f"bounding rectangles well-formed (x2>=x1, y2>=y1): "
          f"{wf} of {len(rects)}")
    ys = [v for rc in rects for v in (rc[1], rc[3])]
    print(f"vertical extent {min(ys)}..{max(ys)} "
          f"(two stacked 256x192 screens span 0..383)")
    none = sum(1 for r in rows for _, _, _k, _n, _rc, sres in r["widgets"]
               if sres == NO_RESOURCE)
    print(f"records with the 0x{NO_RESOURCE:04X} 'none' resource at +0x18: {none}")

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["path", "size", "version", "widget_count",
                               "screen_id", "widgets", "rects"]) + nl)
            for r in rows:
                fh.write(tab.join([
                    r["path"], str(r["size"]), str(r["version"]),
                    str(r["count"]), str(r["screen_id"]),
                    ";".join("0x%X+%d:%s" % (o, w, nm or "type%d" % k)
                             for o, w, k, nm, _rc, _s in r["widgets"]),
                    ";".join(("%d,%d,%d,%d" % rc) if rc else ""
                             for _o, _w, _k, _n, rc, _s in r["widgets"]),
                ]) + nl)
        print(f"wrote {args.tsv} ({len(rows)} layouts)")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
