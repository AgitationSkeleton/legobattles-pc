#!/usr/bin/env python3
"""Write an index of screenshots/: what each captured run was testing, and when.

Captured screenshots and logs are kept for later review, by timestamp and
description of what was being tested. A folder of 690 directories named
`exp-fxband` and `mp-mp3g` does not answer that on its own.

DATES DO NOT COME FROM mtime, and that is this script's whole reason for
existing. `tools/lb_prune_shots.py` converted 7899 .ppm dumps to .png to reclaim
3.2 GB, which rewrote every one of those files and set their mtime to the
conversion time. So the filesystem now says every run happened on the same
morning, which is false. The date a run's files were first COMMITTED is
recoverable and honest, so that is what is used; runs not yet committed are
marked as current, uncommitted work.

    python tools/lb_shots_index.py
"""

import collections
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.path.join(ROOT, "screenshots")
OUT = os.path.join(ROOT, "docs", "SCREENSHOTS-INDEX.md")

# What a run was testing, by label prefix. Longest prefix wins, so a specific
# run can override its family.
WHAT = [
    ("mp-mp3", "three-player multiplayer: join, seating, sync"),
    ("mp-mpreg", "multiplayer regression after the touch/camera changes"),
    ("mp-mplatch", "multiplayer music latch (faction from the local player)"),
    ("mp-ms", "master-server directory: browse, announce, join by code"),
    ("mp-", "two-instance multiplayer over the local wireless bridge"),
    ("mp-regress3p", "three-player regression after the panel and patch changes"),
    ("exp-paneltest", "finding out why the overlay tab click never worked"),
    ("exp-enhtab", "the Enhancements tab, opened by name with LB_PANEL_TAB"),
    ("exp-enhcombo", "the Transport Occupants combo, showing Destroy them greyed"),
    ("exp-tab", "one settings tab, opened by name and photographed"),
    ("exp-savexport", "pressing Export live save with LB_SAVE_PATH set"),
    ("exp-saveimport", "pressing Import, and the retag it reports"),
    ("exp-mpbrowse", "Browse lobbies pressed against a live master server"),
    ("exp-captest", "a unit cap watched changing a byte at 0x02126D94"),
    ("exp-demorec", "recording a demo through LB_DEMO_REC"),
    ("exp-demoplay", "replaying it with --no-route, so the demo is the only input"),
    ("exp-killtest", "the reduced three-call WaitingToDie chain, under LB_KILLTEST"),
    ("exp-mapsel", "the Free Play map-select screen, to locate its arrows"),
    ("exp-maplist", "cycling the Free Play map list to find a naval map"),
    ("exp-siege", "hunting a model-backed unit with the economy cheats on"),
    ("exp-naval", "Free Play on River Crossing, where transports are needed"),
    ("exp-model", "a model-backed unit (cat 5/6) in a widened column"),
    ("exp-fxband", "a named effect in a widened column, with the fog state"),
    ("exp-fxpic", "the widened surface photographed with a band effect"),
    ("exp-fxwalk", "walking a unit into the band so it carries effects there"),
    ("exp-fxclear", "effects on unfogged ground in a widened column"),
    ("exp-fxpan", "panning to where the effects are, then sampling"),
    ("exp-fxall", "every effect sampled unfiltered, to see class positions"),
    ("exp-kill", "the WaitingToDie guest-call path (hotwire transport mode 1)"),
    ("exp-ord", "commanding a unit to a destination in the widened band"),
    ("exp-fix", "the order-destination fix (byte path -> 16-bit shadows)"),
    ("exp-thr", "threshold bisect: where a band order stops working"),
    ("exp-neg", "left-band order: a negative destination"),
    ("exp-deflt", "the order fix applying by default under Enhanced"),
    ("exp-regsel", "regression: band selection still 4 of 4 with the fix on"),
    ("exp-hw", "the hotwire-anything cheat, patched vs stock"),
    ("exp-band", "clicking and dragging in the widened columns"),
    ("exp-sizes", "magnification and field of view across window sizes"),
    ("exp-", "single-instance gameplay probe"),
]


def describe(name):
    best = ""
    text = "captured run"
    for prefix, what in WHAT:
        if name.startswith(prefix) and len(prefix) > len(best):
            best, text = prefix, what
    return text


def git_add_dates():
    """First commit date per screenshots/<dir>, which mtime can no longer give."""
    try:
        out = subprocess.run(
            ["git", "log", "--diff-filter=A", "--format=%cs", "--name-only",
             "--", "screenshots"],
            cwd=ROOT, capture_output=True, text=True, timeout=180).stdout
    except Exception:                                    # noqa: BLE001
        return {}
    dates, date = {}, None
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        if len(line) == 10 and line[4] == "-":
            date = line
            continue
        parts = line.split("/")
        if len(parts) > 1 and parts[1] not in dates:
            dates[parts[1]] = date
    return dates


def main():
    if not os.path.isdir(SHOTS):
        print("no screenshots/ directory")
        return 1
    dates = git_add_dates()

    runs = []
    for name in sorted(os.listdir(SHOTS)):
        path = os.path.join(SHOTS, name)
        if not os.path.isdir(path):
            continue
        size = files = pngs = logs = 0
        for base, _d, fs in os.walk(path):
            for f in fs:
                try:
                    size += os.path.getsize(os.path.join(base, f))
                except OSError:
                    pass
                files += 1
                if f.endswith(".png"):
                    pngs += 1
                elif f.endswith(".log"):
                    logs += 1
        runs.append({"name": name, "date": dates.get(name, "(this session)"),
                     "size": size, "files": files, "pngs": pngs, "logs": logs})

    by_date = collections.defaultdict(list)
    for r in runs:
        by_date[r["date"]].append(r)

    total = sum(r["size"] for r in runs)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("# Captured runs, by date and what they were testing\n\n")
        fh.write("Generated by `tools/lb_shots_index.py`; re-run it after any "
                 "batch of runs.\n\n")
        fh.write("**%d runs, %.2f GB.** Every frame is a PNG: "
                 "`tools/lb_prune_shots.py` converted 7899 uncompressed `.ppm` "
                 "dumps (3.33 GB) to 0.13 GB of lossless PNG, which is also the "
                 "format that previews in a file browser.\n\n" %
                 (len(runs), total / 1073741824.0))
        fh.write("**Dates come from git, not the filesystem.** That conversion "
                 "rewrote every file, so mtime now reports the conversion time "
                 "for all of them. The date below is when a run's files were "
                 "first committed; runs from the current working session are "
                 "marked as such.\n\n")
        for date in sorted(by_date, reverse=True):
            rs = sorted(by_date[date], key=lambda r: r["name"])
            sz = sum(r["size"] for r in rs)
            fh.write("## %s — %d runs, %.0f MB\n\n" %
                     (date, len(rs), sz / 1048576.0))
            fh.write("| run | what it was testing | files | png | size |\n")
            fh.write("|---|---|---|---|---|\n")
            for r in rs:
                fh.write("| `%s` | %s | %d | %d | %.1f MB |\n" %
                         (r["name"], describe(r["name"]), r["files"],
                          r["pngs"], r["size"] / 1048576.0))
            fh.write("\n")
    print("wrote %s (%d runs, %.2f GB)" % (OUT, len(runs),
                                           total / 1073741824.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
