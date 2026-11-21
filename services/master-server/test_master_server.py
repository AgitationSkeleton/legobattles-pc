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
    signin(tmp)

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
    master_server.Handler.read_timeout = 2
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


def mk(tag, sub, aud="play", exp_in=3600):
    """A token shaped like the Worker's (v1.<payload>.<86-char signature>). The
    fake Worker below trusts the payload's "tag": "good" is good."""
    import base64
    payload = {"sub": sub, "uid": "LB-TEST0001", "name": "Tester", "aud": aud, "tag": tag,
               "iat": int(time.time()), "exp": int(time.time()) + exp_in}
    b = base64.urlsafe_b64encode(json.dumps(payload).encode()).decode().rstrip("=")
    return "v1." + b + "." + "S" * 86


GOOD = mk("good", "1000a1b")
GOOD2 = mk("good", "1000d4e")
BAD = mk("bad", "1000zzz")
BANS = []   # the fake Worker's banned accounts


def fake_auth():
    """A stand-in for the Worker's POST /auth/verify (a payload tagged "good"
    is good; anything else is refused with a 401 {ok:false}) and GET /auth/bans."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    calls = []

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            data = json.dumps({"ok": True, "banned": list(BANS)}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            token = json.loads(self.rfile.read(n).decode("utf-8")).get("token", "")
            calls.append(token)
            c = master_server.token_claims(token) or {}
            if self.path == "/auth/verify" and c.get("tag") == "good":
                code, out = 200, {"ok": True, "sub": c["sub"], "uid": "LB-TEST0001",
                                  "name": "Tester", "aud": c.get("aud")}
            else:
                code, out = 401, {"ok": False, "error": "bad token"}
            data = json.dumps(out).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, calls


def signin(tmp):
    """docs/501: --require-signin and --min-version."""
    print("\n-- docs/501: sign-in and the version gate --")
    master_server.Handler.timeout = 5
    vk = master_server.version_older
    check("versions compare per dotted part, numerically",
          vk("0.9.9", "0.10.0") and not vk("0.10.0", "0.9.9") and not vk("0.10", "0.10.0"))
    check("a suffix sorts below the plain release",
          vk("0.5.0-beta", "0.5.0") and not vk("0.5.0", "0.5.0-beta") and not vk("0.5.1-beta", "0.5.0"))
    check("a missing or malformed version is too old",
          vk(None, "0.5.0") and vk("", "0.5.0") and vk("banana", "0.5.0"))

    fake, calls = fake_auth()
    auth_base = "http://127.0.0.1:%d" % fake.server_address[1]
    logfile = os.path.join(tmp, "logs", "signin.log")
    srv, store, log = master_server.build(0, os.path.join(tmp, "s.sqlite3"), logfile, echo=False,
                                          max_players=8, host="127.0.0.1", require_signin=True,
                                          auth_server=auth_base, min_version="0.5.0")
    port = srv.server_address[1]
    base = "http://127.0.0.1:%d" % port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    c, b = get(base, "/health")
    check("/health needs neither a token nor a version", c == 200 and b.get("ok") is True)

    # The version gate comes first.
    c, b = get(base, "/list?auth=" + GOOD)
    check("a GET with no ?v= is refused with 426", c == 426 and b.get("reason") == "update"
          and "Install update" in b.get("message", ""), "%d" % c)
    c, b = get(base, "/list?v=0.4.18&auth=" + GOOD)
    check("a GET below --min-version is refused with 426", c == 426, "%d" % c)
    c, b = get(base, "/list?v=0.5.0-beta&auth=" + GOOD)
    check("a pre-release of the minimum is refused", c == 426, "%d" % c)
    c, b = post(base, "/announce", {"name": "Old", "port": 32000, "max_players": 2, "auth": GOOD})
    check("a POST with no version is refused with 426", c == 426, "%d" % c)

    # Then the token.
    c, b = get(base, "/list?v=0.5.0")
    check("a GET with no token is refused with 401 signin",
          c == 401 and b.get("reason") == "signin" and "Sign in with Discord" in b.get("message", ""),
          "%d %s" % (c, b.get("error")))
    c, b = get(base, "/list?v=0.5.0&auth=" + BAD)
    check("a GET with a bad token is refused with 401", c == 401, "%d" % c)
    c, b = get(base, "/list?v=0.10.0&auth=" + GOOD)
    check("a GET with a good token and a newer version is served", c == 200 and b.get("ok"), "%d" % c)
    c, b = post(base, "/announce", {"name": "NoAuth", "port": 32001, "max_players": 2,
                                    "version": "0.5.0"})
    check("a POST with no token is refused with 401", c == 401 and b.get("reason") == "signin")
    c, b = post(base, "/announce", {"name": "Bad", "port": 32002, "max_players": 2,
                                    "version": "0.5.0", "auth": BAD})
    check("a POST with a bad token is refused with 401", c == 401)
    c, ann = post(base, "/announce", {"name": "Signed", "host_name": "Host", "port": 32003,
                                      "max_players": 2, "version": "0.5.0", "auth": GOOD})
    check("a signed-in host can announce", c == 200 and ann.get("ok"), "%d %s" % (c, ann.get("error")))
    room = ann.get("code")
    check("the lobby records the host's sub", srv.meta.get(room, {}).get("sub") == "1000a1b",
          str(srv.meta.get(room, {}).get("sub")))
    c, b = post(base, "/join", {"code": room, "name": "Guest", "version": "0.5.0", "auth": GOOD2})
    check("a signed-in player can join, and its sub is recorded",
          c == 200 and srv.meta[room]["member_subs"].get("guest") == "1000d4e",
          str(srv.meta[room].get("member_subs")))
    c, b = get(base, "/lobby/%s?v=0.5.0&auth=%s" % (room, GOOD))
    check("GET /lobby needs the token too, and is served with it", c == 200
          and get(base, "/lobby/%s?v=0.5.0" % room)[0] == 401)

    n = len(calls)
    for _ in range(5):
        get(base, "/list?v=0.5.0&auth=" + GOOD)
        get(base, "/list?v=0.5.0&auth=" + BAD)
    check("answers are cached: no new /auth/verify calls", len(calls) == n,
          "%d calls, then %d" % (n, len(calls)))
    check("the cache is keyed by a hash, never the token",
          all(GOOD not in k and BAD not in k for k in srv.auth.cache))

    # security review: what is refused without asking the Worker at all.
    n = len(calls)
    for junk in ("v1.x.y", "notatoken", mk("good", "1000acc", aud="main"), mk("good", "1000old", exp_in=-5)):
        c, b = get(base, "/list?v=0.5.0&auth=" + junk)
        check("refused unasked: %s" % ("an account token" if "main" in str(master_server.token_claims(junk))
                                         else "junk or expired"), c == 401, "%d" % c)
    check("... and the Worker was not asked", len(calls) == n, "%d calls, then %d" % (n, len(calls)))
    # A ban reaches this server within a minute (the list is fetched).
    BANS.append("1000a1b")
    srv.auth.bans_at = -1e9
    get(base, "/list?v=0.5.0&auth=" + GOOD2)     # starts the fetch
    time.sleep(0.5)
    c, b = get(base, "/list?v=0.5.0&auth=" + GOOD)
    check("a banned account's cached token is refused once the list arrives", c == 401, "%d" % c)
    BANS.clear()
    srv.auth.bad.clear()
    srv.auth.cache.pop(srv.auth.key(GOOD), None)
    srv.auth.bans_at = -1e9
    get(base, "/list?v=0.5.0&auth=" + GOOD2)
    time.sleep(0.5)
    # Join/leave are only for the caller's own account.
    c, b = post(base, "/leave", {"code": room, "name": "Guest", "version": "0.5.0", "auth": GOOD})
    check("an account cannot take another account's name out", c == 403, "%d" % c)
    c, b = post(base, "/join", {"code": room, "name": "Guest", "version": "0.5.0", "auth": GOOD})
    check("... nor take it over", srv.meta[room]["member_subs"].get("guest") == "1000d4e" or c == 403,
          str(srv.meta[room].get("member_subs")))
    # Numbers that used to crash a request now get a clean answer.
    c, b = post(base, "/announce", {"name": "Huge", "port": 32010, "max_players": 2, "version": "0.5.0",
                                    "players": 10 ** 40, "auth": GOOD2})
    check("a huge number is clamped, not a crash", c in (200, 429), "%d" % c)
    c, b = get(base, "/list?" + "&".join("a%d=1" % i for i in range(20)))
    check("too many query fields: a clean refusal", c in (401, 426), "%d" % c)

    # The Worker goes away.
    fake.shutdown()
    fake.server_close()
    key = srv.auth.key(GOOD)
    srv.auth.cache[key][0] -= master_server.AUTH_TTL + 60      # past the reuse time
    c, b = get(base, "/list?v=0.5.0&auth=" + GOOD)
    check("unreachable: a token cached as good stays good (under an hour)", c == 200, "%d" % c)
    srv.auth.cache[key][0] -= master_server.AUTH_STALE          # now past an hour
    c, b = get(base, "/list?v=0.5.0&auth=" + GOOD)
    check("... but not once its answer is an hour old (503)",
          c == 503 and b.get("error") == "sign-in unavailable", "%d %s" % (c, b.get("error")))
    c, b = get(base, "/list?v=0.5.0&auth=" + mk("good", "1000new"))
    check("unreachable: a token never seen is refused with 503",
          c == 503 and b == {"ok": False, "error": "sign-in unavailable"}, "%d %s" % (c, b))
    c, b = get(base, "/list?v=0.5.0")
    check("unreachable: no token at all is still a 401", c == 401, "%d" % c)

    a = master_server.Auth(auth_base, max_entries=3)
    for i in range(6):
        a._put(a.key("t%d" % i), {"sub": str(i)}, time.monotonic())
    check("the cache is bounded (oldest dropped)",
          len(a.cache) == 3 and a.key("t0") not in a.cache and a.key("t5") in a.cache)

    # A token is never logged or echoed.
    with open(logfile, encoding="utf-8") as fh:
        text = fh.read()
    check("no token, nor part of one, reaches the log",
          "v1." not in text and GOOD.split(".")[1][:20] not in text, "%d bytes of log" % len(text))

    srv.shutdown()
    srv.server_close()
    store.close()

    # Without the options (a self-hosted server), none of this applies.
    srv, store, log = master_server.build(0, os.path.join(tmp, "s2.sqlite3"), None, echo=False,
                                          max_players=8, host="127.0.0.1")
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    c, b = get("http://127.0.0.1:%d" % port, "/list")
    check("self-hosted (no options): no token or version needed", c == 200 and srv.auth is None)

    # docs/502: names with a slur are refused. No slur is written here: the
    # refused name comes from LB_SLUR_TEST on the command line (unset: only the
    # innocent half runs).
    base2 = "http://127.0.0.1:%d" % port
    c, b = post(base2, "/announce", {"name": "Night Knight", "host_name": "Big Gun", "port": 28700,
                                     "max_players": 3})
    check("innocent names that contain look-alikes are allowed", c == 200, "%d" % c)
    bad = os.environ.get("LB_SLUR_TEST", "")
    if bad:
        c, b = post(base2, "/announce", {"name": "LEGO Battles", "host_name": bad, "port": 28701,
                                         "max_players": 3})
        check("a host name with a slur is refused", c == 403 and b.get("reason") == "name", "%d" % c)
        c, b = post(base2, "/join", {"code": "ZZZZ", "name": bad})
        check("a joiner name with a slur is refused", c == 403, "%d" % c)
        # security review: and everywhere else a lobby shows text.
        for what, extra in (("a lobby setting's value", {"options": {"Rules": bad}}),
                            ("a lobby setting's name", {"options": {bad: True}}),
                            ("the player list", {"players_list": [{"name": bad}]})):
            c, b = post(base2, "/announce", dict({"name": "LEGO Battles", "host_name": "Fine", "port": 28702,
                                                  "max_players": 3}, **extra))
            check("a slur in %s is refused" % what, c == 403, "%d" % c)
        c, b = post(base2, "/remove", {"code": "ZZZZ", "token": "x", "winners": [bad]})
        check("a slur among the winners is refused", c == 403, "%d" % c)
    srv.shutdown()
    srv.server_close()
    store.close()


if __name__ == "__main__":
    raise SystemExit(main())
