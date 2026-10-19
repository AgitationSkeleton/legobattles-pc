"""Tests for the LEGO Battles master server.

Runs the real server on an ephemeral port against a temporary database and
talks to it over HTTP with the standard library, so what is tested is the
service an actual client would meet rather than the functions underneath it.

    python test_master_server.py
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import master_server


PASS = []
FAIL = []


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print("  [%s] %-58s %s" % ("PASS" if ok else "FAIL", name, detail))


def post(base, path, payload):
    req = urllib.request.Request(
        base + path, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))


def get(base, path):
    try:
        with urllib.request.urlopen(base + path, timeout=5) as r:
            return r.status, json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    tmp = tempfile.mkdtemp(prefix="lbmaster")
    db = os.path.join(tmp, "t.sqlite3")
    logfile = os.path.join(tmp, "logs", "t.log")

    srv, store, log = master_server.build(0, db, logfile, echo=False,
                                          max_players=8, host="127.0.0.1")
    port = srv.server_address[1]
    base = "http://127.0.0.1:%d" % port
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("master server on %s, db %s\n" % (base, db))

    # -- health --
    code, body = get(base, "/health")
    check("health responds", code == 200 and body.get("ok") is True,
          "lobbies=%s" % body.get("lobbies"))

    # -- announce --
    code, ann = post(base, "/announce", {
        "name": "Test Battle", "host_name": "Ada", "port": 30000,
        "players": 1, "max_players": 3, "map_name": "ck1_1",
        "mode": "skirmish", "version": "0.1",
        "players_list": [{"name": "Ada", "address": "127.0.0.1"}],
    })
    ok = code == 200 and ann.get("ok") and len(ann.get("code", "")) == 4
    check("announce returns a 4-character room code", ok,
          "code=%s ttl=%s" % (ann.get("code"), ann.get("ttl")))
    room, token = ann.get("code"), ann.get("token")

    check("the room code uses the confusion-free alphabet",
          all(c in master_server.CROCKFORD for c in (room or "x")),
          room or "")

    # -- list --
    code, body = get(base, "/list")
    lob = (body.get("lobbies") or [{}])[0]
    check("the lobby appears in /list",
          code == 200 and lob.get("code") == room, lob.get("name", ""))
    check("/list never leaks the host token", "token" not in lob)
    check("the player list comes back for the GUI to expand",
          [p["name"] for p in lob.get("players_list", [])] == ["Ada"])

    # -- heartbeat --
    code, body = post(base, "/heartbeat",
                      {"code": room, "token": token, "players": 2,
                       "players_list": [{"name": "Ada"}, {"name": "Linus"}]})
    check("heartbeat with the right token is accepted",
          code == 200 and body.get("ok") is True)
    _, body = get(base, "/lobby/%s" % room)
    check("heartbeat updates the player count",
          body["lobby"]["players"] == 2, "players=%s" % body["lobby"]["players"])
    check("heartbeat updates the player list",
          [p["name"] for p in body["lobby"]["players_list"]] == ["Ada", "Linus"])

    code, body = post(base, "/heartbeat",
                      {"code": room, "token": "wrong", "players": 3})
    check("heartbeat with a wrong token is refused", code == 404)

    # -- the host states its own cap; the operator ceiling only stops nonsense --
    code, body = post(base, "/announce",
                      {"name": "Huge", "port": 30001, "max_players": 99})
    check("an absurd max_players is refused by the operator ceiling",
          code == 400, body.get("error", ""))
    code, body = post(base, "/announce",
                      {"name": "Three", "port": 30002, "max_players": 3})
    check("a host may state its own max_players within the ceiling",
          code == 200 and body.get("ok"))
    if body.get("ok"):
        post(base, "/remove", {"code": body["code"], "token": body["token"]})

    code, body = post(base, "/announce", {"name": "Bad", "port": 99999,
                                          "max_players": 2})
    check("an out-of-range port is refused", code == 400)

    # -- docs/400: the introduction. Every request here comes from 127.0.0.1,
    #    so the joiner is "behind the host's router" and gets the LAN pair. --
    code, body = post(base, "/announce",
                      {"name": "Nat", "port": 30010, "max_players": 2,
                       "lan_address": "192.168.5.9", "lan_port": 26710})
    nat, ntok = body.get("code"), body.get("token")
    _, body = get(base, "/list")
    mine = [l for l in body.get("lobbies", []) if l["code"] == nat]
    check("a viewer behind the host's router sees its LAN pair",
          bool(mine) and mine[0]["address"] == "192.168.5.9"
          and mine[0]["port"] == 26710, str(mine[:1]))
    code, _ = post(base, "/punch", {"code": nat, "port": 0})
    check("punch refuses a bad port", code == 400)
    code, _ = post(base, "/punch", {"code": "ZZZZ", "port": 40000})
    check("punch to an unknown lobby is refused", code == 404)
    code, _ = post(base, "/punch", {"code": nat, "port": 40000,
                                    "lan_address": "192.168.5.7",
                                    "lan_port": 26711})
    check("punch", code == 200)
    code, _ = post(base, "/poll", {"code": nat, "token": "wrong"})
    check("poll with a wrong token is refused", code == 404)
    code, body = post(base, "/poll", {"code": nat, "token": ntok})
    check("poll hands the host the joiner's LAN pair",
          code == 200 and body.get("punch") == [{"address": "192.168.5.7",
                                                 "port": 26711}], str(body))
    post(base, "/punch", {"code": nat, "port": 40001,
                          "lan_address": "8.8.8.8", "lan_port": 26711})
    _, body = post(base, "/poll", {"code": nat, "token": ntok})
    check("a public lan_address is never handed out",
          all(p["address"] != "8.8.8.8" for p in body.get("punch", [])),
          str(body))
    post(base, "/remove", {"code": nat, "token": ntok})

    # -- expiry --
    stale = time.time() - master_server.TTL_SECONDS - 5
    with store.lock:
        store.db.execute("UPDATE lobby SET seen=? WHERE code=?", (stale, room))
        store.db.commit()
    _, body = get(base, "/list")
    check("a lobby that stops heartbeating expires out of the list",
          all(l["code"] != room for l in body.get("lobbies", [])),
          "%d live" % len(body.get("lobbies", [])))

    # -- bans --
    store.ban("127.0.0.1", "test")
    code, body = post(base, "/announce", {"name": "Nope", "port": 30003,
                                          "max_players": 2})
    check("a banned address cannot announce", code == 403)
    store.unban("127.0.0.1")
    code, body = post(base, "/announce", {"name": "Back", "port": 30004,
                                          "max_players": 2})
    check("unbanning restores access", code == 200 and body.get("ok"))

    # -- the log is kept on disk --
    check("the log file exists and has content",
          os.path.exists(logfile) and os.path.getsize(logfile) > 0,
          logfile)
    with open(logfile, encoding="utf-8") as fh:
        text = fh.read()
    check("announcements are recorded in the log", "ANNOUNCE" in text)

    srv.shutdown()
    srv.server_close()
    store.close()

    print("\n%d passed, %d failed" % (len(PASS), len(FAIL)))
    if FAIL:
        for n in FAIL:
            print("  failed:", n)
    return 1 if FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
