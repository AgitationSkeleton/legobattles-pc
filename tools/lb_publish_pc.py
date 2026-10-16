#!/usr/bin/env python3
"""Refresh the public source-and-release repository from this one.

    python tools/lb_publish_pc.py [--dest ../legobattles-pc] [--add-doc docs/NNN-x.md ...] [--commit]

legobattles-pc is the PC port's own repository: the documentation of how the
game works, every line of the port, the tools and the lobby servers -- and
none of this repository's working evidence (32,000 screenshots, session
recordings, logs, the master server's database, private planning notes).

WHAT GOES: the COMMITTED content (`git archive HEAD`, so nothing half-edited
in the working tree leaks) of the paths in INCLUDE, minus EXCLUDE, plus
publish/legobattles-pc/ (its README, notices, licences and GitHub workflows)
at the root. Everything else in the destination except .git and docs/ is
removed first, so a file deleted here is deleted there too.

THE DOCUMENTATION IS CURATED, NOT SYNCED (docs/395). The public docs/ is its
own selection, edited for publication, and is left as it is. A new document is
added only when it is named with --add-doc, and it is checked like everything
else.

It refuses to publish anything that looks like the game -- a .nds, .sav, .bin,
BIOS dump, the USA-to-Europe delta, or out/ -- and anything carrying
personal material: local paths and machine names, quoted feedback, private
planning documents and process records (LEAKS below).
"""

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

INCLUDE = ["port", "src", "config", "services", "tools", ".gitattributes"]
EXCLUDE = [
    r"^port/runner/\.hookstamps\.json$",
    r"^services/master-server/master\.sqlite3$",
    r"^services/master-server/logs/",
    r"(^|/)node_modules/",
    r"\.lbdelta$",
    r"(^|/)SESSION[-_]LOG",
    r"(?i)(^|/)(00-)?mandate",
]
# (BIOS and firmware dumps are .rom/.bin files; a document about BIOS banks
# is not one.)
FORBIDDEN = re.compile(r"(\.nds|\.sav|\.bin|\.rom|\.lbdelta|\.sqlite3)$|(^|/)out/", re.I)
# Personal material. Deliberately specific: "owner" alone is the game's word
# for an entity's team, and "session" its word for a recording.
LEAKS = re.compile(
    r"Claude_RTSGames|ADMINI~1|\\Users\\Administrator|/Users/Administrator|"
    r"\b[A-Z]:[\\/](Users|RETools|LSW2Project|Claude)|rangerquake|AlfaBlastr|"
    r"\bthe (project )?owner\b ?(asked|said|saw|wrote|reported|photographed|noticed)|"
    r"\bthe owner, 20\d\d|\bthe owner:\s|\bowner'?s? (replay|route|request|report|playtest|screenshot|"
    r"monitor|surface|setup|demo|machine|desktop)|\bplaytest item|\bowner item|"
    r"\bmandate|dummy (head|screen|HDMI|display)|\bthe user's\b|"
    r"(?-i:(?<![A-Z_])SESSION[-_]LOG)|docs-ledger|ledger row|audit-2026-09-22|Written for: the project owner",
    re.I)
TEXT = re.compile(r"\.(md|txt|py|sh|ps1|bat|cmd|c|cc|cpp|h|hpp|inc|js|mjs|json|jsonc|"
                  r"toml|yml|yaml|tsv|csv|java|cmake)$|(^|/)(CMakeLists\.txt|\.gitignore)$", re.I)
TEMPLATE = os.path.join(ROOT, "publish", "legobattles-pc")


def git(*args, cwd=ROOT, check=True):
    return subprocess.run(["git", *args], cwd=cwd, check=check, capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", default=os.path.join(os.path.dirname(ROOT), "legobattles-pc"))
    ap.add_argument("--add-doc", action="append", default=[],
                    help="a committed doc to add to the public docs/ (repeatable)")
    ap.add_argument("--commit", action="store_true", help="git init/commit in the destination")
    args = ap.parse_args()
    dest = os.path.abspath(args.dest)
    os.makedirs(dest, exist_ok=True)

    head = git("rev-parse", "--short=8", "HEAD").stdout.decode().strip()
    tar = git("archive", "--format=tar", "HEAD", "--", *INCLUDE, *args.add_doc).stdout
    for name in os.listdir(dest):
        if name in (".git", "docs"):
            continue
        p = os.path.join(dest, name)
        shutil.rmtree(p) if os.path.isdir(p) and not os.path.islink(p) else os.remove(p)

    kept = skipped = 0
    written = []
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        for m in t.getmembers():
            if not m.isfile():
                continue
            if any(re.search(x, m.name) for x in EXCLUDE):
                skipped += 1
                continue
            out = os.path.join(dest, m.name)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as fh:
                fh.write(t.extractfile(m).read())
            written.append(m.name)
            kept += 1
    for base, _, files in os.walk(TEMPLATE):
        for f in files:
            src = os.path.join(base, f)
            rel = os.path.relpath(src, TEMPLATE)
            out = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copy2(src, out)
            kept += 1
    print("%s: %d file(s) from %s, %d excluded; docs/ kept as curated" % (dest, kept, head, skipped))

    bad, leaks = [], []
    for base, dirs, files in os.walk(dest):
        dirs[:] = [d for d in dirs if d != ".git"]
        for f in files:
            p = os.path.join(base, f)
            rel = os.path.relpath(p, dest).replace(os.sep, "/")
            if FORBIDDEN.search(rel):
                bad.append(rel)
            if rel.startswith("LICENSES/") or not TEXT.search(rel):
                continue
            with open(p, encoding="utf-8", errors="replace") as fh:
                for k, line in enumerate(fh, 1):
                    if LEAKS.search(line) and "lb_publish_pc.py" not in rel:
                        leaks.append("%s:%d  %s" % (rel, k, line.strip()[:120]))
    if bad:
        print("REFUSING: game-like files in the export:\n  " + "\n  ".join(bad), file=sys.stderr)
        return 1
    if leaks:
        print("REFUSING: personal material in the export (fix it at the source):\n  " +
              "\n  ".join(leaks), file=sys.stderr)
        return 1

    if args.commit:
        if not os.path.isdir(os.path.join(dest, ".git")):
            git("init", "-b", "main", cwd=dest)
        git("add", "--all", cwd=dest)
        if git("diff", "--cached", "--quiet", cwd=dest, check=False).returncode == 0:
            print("nothing changed")
            return 0
        msg = ("Sync from legobattles-decomp %s\n\n"
               "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>\n" % head)
        git("commit", "-q", "-m", msg, cwd=dest)
        print(git("log", "--oneline", "-1", cwd=dest).stdout.decode().strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
