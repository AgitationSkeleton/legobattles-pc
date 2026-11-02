#!/usr/bin/env python3
"""Build the settings menu's search index (port/runner/src/lb_menu_index.inc).

The menu's pages are plain drawing functions (lb_overlay.cpp, menu_headers()),
so what each page holds is read from the source: for every page, its widgets'
labels (Checkbox, Combo, Slider..., InputInt/Text, RadioButton, Button,
SeparatorText, CollapsingHeader, TreeNode) and the help() text that follows
each. The search box (draw_menu_search) matches against these and jumps to the
page; help() highlights the widget it follows.

Run after changing a page's widgets:

    python tools/lb_menu_index.py

The output is committed; a stale index only misses or mis-places a result.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "port", "runner", "src")
OVERLAY = os.path.join(SRC, "lb_overlay.cpp")
OUT = os.path.join(SRC, "lb_menu_index.inc")

WIDGETS = ("Checkbox", "Combo", "BeginCombo", "SliderInt", "SliderFloat", "SliderInt2", "InputInt",
           "InputFloat", "InputText", "InputTextWithHint", "RadioButton", "Button", "SmallButton",
           "SeparatorText", "CollapsingHeader", "TreeNode", "ColorEdit3", "ColorEdit4", "DragFloat", "DragInt")


def strip_comments(s):
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            out.append(s[i:j + 1])
            i = j + 1
        elif s.startswith("//", i):
            j = s.find("\n", i)
            i = n if j < 0 else j
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == "'":
            j = i + 1
            while j < n and s[j] != "'":
                j += 2 if s[j] == "\\" else 1
            out.append(s[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def literal_run(s, i):
    """Adjacent string literals from s[i] (which must be '"'): (text, end)."""
    parts = []
    while True:
        while i < len(s) and s[i] in " \t\r\n":
            i += 1
        if i >= len(s) or s[i] != '"':
            break
        j = i + 1
        buf = []
        while j < len(s) and s[j] != '"':
            if s[j] == "\\" and j + 1 < len(s):
                e = s[j + 1]
                buf.append({"n": " ", "t": " ", '"': '"', "\\": "\\"}.get(e, e))
                j += 2
            else:
                buf.append(s[j])
                j += 1
        parts.append("".join(buf))
        i = j + 1
    return "".join(parts), i


def body_of(src, name):
    m = re.search(r"\n(?:static\s+)?void\s+" + re.escape(name) + r"\s*\(\s*\)\s*\{", src)
    if not m:
        return None
    i, depth = m.end(), 1
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[m.end():i - 1]


def expand(src, fn, seen, depth=0):
    """A page's body with the helpers it calls (name(); or name(args);, defined
    in this file as void) spliced in where they are called, three levels deep."""
    body = body_of(src, fn)
    if body is None or depth > 3:
        return body or ""
    seen.add(fn)
    out, last = [], 0
    for m in re.finditer(r"\b([a-z_][a-z0-9_]*)\s*\([^;{}]*\)\s*;", body):
        name = m.group(1)
        if name in seen or name in ("help", "badge", "guide", "tab_begin", "tab_end", "host_note"):
            continue
        if not re.search(r"\n(?:static\s+)?void\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", src):
            continue
        sub = body_of_any(src, name)
        if sub is None:
            continue
        seen.add(name)
        out.append(body[last:m.end()])
        out.append("\n" + sub + "\n")
        last = m.end()
    out.append(body[last:])
    return "".join(out)


def body_of_any(src, name):
    m = re.search(r"\n(?:static\s+)?void\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", src)
    if not m:
        return None
    i, depth = m.end(), 1
    while i < len(src) and depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    return src[m.end():i - 1]


def entries_of(body):
    """(label, help) for each widget in a page's body, in order."""
    out = []
    pat = re.compile(r"ImGui::(" + "|".join(WIDGETS) + r")\s*\(\s*\"|\bhelp\s*\(\s*\"")
    for m in pat.finditer(body):
        text, _ = literal_run(body, m.end() - 1)
        if m.group(1):
            label = text.split("##")[0].strip()
            if not label or len(label) < 3:
                continue
            out.append([label, "", m.group(1)])
        elif out and not out[-1][1]:
            out[-1][1] = " ".join(text.split())
    return out


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    src = strip_comments(open(OVERLAY, encoding="utf8").read())
    mh = body_of(src, "menu_headers")
    if mh is None:
        m = re.search(r"menu_headers\s*\(\s*\)\s*\{", src)
        i, depth = m.end(), 1
        while depth:
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            i += 1
        mh = src[m.end():i - 1]
    # {"Header", {{"Section", "id", fn}, ...}}
    rows = []
    header = None
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*\{|\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*\}', mh):
        if m.group(1) is not None:
            header = m.group(1)
        elif header:
            rows.append((header, m.group(2), m.group(4)))
    if not rows:
        sys.exit("no menu sections found in menu_headers()")
    lines = ["// Generated by tools/lb_menu_index.py from lb_overlay.cpp; do not edit by hand.",
             "// {header, section, label, help}"]
    n = 0
    for header, section, fn in rows:
        lines.append('{"%s", "%s", "%s", ""},' % (esc(header), esc(section), esc(section)))
        body = expand(src, fn, set())
        if not body:
            continue
        seen = set()
        for label, helptext, _kind in entries_of(body):
            if label in seen:
                continue
            seen.add(label)
            lines.append('{"%s", "%s", "%s", "%s"},' % (esc(header), esc(section), esc(label), esc(helptext[:240])))
            n += 1
    data = "\n".join(lines) + "\n"
    old = open(OUT, encoding="utf8").read() if os.path.exists(OUT) else ""
    if old != data:
        open(OUT, "w", encoding="utf8", newline="\n").write(data)
    print("%d sections, %d entries -> %s" % (len(rows), n, os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()
