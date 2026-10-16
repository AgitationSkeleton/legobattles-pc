"""Is every ImGui setting still wired?

    python tools/lb_wiring.py

The requirement is that nothing in the settings
panel is "read, but not wired" or unwired. That has been re-derived by
hand several times, and the hand version
is easy to get wrong in one particular way: grepping for an accessor name
also matches the COMMENTS that explain why an accessor was removed.
`lb_setting_fit_all` is deleted and mentioned in six comments, and a
naive check reports it as an orphan every time.

So this strips comments before looking, and reports:

  * the badge census, which is the panel's own claim about itself;
  * every `lb_setting_*` / `lb_binding_*` accessor DEFINED in
    lb_overlay.cpp that no other translation unit references -- the
    category that found the lb_overlay_shutdown bug, where a complete
    exported function had no caller anywhere;
  * save/load asymmetry between lb_settings_save and lb_settings_load.

Exit status is 1 if anything is unwired, so it can gate a commit.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "port", "runner", "src")
OVERLAY = os.path.join(SRC, "lb_overlay.cpp")
HOOKS = os.path.join(ROOT, "port", "runner", "apply_hooks.py")


def strip_comments(text):
    """Drop // and /* */ so prose about a removed symbol is not a hit."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def main():
    overlay = read(OVERLAY)
    overlay_code = strip_comments(overlay)
    problems = []

    print("badge census (the panel's own claim about itself)")
    total = 0
    for name in ("WIRED", "READ", "UNSEEN", "BLOCKED"):
        n = len(re.findall(r"\bbadge\(%s\)" % name, overlay_code))
        total += n
        print("    %-8s %d" % (name, n))
    print("    %-8s %d call sites" % ("total", total))
    read_n = len(re.findall(r"\bbadge\(READ\)", overlay_code))
    if read_n:
        problems.append("%d control(s) still badged READ" % read_n)

    # Accessors DEFINED here, and whether any other TU names them. The hooks
    # file counts: it inserts calls into the generated tree.
    defined = sorted(set(re.findall(
        r"\b(lb_setting_[a-z0-9_]+|lb_binding_[a-z0-9_]+|lb_overlay_[a-z0-9_]+)"
        r"\s*\(", strip_comments(overlay))))
    others = {}
    for fn in sorted(os.listdir(SRC)):
        if fn.endswith((".cpp", ".inc")) and fn != "lb_overlay.cpp":
            others[fn] = strip_comments(read(os.path.join(SRC, fn)))
    others["apply_hooks.py"] = read(HOOKS)

    # Deliberately-unused exports, each with the reason it is kept. An entry
    # here is a claim that someone checked; the comment at the definition is
    # where the evidence lives.
    ALLOWED_ORPHANS = {
        # lb_overlay_event already returns io.WantCaptureKeyboard for KEYDOWN /
        # KEYUP / TEXTINPUT and the frontend does `if (lb_overlay_event(&e))
        # continue;`, so keyboard capture is complete without a second gate.
        # The mouse needs one because set_touch_from_mouse is reachable from
        # several call sites; there is no keyboard analogue of that function.
        "lb_overlay_wants_keyboard",
    }
    orphans = []
    for sym in defined:
        if sym in ALLOWED_ORPHANS:
            continue
        if not any(sym in body for body in others.values()):
            orphans.append(sym)
    print("\nexports defined in lb_overlay.cpp with no reference in any other "
          "translation unit")
    if orphans:
        for sym in orphans:
            print("    ORPHAN  %s" % sym)
        problems.append("%d exported symbol(s) have no caller" % len(orphans))
    else:
        print("    none")

    # Save/load symmetry. Keys are the string literals each function writes
    # or reads.
    def keys(fn_name):
        m = re.search(r"void %s\(void\)\s*\{(.*?)\n\}" % fn_name, overlay,
                      flags=re.S)
        if not m:
            return set()
        # THE TWO SIDES SPELL A KEY DIFFERENTLY, and matching only one of them
        # reports every key as asymmetric.
        #
        #   save:  snprintf(buf, ..., "  \"enhanced\": %s,\n", ...)
        #          json_put_str(o, "master_host", ...)
        #   load:  json_get_bool(s, "enhanced", &g_set.enhanced)
        #
        # So take escaped-quote keys AND the key argument of any json_ helper.
        # Matching bare quoted words instead sweeps up fopen modes ("wb",
        # "rb") and the JSON literals "true" and "false".
        body = strip_comments(m.group(1))
        found = set(re.findall(r'\\"([a-z_][a-z0-9_]*)\\"', body))
        found |= set(re.findall(
            r'json_\w+\(\s*\w+\s*,\s*"([a-z_][a-z0-9_]*)"', body))
        return found

    saved, loaded = keys("lb_settings_save"), keys("lb_settings_load")
    saved.discard("_comment")
    print("\nsettings.json symmetry")
    only_saved = sorted(saved - loaded)
    only_loaded = sorted(loaded - saved)
    if only_saved or only_loaded:
        for k in only_saved:
            print("    saved but never loaded:  %s" % k)
        for k in only_loaded:
            print("    loaded but never saved:  %s" % k)
        problems.append("settings.json save/load is asymmetric")
    else:
        print("    %d keys, symmetric" % len(saved))

    print()
    if problems:
        for p in problems:
            print("UNWIRED: %s" % p)
        return 1
    print("OK: nothing is read-but-not-wired.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
