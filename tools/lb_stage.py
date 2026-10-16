#!/usr/bin/env python3
"""Inspect and stage the live entity pool through the debug server.

Three settings are implemented but unverified because the harness can reach a
MATCH but not a SITUATION: tools/lb_warp.py walks to a Free Play start where
nothing fights, heals or casts (docs/139). This is the first half of fixing
that -- seeing and editing the entities that exist.

Requires the runner started with NDS_DEBUG_ALLOW_WRITES=1 for any write.

    python tools/lb_stage.py list
    python tools/lb_stage.py set <index> <field> <value>

Entity fields, each established elsewhere in this project rather than guessed:

    +0xDC/+0xDD  handle: index and generation      (docs/125)
    +0xE4        owner byte                        (docs/108, docs/28)
    +0x114       transport link, a handle          (docs/108)
    +0x160       blueprint pointer; [+8] is the
                 info block, whose +0x5C is the
                 category                          (docs/72)
    +0x1A0       current hp                        (docs/95)

Categories (docs/72): 0 Heroes, 1 Builder, 2 Melee, 3 Ranged, 4 Mounted,
5 Transport Ship, 6 Siege/Special, 7+ structures. A category of 229 is an
unused pool slot, not a unit.
"""

import json
import socket
import sys

POOL_PTR = 0x02155028

CATEGORY = {
    0: "Hero", 1: "Builder", 2: "Melee", 3: "Ranged", 4: "Mounted",
    5: "Transport", 6: "Siege/Special", 7: "Base", 8: "Lumber",
    10: "Farm", 11: "Barracks", 12: "SpecialFactory", 13: "Tower",
    16: "Shipyard", 17: "Bridge", 18: "Gate",
}

FIELDS = {"owner": (0xE4, 1), "hp": (0x1A0, 2), "transport": (0x114, 2)}


class Dbg:
    def __init__(self, host="127.0.0.1", port=19842, timeout=900.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.f = self.s.makefile("rwb")

    def cmd(self, name, **kw):
        kw["cmd"] = name
        self.f.write((json.dumps(kw) + "\n").encode())
        self.f.flush()
        return json.loads(self.f.readline().decode())

    def read(self, addr, n):
        return bytes.fromhex(self.cmd("read_mem", cpu=9, addr=addr, len=n)["hex"])

    def u8(self, a):
        return self.read(a, 1)[0]

    def u16(self, a):
        return int.from_bytes(self.read(a, 2), "little")

    def u32(self, a):
        return int.from_bytes(self.read(a, 4), "little")

    def write(self, addr, value, width):
        r = self.cmd("write_mem", cpu=9, addr=addr, value=value, width=width)
        if "error" in r:
            raise RuntimeError(r["error"])


def entities(d):
    pool = d.u32(POOL_PTR)
    array = d.u32(pool + 8)
    count = d.u32(pool + 0xC)
    out = []
    for i in range(min(count, 256)):
        e = d.u32(array + i * 4)
        if not e:
            continue
        blueprint = d.u32(e + 0x160)
        cat = 0xFF
        if blueprint:
            info = d.u32(blueprint + 8)
            if info:
                cat = d.u8(info + 0x5C)
        out.append({
            "index": i, "addr": e, "owner": d.u8(e + 0xE4), "category": cat,
            "hp": d.u16(e + 0x1A0), "handle": (d.u8(e + 0xDC), d.u8(e + 0xDD)),
            "transport": d.u16(e + 0x114),
        })
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    d = Dbg()
    if sys.argv[1] == "list":
        rows = entities(d)
        print("%-3s %-9s %-5s %-15s %-6s %-8s %s"
              % ("i", "entity", "owner", "category", "hp", "handle", "transport"))
        for r in rows:
            live = r["category"] in CATEGORY
            name = CATEGORY.get(r["category"], "unused(%d)" % r["category"])
            print("%-3d %08X  %-5d %-15s %-6d %02X.%02X    %s%s"
                  % (r["index"], r["addr"], r["owner"], name, r["hp"],
                     r["handle"][0], r["handle"][1],
                     "-" if r["transport"] == 0xFFFF else "%04X" % r["transport"],
                     "" if live else "   (empty slot)"))
        live = [r for r in rows if r["category"] in CATEGORY]
        owners = sorted({r["owner"] for r in live})
        print()
        print("%d live entities, owners %s"
              % (len(live), ", ".join(str(o) for o in owners)))
        return 0
    if sys.argv[1] == "set" and len(sys.argv) == 5:
        idx, field, value = int(sys.argv[2]), sys.argv[3], int(sys.argv[4], 0)
        if field not in FIELDS:
            print("field must be one of: %s" % ", ".join(FIELDS))
            return 2
        rows = {r["index"]: r for r in entities(d)}
        if idx not in rows:
            print("no entity at index %d" % idx)
            return 2
        off, width = FIELDS[field]
        addr = rows[idx]["addr"] + off
        before = d.u8(addr) if width == 1 else d.u16(addr)
        d.write(addr, value, width * 8)
        after = d.u8(addr) if width == 1 else d.u16(addr)
        print("entity %d %s: %d -> %d (wrote %d at %08X)"
              % (idx, field, before, after, value, addr))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
