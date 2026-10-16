#!/usr/bin/env python3
"""Parse `Sound/sound_data.sadl` — the game's own symbol names for its audio.

This file is not audio. It is a **C header**, 1,311 `#define` lines, shipped
inside the cartridge beside the 60 MB `sound_data.sdat`. It is the developers'
own naming for every wave archive, bank, sequence archive, stream and sound
effect in the game, and it is therefore the single best source of *the
vocabulary they used* — which turns out to name the unit roles, the structure
roles, the spell set and the behaviour events, in their words rather than
anyone's inference.

    WAVE_*        8    wave archives, by content group
    BANK_*      200    instrument banks — 60 unit, 60 structure, 34 spell,
                       34 colour-faction, plus group banks
    SEQARC_*    217    sequence archives
    STRM_*       98    streamed music
    SE_*        784    sound effects, one per entity per event
    PLAYER_*      2
    STRMPLAYER_*  2

Usage:
    python tools/lb_sound_symbols.py out/eur/plain/Sound/sound_data.sadl
        [--tsv docs/data/sound_symbols.tsv] [--roles]
"""

import argparse
import collections
import os
import re
import sys

DEFINE = re.compile(r"^#define\s+(\S+)\s+(\d+)\s*$")
FACTIONS = ["KING", "WIZARD", "PIRATE", "IMPERIAL", "EARTH", "ALIEN"]

#: The header, as it sits in the NitroFS.
SADL = "Sound/sound_data.sadl"

#: Namespaces that must be a contiguous bijection onto 0..N-1. `SE` is
#: deliberately absent: its numbering is archive-local, which is the whole
#: point of the pairing below.
CONTIGUOUS = ("WAVE", "BANK", "SEQARC", "STRM", "PLAYER", "STRMPLAYER")

#: The ten unit role slots, in the order the header lists them for every
#: faction. Some factions rename the slot -- EARTH and ALIEN call the builder
#: an ENGINEER -- but the slot is what the engine indexes.
UNIT_ROLES = ("HEROM", "HEROF", "BUILDER", "MELEE", "RANGED", "MOUNTED",
              "SIEGEONE", "SIEGETWO", "SIEGETHREE", "TRANSPORT")

#: The ten structure slots, likewise in header order.
STRUCTURE_SLOTS = ("BASE", "MILL", "MINE", "FARM", "BARRACKS", "VEHICLECENTER",
                   "TOWER", "TOWER2", "TOWER3", "SHIPYARD")

#: The `SE` names use their own shorthand where the `SEQARC` names do not.
#: Every one of these was read off the pairing rather than assumed: a category
#: prefix the SE side adds, one abbreviation, and two slots a faction renames.
#: Undoing them takes the name agreement from 103 of 217 to 205; the residue is
#: the tower copy-paste, which the duplicate count finds independently.
SHORTHAND = (
    ("SPELL_", ""),
    ("COLOURFACTION_", ""),
    ("PROJECTILE_", ""),
    ("_VC_", "_VEHICLECENTER_"),
    ("_BUILDER_", "_ENGINEER_"),
    ("_TRANSPORT_", "_TRANSPORTSHIP_"),
)


def namespace(name):
    """`SE_KING_MELEE_ONHIT` -> `SE`, `BANK_UNIT__KING_MELEE` -> `BANK`."""
    return name.split("_", 1)[0]


def expand(name):
    """Undo the `SE` side's shorthand so a name can be compared to a SEQARC."""
    for a, b in SHORTHAND:
        name = name.replace(a, b)
    return name


def segments(defs):
    """Cut the `SE` defines into archive-local runs at every reset to 0.

    The header never states which `SE` belongs to which `SEQARC`. It does not
    have to: `SE` numbering restarts at zero for each archive, so cutting the
    784 `SE` lines wherever the value returns to 0 recovers the grouping.

    That the cut yields **217 runs -- the exact `SEQARC` count** -- and that run
    *k* carries the name of `SEQARC` *k* are two facts nobody put in the file on
    purpose. They hold only if it really is emitted archive by archive, in
    order, which is what makes the pairing a reading and not a guess.
    """
    runs = []
    for name, value in defs:
        if not name.startswith("SE_"):
            continue
        if value == 0 or not runs:
            runs.append([])
        runs[-1].append((name[3:], value))
    return runs


def archives(defs):
    """[(seqarc id, seqarc name, [(se name, se index), ...])], in id order."""
    seq = sorted(((n[len("SEQARC__"):], v) for n, v in defs
                  if n.startswith("SEQARC__")), key=lambda kv: kv[1])
    runs = segments(defs)
    out = []
    for k, run in enumerate(runs):
        out.append((k, seq[k][0] if k < len(seq) else "", run))
    return out


def parse(text):
    out = []
    for line in text.splitlines():
        m = DEFINE.match(line)
        if m:
            out.append((m.group(1), int(m.group(2))))
    return out


def roles(defs, prefix):
    """{faction: [role, ...]} for BANK_UNIT / BANK_STRUCTURE."""
    rx = re.compile(r"^%s__(%s)_(.+)$" % (prefix, "|".join(FACTIONS)))
    out = collections.defaultdict(list)
    for name, _v in defs:
        m = rx.match(name)
        if m:
            out[m.group(1)].append(m.group(2))
    return out


def events(defs):
    """{event: count} over SE_<FACTION>_<ROLE>_<EVENT>."""
    rx = re.compile(r"^SE_(?:%s)_([A-Z0-9]+)_(.+)$" % "|".join(FACTIONS))
    out = collections.Counter()
    for name, _v in defs:
        m = rx.match(name)
        if m:
            out[m.group(2)] += 1
    return out


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--tsv")
    ap.add_argument("--events", help="write the SEQARC/SE pairing as TSV")
    ap.add_argument("--roles", action="store_true")
    args = ap.parse_args()

    with open(args.file, encoding="latin-1") as fh:
        text = fh.read()
    defs = parse(text)
    lines = len(text.splitlines())
    print("%d #defines from %d lines" % (len(defs), lines))
    if len(defs) != lines:
        print("WARNING: %d lines were not #defines" % (lines - len(defs)))

    ns = collections.defaultdict(list)
    for name, value in defs:
        ns[namespace(name)].append(value)
    print("   %-12s %5s  %-12s %-11s %s"
          % ("namespace", "count", "range", "contiguous", "unique"))
    for k in sorted(ns):
        vals = sorted(ns[k])
        contig = vals == list(range(vals[0], vals[0] + len(vals)))
        print("   %-12s %5d  %-12s %-11s %s"
              % (k, len(vals), "%d..%d" % (vals[0], vals[-1]),
                 "yes" if contig else "no",
                 "yes" if len(set(vals)) == len(vals) else "no"))
    bad = [k for k in CONTIGUOUS
           if k in ns and sorted(ns[k]) != list(range(len(ns[k])))]
    print("   namespaces required contiguous that are not: %d%s"
          % (len(bad), (" -- " + ", ".join(bad)) if bad else ""))

    # The pairing, and the two facts that make it a reading.
    pairs = archives(defs)
    nseq = len(ns.get("SEQARC", []))
    print()
    print("SE runs (cut at every reset to 0): %d   SEQARC entries: %d   %s"
          % (len(pairs), nseq, "MATCH" if len(pairs) == nseq else "MISMATCH"))
    raw = agree = 0
    residue = []
    for _k, owner, run in pairs:
        names = [n for n, _v in run]
        if any(n == owner or n.startswith(owner + "_") for n in names):
            raw += 1
            agree += 1
            continue
        wide = [expand(n) for n in names]
        if any(n == owner or n.startswith(owner + "_") for n in wide):
            agree += 1
        else:
            residue.append((owner, run[0][0]))
    print("runs whose SE names carry the SEQARC name: %d of %d raw, "
          "%d after shorthand" % (raw, len(pairs), agree))
    for owner, first in residue:
        print("   SEQARC %-28s SE %s" % (owner, first))

    seen = collections.Counter(n for _k, _o, run in pairs for n, _v in run)
    dup = {n: c for n, c in seen.items() if c > 1}
    print()
    print("duplicate SE names: %d redefines over %d names -- the tower tiers "
          "were copy-pasted without renaming"
          % (sum(c - 1 for c in dup.values()), len(dup)))

    sets = collections.Counter()
    for _k, owner, run in pairs:
        sets[tuple(sorted({n.rsplit("_", 1)[-1] for n, _v in run}))] += 1
    print()
    print("archives by the event set they raise:")
    for evs, n in sorted(sets.items(), key=lambda kv: (-kv[1], kv[0])):
        label = ",".join(evs)
        print("   %4d  %s" % (n, label if len(label) < 60
                              else label[:57] + "..."))

    if args.roles:
        for prefix, label in (("BANK_UNIT", "unit"),
                              ("BANK_STRUCTURE", "structure")):
            r = roles(defs, prefix)
            first = r[FACTIONS[0]]
            same = all(r[f] == first for f in FACTIONS)
            print()
            print("%s roles (%d per faction, identical across all six: %s):"
                  % (label, len(first), same))
            for i, name in enumerate(first):
                print("   %2d  %s" % (i, name))

        print()
        ev = events(defs)
        print("behaviour events, one sound per entity per event:")
        for k, v in ev.most_common():
            print("   %-12s x%d  (%s)"
                  % (k, v, "units and structures" if v == 120 else "units only"))

        spells = sorted(n.split("__")[1] for n, _ in defs
                        if n.startswith("BANK_SPELL__"))
        print()
        print("spell banks (%d): %s" % (len(spells), ", ".join(spells)))

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        with open(args.tsv, "w", encoding="utf-8") as fh:
            fh.write("symbol\tvalue\tgroup\n")
            for name, value in defs:
                fh.write("%s\t%d\t%s\n" % (name, value, name.split("_")[0]))
        print()
        print("wrote %s" % args.tsv)

    if args.events:
        os.makedirs(os.path.dirname(os.path.abspath(args.events)),
                    exist_ok=True)
        with open(args.events, "w", encoding="utf-8", newline="") as fh:
            fh.write("seqarc_id\tseqarc_name\tse_index\tse_name\tevent\n")
            for k, owner, run in pairs:
                for name, value in run:
                    fh.write("%d\t%s\t%d\t%s\t%s\n"
                             % (k, owner, value, name,
                                name.rsplit("_", 1)[-1]))
        print("wrote %s (%d effects)"
              % (args.events, sum(len(r) for _k, _o, r in pairs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
