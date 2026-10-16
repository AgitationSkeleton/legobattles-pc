#!/usr/bin/env python3
"""A distributable test build: a clean folder and a zip to hand to players.

    python tools/lb_dist.py [--version playtest-2026-10-16] [--out dist]

It is lb_deploy.py's layout into a FRESH folder -- nothing of the build
machine's (no settings.json, saves, logs, recordings or ROM) -- plus version.txt, which
the game reports to the master server so the lobby log says which build a
player runs. The ROM is the player's own: the first start asks for it
(Europe or USA, docs/394). The USA-to-Europe conversion delta must exist
first (tools/lb_region_delta.py); a build without it would refuse USA ROMs.

The zip goes beside the folder: dist/LegoBattles-PC-<version>.zip, with
THIRD_PARTY_NOTICES.md and LICENSES/ inside, and the runner's corresponding
source beside it as LegoBattles-PC-<version>-runner-source.zip.
"""

import argparse
import datetime
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lb_deploy  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="playtest-" + datetime.date.today().isoformat())
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = ap.parse_args()

    delta = os.path.join(ROOT, "port", "runner", "build", "data", "usa_to_eur.lbdelta")
    if not os.path.exists(delta):
        print("missing %s\n  py -3.13 tools/lb_region_delta.py %s" % (delta, delta),
              file=sys.stderr)
        return 2
    try:
        commit = subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], cwd=ROOT,
                                capture_output=True, text=True).stdout.strip()
    except OSError:
        commit = ""
    name = "LegoBattles-PC-" + args.version
    dest = os.path.join(args.out, name)
    if os.path.exists(dest):
        shutil.rmtree(dest)
    if lb_deploy.main(["--dest", dest]) != 0:
        print("deploy failed", file=sys.stderr)
        return 1
    # A playtest build talks to the public lobby server; the self-hosted one
    # is for operators, not players.
    for exe in ("lb_master_server.exe", "lb_master_server_gui.exe"):
        p = os.path.join(dest, exe)
        if os.path.exists(p):
            os.remove(p)
    version = args.version + (" (" + commit + ")" if commit else "")
    with open(os.path.join(dest, "version.txt"), "w", newline="\r\n") as fh:
        fh.write(version + "\n")
    for leftover in ("settings.json", "imgui.ini"):
        assert not os.path.exists(os.path.join(dest, leftover)), leftover
    # Credits and licences (docs/395): the executable links GPL-3.0-or-later
    # melonDS code and is built on ndsrecomp, so the notices and every licence
    # text travel with it.
    pub = os.path.join(ROOT, "publish", "legobattles-pc")
    shutil.copy2(os.path.join(pub, "THIRD_PARTY_NOTICES.md"), dest)
    shutil.copytree(os.path.join(pub, "LICENSES"), os.path.join(dest, "LICENSES"))
    zpath = os.path.join(args.out, name + ".zip")
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, dirs, files in os.walk(dest):
            rel = os.path.relpath(base, args.out)
            if not files and not dirs:
                z.writestr(rel.replace(os.sep, "/") + "/", "")
            for f in files:
                z.write(os.path.join(base, f), os.path.join(rel, f))
    print("%s\n  %s (%.1f MB)" % (version, zpath, os.path.getsize(zpath) / 1e6))
    # The runner's corresponding source (GPL-3.0-or-later): the assembled tree
    # the executable was compiled from -- upstream ndsrecomp's cmake/,
    # recompiler/, runner/ and vendor/ with this port's hooks and sources
    # applied. The lifted title code is made from the player's cartridge and is
    # not included.
    tree = os.path.join(ROOT, "port", "runner", "tree")
    spath = os.path.join(args.out, name + "-runner-source.zip")
    if os.path.isdir(tree):
        if os.path.exists(spath):
            os.remove(spath)
        with zipfile.ZipFile(spath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for base, dirs, files in os.walk(tree):
                dirs[:] = [d for d in dirs if d != "build"]
                for f in files:
                    p = os.path.join(base, f)
                    z.write(p, os.path.join(name + "-runner-source",
                                            os.path.relpath(p, tree)))
            for f in ("THIRD_PARTY_NOTICES.md",):
                z.write(os.path.join(pub, f), os.path.join(name + "-runner-source", f))
            for f in os.listdir(os.path.join(pub, "LICENSES")):
                z.write(os.path.join(pub, "LICENSES", f),
                        os.path.join(name + "-runner-source", "LICENSES", f))
        print("  %s (%.1f MB)" % (spath, os.path.getsize(spath) / 1e6))
    else:
        print("  no port/runner/tree: build first for the runner-source zip", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
