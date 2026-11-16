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

    issue2(tmp)

    print("\n%d passed, %d failed" % (len(PASS), len(FAIL)))
    if FAIL:
        for n in FAIL:
            print("  failed:", n)
    return 1 if FAIL else 0


def raw(port, data, source="127.0.0.1"):
    """A socket from `source` with `data` sent and left open."""
    import socket
    s = socket.socket()
    s.bind((source, 0))
    s.connect(("127.0.0.1", port))
    if data:
        s.sendall(data)
    return s


def health_from(port, source, timeout=5):
    """GET /health from the loopback address `source`; the status or an error."""
    import socket
    try:
        s = raw(port, b"GET /health HTTP/1.0\r\nHost: x\r\n\r\n", source)
        s.settimeout(timeout)
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        s.close()
        return int(data.split(b" ", 2)[1]) if data else "closed"
    except (OSError, ValueError, IndexError) as e:
        return "error: %s" % e


def issue2(tmp):
    """GitHub issue #2: the load test's findings, each reproduced."""
    print("\n-- issue #2 --")
    # Short limits so the test is quick: the same code paths as the defaults.
    master_server.Handler.timeout = 2
    master_server.CONN_DEADLINE = 4
    sent = []
    disc = master_server.Discord([], master_server.Log(None, echo=False))
    disc.queue = type("Q", (), {"put_nowait": lambda self, item: sent.append(item[0])})()
    srv, store, log = master_server.build(0, os.path.join(tmp, "i2.sqlite3"), None, echo=False,
                                          max_players=8, host="127.0.0.1")
    srv.discord = disc
    port = srv.server_address[1]
    base = "http://127.0.0.1:%d" % port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    # Finding 1: half-sent requests held open. One address gets
    # MAX_CONNECTIONS_PER_IP; another address is still served meanwhile, and
    # the held ones are dropped by the read timeout.
    held = [raw(port, b"GET / HTTP/1.1\r\nHost: x\r\nX-Pad: AAAA", "127.0.0.3") for _ in range(40)]
    time.sleep(0.5)
    check("40 half-sent requests from one address: another is served",
          health_from(port, "127.0.0.2") == 200)
    check("... and that address gets only its share of threads",
          len(srv.conns) <= master_server.MAX_CONNECTIONS_PER_IP + 1, "%d handled" % len(srv.conns))
    time.sleep(3)
    check("held connections end at the read timeout", len(srv.conns) == 0, "%d left" % len(srv.conns))
    for s in held:
        s.close()
    # A client that trickles a byte every second never trips the read
    # timeout; the connection deadline cuts it.
    s = raw(port, b"GET / HTTP/1.1\r\n", "127.0.0.4")
    t0 = time.time()
    cut = False
    while time.time() - t0 < 10:
        try:
            s.sendall(b"X")
        except OSError:
            cut = True
            break
        time.sleep(1)
        if not any(c[0] == "127.0.0.4" for c in list(srv.conns.values())):
            cut = True
            break
    s.close()
    check("a trickling client is cut at the connection deadline", cut,
          "after %.0f s" % (time.time() - t0))

    # Finding 2: lobbies per address, the announce rate, the list's size.
    codes = []
    for i in range(5):
        c, b = post(base, "/announce", {"name": "Flood %d" % i, "port": 31000 + i, "max_players": 2})
        codes.append(c)
    check("one address lists at most MAX_LOBBIES_PER_IP lobbies",
          codes[:3] == [200, 200, 200] and codes[3:] == [429, 429], str(codes))
    with store.lock:   # its lobbies closed: the cap no longer applies, the rate does
        store.db.execute("DELETE FROM lobby WHERE peer = '127.0.0.1'")
        store.db.commit()
    rc = [post(base, "/announce", {"name": "More", "port": 31010, "max_players": 2})[0] for _ in range(3)]
    check("... and its announce rate is limited (RATE_ANNOUNCE)", rc == [200, 429, 429], str(rc))
    now = time.time()
    for i in range(130):
        store.announce("T%03d" % i, "t", {"name": "Bulk", "host_name": "B", "address": "1.2.3.4",
                                          "port": 1, "players": 1, "max_players": 2,
                                          "peer": "10.0.%d.%d" % (i // 200, i)}, now)
    c, b = get(base, "/list")
    check("/list carries at most MAX_LIST lobbies, and says how many there are",
          c == 200 and len(b["lobbies"]) == master_server.MAX_LIST and b.get("total", 0) >= 130,
          "%d of %s" % (len(b.get("lobbies", [])), b.get("total")))
    check("/list never shows the announcing address field", all("peer" not in l for l in b["lobbies"]))
    old = master_server.MAX_LOBBIES
    master_server.MAX_LOBBIES = 10
    srv.window = master_server.Window()
    with store.lock:
        store.db.execute("DELETE FROM lobby WHERE peer = '127.0.0.1'")
        store.db.commit()
    c, b = post(base, "/announce", {"name": "Full", "port": 31020, "max_players": 2})
    check("a full server answers 503", c == 503, "%d %s" % (c, b.get("error")))
    master_server.MAX_LOBBIES = old
    with store.lock:
        store.db.execute("DELETE FROM lobby")
        store.db.commit()
    real = store.new_code
    store.new_code = lambda: (_ for _ in ()).throw(RuntimeError("full"))
    c, b = post(base, "/announce", {"name": "NoCode", "port": 31021, "max_players": 2})
    store.new_code = real
    check("no free room code is a 503, not a dropped connection", c == 503, "%d" % c)

    # Finding 3: the address a host names.
    srv.window = master_server.Window()
    c, b = post(base, "/announce", {"name": "Spoof", "port": 31030, "max_players": 2,
                                    "address": "ATTACKER-CONTROLLED-STRING-1234567890"})
    _, lb = get(base, "/lobby/%s" % b.get("code"))
    check("a host cannot list itself at an arbitrary string",
          lb.get("lobby", {}).get("address") == "127.0.0.1", str(lb.get("lobby", {}).get("address")))
    post(base, "/remove", {"code": b.get("code"), "token": b.get("token")})
    check("a public address is believed from the address seen",
          master_server.host_address("8.8.8.8", "8.8.8.8") == "8.8.8.8")
    check("... not from another public address",
          master_server.host_address("8.8.8.8", "1.1.1.1") == "1.1.1.1")
    check("... and from this server's own network (docs/400)",
          master_server.host_address("8.8.8.8", "192.168.1.6") == "8.8.8.8")
    check("... but never a private or reserved one from there",
          master_server.host_address("10.0.0.1", "192.168.1.6") == "192.168.1.6"
          and master_server.host_address("100.64.1.1", "192.168.1.6") == "192.168.1.6")
    srv.store.set_players("ZZZZ", [{"name": "<b>x", "address": "evil"}, "junk"])
    check("a player list keeps only clean names and dotted quads",
          srv.store.players("ZZZZ") == [{"slot": 0, "name": "Player", "address": ""}],
          str(srv.store.players("ZZZZ")))

    # Finding 4: HEAD.
    import http.client
    hc = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    hc.request("HEAD", "/list")
    r = hc.getresponse()
    body = r.read()
    check("HEAD answers 200 with headers only", r.status == 200 and body == b""
          and int(r.getheader("Content-Length")) > 0, "%d, %d bytes" % (r.status, len(body)))
    check("the banner no longer names the Python version", "Python" not in (r.getheader("Server") or ""),
          r.getheader("Server"))
    hc.close()
    hc = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    hc.request("PUT", "/list")
    r = hc.getresponse()
    data = r.read()
    check("an unknown method is a JSON error with a correct length",
          r.status == 501 and len(data) == int(r.getheader("Content-Length")) and data.startswith(b"{"),
          "%d" % r.status)
    hc.close()

    # The Discord channel: one source cannot flood it, nor many together.
    sent.clear()
    for i in range(40):
        disc("line %d" % i, source="203.0.113.9")
    check("one address gets at most DISCORD_PER_SOURCE[0] lines at once",
          len(sent) == master_server.DISCORD_PER_SOURCE[0], "%d sent" % len(sent))
    for i in range(100):
        disc("line %d" % i, source="198.51.100.%d" % i)
    check("every address together gets at most DISCORD_GLOBAL[0] at once",
          len(sent) <= master_server.DISCORD_GLOBAL[0], "%d sent" % len(sent))
    disc.held_said = -1e9
    disc.every = master_server.Bucket(*master_server.DISCORD_GLOBAL)
    disc("after the flood", source="192.0.2.1")
    check("the next line says how many were held back", "held back" in sent[-1], sent[-1][-60:])

    srv.shutdown()
    srv.server_close()
    store.close()


if __name__ == "__main__":
    raise SystemExit(main())
