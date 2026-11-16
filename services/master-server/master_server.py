"""The LEGO Battles master server.

WHAT IT IS. A directory, not a relay. Hosts advertise a lobby; clients ask for
the list and get addresses back. No game traffic passes through here, so this
cannot make an unreachable host reachable - that is what the UPnP mapping on the
host is for.

    python master_server.py --port 28555

WHY IT LOOKS LIKE THE LSW2 ONE. It is modelled on the sibling LSW2DS-RE
project's server, so the endpoint shape, the TTL model and the room-code
alphabet are deliberately the same as that project's
`services/master-server-selfhost`. A client written
against one is a small edit from working against the other, and both can run on
one host side by side.

WHY IT DOES NOT KNOW HOW MANY PLAYERS A MATCH HOLDS. LEGO Battles' own
multiplayer player cap has NOT been established from the ROM - see
`docs/86-port-progress.md`. Values are read from the ROM, never invented, so
the server never assumes one: the host states `max_players` in its own
advertisement and the server only enforces an operator-configured ceiling
(`--max-players`, default 8) to stop a malicious host advertising nonsense. When
the real cap is read out of the ROM, the default can tighten; nothing here has
to change.

STDLIB ONLY. No pip install, no build step. SQLite for storage so the directory
survives a restart, which matters when the operator is running this on their own
machine rather than a platform that keeps state for them.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import signal
import socket
import sqlite3
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ------------------------------------------------------------------ config --

# 28555 sits two above the LSW2 server's 28553 so an operator can run both on
# one machine without thinking about it. Nothing in the ROM picks this number;
# it is an operator-facing default and is overridable.
DEFAULT_PORT = 28555

# How long an advertisement survives without a heartbeat. Ninety seconds is the
# LSW2 server's value, kept identical: long enough that a client heartbeating
# every 30s survives two dropped packets, short enough that a crashed host
# disappears before somebody tries to join it.
TTL_SECONDS = 90

# Crockford Base32 minus I, L, O and U - characters that cannot be confused
# when read aloud or typed off a screenshot. U is dropped rather than folded so
# a random code cannot spell something unfortunate.
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
CODE_LENGTH = 4

MAX_NAME = 24            # a lobby or player name, generous but bounded
DEFAULT_MAX_PLAYERS = 8  # an operator ceiling, NOT the game's cap - see above
MAX_BODY = 8192          # refuse oversized request bodies outright
# docs/428: the match picture, a PNG the host posts once per match, base64.
MAX_IMAGE_BYTES = 400 * 1024
MAX_IMAGE_BODY = MAX_IMAGE_BYTES * 4 // 3 + 4096

NAME_OK = re.compile(r"^[\w \-'.!?]{1,%d}$" % MAX_NAME)

# ---------------------------------------------------------------- limits ----
#
# GitHub issue #2 (a load test on a public instance): a slow client could hold
# a handler thread forever, one address could list lobbies without end and
# flood the Discord channel, and `address` was taken on trust. The game's own
# traffic, for scale: a host announces once per lobby, heartbeats every 30 s,
# polls every 2 s for its first two minutes (then every 6 s); the Join screen
# fetches the list every 5 s. Every limit below sits far above that, and an
# address that passes one gets a 429 (a 503 when the server is full), which
# the game treats as a refusal and retries later.

READ_TIMEOUT = 10          # s: a request line, a header or a body read
CONN_DEADLINE = 20         # s: a connection's whole life; then it is cut
MAX_CONNECTIONS = 96       # handled at once; the rest are closed at once
MAX_CONNECTIONS_PER_IP = 8
MAX_LOBBIES = 200          # listed at once, every address together
MAX_LOBBIES_PER_IP = 3     # listed at once from one address
MAX_LIST = 100             # lobbies in one /list reply
# Per address: (requests, seconds).
RATE_GET = (120, 60)
RATE_POST = (120, 60)
RATE_ANNOUNCE = (6, 600)
RATE_MEMBER = (30, 600)    # /join and /leave, which make Discord lines
RATE_PUNCH = (20, 60)
# The Discord channel: a token bucket for every line, and one per source
# address (the lobby's host, or the joiner): (burst, seconds per token).
DISCORD_GLOBAL = (30, 2.0)
DISCORD_PER_SOURCE = (10, 30.0)


class Window:
    """Fixed-window counters per key, pruned as they age. Thread-safe."""

    def __init__(self):
        self.lock = threading.Lock()
        self.data = {}   # (kind, key) -> [window start, count]
        self.pruned = 0.0

    def over(self, kind, key, limit, now=None):
        count, seconds = limit
        now = time.monotonic() if now is None else now
        with self.lock:
            if now - self.pruned > 60:
                self.pruned = now
                self.data = {k: v for k, v in self.data.items() if now - v[0] < 3600}
            w = self.data.get((kind, key))
            if not w or now - w[0] >= seconds:
                self.data[(kind, key)] = [now, 1]
                return False
            w[1] += 1
            return w[1] > count


class Bucket:
    """Token buckets per key: `burst` at most, one more every `every` s."""

    def __init__(self, burst, every):
        self.burst, self.every = burst, every
        self.lock = threading.Lock()
        self.data = {}   # key -> [tokens, last]

    def take(self, key, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            if len(self.data) > 4096:
                self.data = {k: v for k, v in self.data.items()
                             if v[0] + (now - v[1]) / self.every < self.burst}
            t = self.data.get(key)
            tokens = self.burst if t is None else min(self.burst, t[0] + (now - t[1]) / self.every)
            if tokens < 1:
                self.data[key] = [tokens, now]
                return False
            self.data[key] = [tokens - 1, now]
            return True


_V4 = re.compile(r"^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$")


def is_v4(s):
    return bool(_V4.match(str(s or "")))


def is_public_v4(s):
    """A dotted quad on the public internet: not private, loopback, link-local,
    carrier-grade NAT, 'this network', multicast or reserved."""
    if not is_v4(s):
        return False
    a, b = (int(x) for x in s.split(".")[:2])
    if a in (0, 10, 127) or a >= 224:
        return False
    if (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168) or (a == 169 and b == 254):
        return False
    return not (a == 100 and 64 <= b <= 127)


def host_address(claimed, peer):
    """issue #2, finding 3: the address a lobby is listed at. A host names its
    public address (from UPnP or STUN), which is normally the one this server
    sees; it is believed only then, or when the server cannot check it: the
    host is on this server's own network (seen by a private address, docs/400)
    or reached it over IPv6. Anything else is listed at the address seen, so a
    listing can never point joiners at a third party."""
    claimed = str(claimed or "").strip()
    if claimed == peer and is_v4(claimed):
        return claimed
    if is_public_v4(claimed) and (is_local(peer) or ":" in str(peer)):
        return claimed
    return peer


# ------------------------------------------------------------------ storage --

SCHEMA = """
CREATE TABLE IF NOT EXISTS lobby (
    code        TEXT PRIMARY KEY,
    token       TEXT NOT NULL,
    name        TEXT NOT NULL,
    host_name   TEXT NOT NULL,
    address     TEXT NOT NULL,
    port        INTEGER NOT NULL,
    players     INTEGER NOT NULL,
    max_players INTEGER NOT NULL,
    map_name    TEXT NOT NULL DEFAULT '',
    mode        TEXT NOT NULL DEFAULT '',
    version     TEXT NOT NULL DEFAULT '',
    options     TEXT NOT NULL DEFAULT '{}',
    created     REAL NOT NULL,
    seen        REAL NOT NULL,
    peer        TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS player (
    code        TEXT NOT NULL,
    slot        INTEGER NOT NULL,
    name        TEXT NOT NULL,
    address     TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (code, slot)
);
CREATE TABLE IF NOT EXISTS ban (
    address     TEXT PRIMARY KEY,
    reason      TEXT NOT NULL DEFAULT '',
    created     REAL NOT NULL
);
"""


class Store:
    """SQLite behind a lock. Every method is safe to call from a handler thread."""

    def __init__(self, path):
        self.path = path
        self.lock = threading.RLock()
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        with self.lock:
            self.db.executescript(SCHEMA)
            # issue #2: the address a lobby was announced from (never shown),
            # for the per-address cap; a database from before it gains it.
            cols = [r[1] for r in self.db.execute("PRAGMA table_info(lobby)")]
            if "peer" not in cols:
                self.db.execute("ALTER TABLE lobby ADD COLUMN peer TEXT NOT NULL DEFAULT ''")
            self.db.commit()

    def close(self):
        with self.lock:
            self.db.close()

    # -- lobbies --

    def new_code(self):
        """A code nobody is using. Retries rather than trusting randomness."""
        with self.lock:
            for _ in range(64):
                code = "".join(secrets.choice(CROCKFORD)
                               for _ in range(CODE_LENGTH))
                row = self.db.execute(
                    "SELECT 1 FROM lobby WHERE code=?", (code,)).fetchone()
                if not row:
                    return code
        raise RuntimeError("could not allocate a free room code")

    def announce(self, code, token, fields, now):
        with self.lock:
            self.db.execute(
                "INSERT OR REPLACE INTO lobby (code, token, name, host_name, "
                "address, port, players, max_players, map_name, mode, version, "
                "options, created, seen, peer) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (code, token, fields["name"], fields["host_name"],
                 fields["address"], fields["port"], fields["players"],
                 fields["max_players"], fields.get("map_name", ""),
                 fields.get("mode", ""), fields.get("version", ""),
                 json.dumps(fields.get("options", {})), now, now,
                 fields.get("peer", "")))
            self.db.commit()

    def count(self, now, peer=None, ttl=TTL_SECONDS):
        """Lobbies listed now: all of them, or those announced from `peer`."""
        with self.lock:
            if peer is None:
                row = self.db.execute("SELECT COUNT(*) FROM lobby WHERE seen >= ?",
                                      (now - ttl,)).fetchone()
            else:
                row = self.db.execute("SELECT COUNT(*) FROM lobby WHERE seen >= ? AND peer = ?",
                                      (now - ttl, peer)).fetchone()
            return int(row[0])

    def heartbeat(self, code, token, players, now):
        with self.lock:
            cur = self.db.execute(
                "UPDATE lobby SET seen=?, players=? WHERE code=? AND token=?",
                (now, players, code, token))
            self.db.commit()
            return cur.rowcount > 0

    def remove(self, code, token=None):
        with self.lock:
            if token is None:
                cur = self.db.execute("DELETE FROM lobby WHERE code=?", (code,))
            else:
                cur = self.db.execute(
                    "DELETE FROM lobby WHERE code=? AND token=?", (code, token))
            self.db.execute("DELETE FROM player WHERE code=?", (code,))
            self.db.commit()
            return cur.rowcount > 0

    def expire(self, now, ttl=TTL_SECONDS):
        """Drop advertisements nobody has refreshed. Returns the codes dropped."""
        with self.lock:
            rows = self.db.execute(
                "SELECT code FROM lobby WHERE seen < ?", (now - ttl,)).fetchall()
            codes = [r["code"] for r in rows]
            for code in codes:
                self.db.execute("DELETE FROM lobby WHERE code=?", (code,))
                self.db.execute("DELETE FROM player WHERE code=?", (code,))
            if codes:
                self.db.commit()
            return codes

    def lobbies(self, now=None, ttl=TTL_SECONDS):
        with self.lock:
            if now is None:
                rows = self.db.execute(
                    "SELECT * FROM lobby ORDER BY created").fetchall()
            else:
                rows = self.db.execute(
                    "SELECT * FROM lobby WHERE seen >= ? ORDER BY created",
                    (now - ttl,)).fetchall()
            return [dict(r) for r in rows]

    def lobby(self, code):
        with self.lock:
            row = self.db.execute(
                "SELECT * FROM lobby WHERE code=?", (code,)).fetchone()
            return dict(row) if row else None

    # -- players, so the GUI can expand a lobby --

    def set_players(self, code, players):
        # issue #2: a player's name is cleaned like any other, and an address
        # is kept only as a dotted quad (it is shown to every client).
        with self.lock:
            self.db.execute("DELETE FROM player WHERE code=?", (code,))
            for slot, p in enumerate(players):
                if not isinstance(p, dict):
                    continue
                addr = str(p.get("address") or "")
                self.db.execute(
                    "INSERT OR REPLACE INTO player (code, slot, name, address) "
                    "VALUES (?,?,?,?)",
                    (code, slot, clean_name(p.get("name"), "Player"), addr if is_v4(addr) else ""))
            self.db.commit()

    def players(self, code):
        with self.lock:
            rows = self.db.execute(
                "SELECT slot, name, address FROM player WHERE code=? "
                "ORDER BY slot", (code,)).fetchall()
            return [dict(r) for r in rows]

    # -- bans --

    def ban(self, address, reason=""):
        with self.lock:
            self.db.execute(
                "INSERT OR REPLACE INTO ban (address, reason, created) "
                "VALUES (?,?,?)", (address, reason, time.time()))
            self.db.commit()

    def unban(self, address):
        with self.lock:
            cur = self.db.execute("DELETE FROM ban WHERE address=?", (address,))
            self.db.commit()
            return cur.rowcount > 0

    def is_banned(self, address):
        with self.lock:
            return self.db.execute(
                "SELECT 1 FROM ban WHERE address=?", (address,)).fetchone() is not None

    def bans(self):
        with self.lock:
            return [dict(r) for r in
                    self.db.execute("SELECT * FROM ban ORDER BY created").fetchall()]


# -------------------------------------------------------------------- log ----

class Log:
    """Console plus an appended file, with an in-memory tail for the GUI.

    The server runs as a cmd instance that shows logs AND keeps them, so both
    sinks are always on. Listeners let the GUI subscribe without polling a file.
    """

    def __init__(self, path=None, echo=True, keep=400):
        self.path = path
        self.echo = echo
        self.keep = keep
        self.lines = []
        self.lock = threading.RLock()
        self.listeners = []
        if path:
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)

    def __call__(self, kind, message):
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        line = "%s  %-9s %s" % (stamp, kind, message)
        with self.lock:
            self.lines.append(line)
            if len(self.lines) > self.keep:
                del self.lines[:len(self.lines) - self.keep]
            listeners = list(self.listeners)
            if self.path:
                try:
                    with open(self.path, "a", encoding="utf-8") as fh:
                        fh.write(line + "\n")
                except OSError:
                    pass
        if self.echo:
            print(line, flush=True)
        for fn in listeners:
            try:
                fn(line)
            except Exception:
                pass

    def subscribe(self, fn):
        with self.lock:
            self.listeners.append(fn)
            return list(self.lines)

    def tail(self, n=None):
        with self.lock:
            return list(self.lines) if n is None else list(self.lines[-n:])


# --------------------------------------------------------------- handlers ----

def clean_name(value, fallback="Player"):
    value = (value or "").strip()
    if not value:
        return fallback
    value = value[:MAX_NAME]
    return value if NAME_OK.match(value) else fallback


_PRIVATE_V4 = re.compile(
    r"^(10\.\d{1,3}|172\.(1[6-9]|2\d|3[01])|192\.168)\.\d{1,3}\.\d{1,3}$")


def lan_address(value):
    """docs/400: a private IPv4 dotted quad, or '' -- nothing else is kept."""
    s = str(value if value is not None else "").strip()
    return s if _PRIVATE_V4.match(s) else ""


def is_local(address):
    """A loopback or private address: this server's own network."""
    s = str(address or "")
    return s.startswith("127.") or bool(_PRIVATE_V4.match(s))


def lan_port(value):
    try:
        n = int(value)
    except (TypeError, ValueError):
        return 0
    return n if 1 <= n <= 65535 else 0


class Handler(BaseHTTPRequestHandler):
    server_version = "LegoBattlesMaster/1.1"
    sys_version = ""   # issue #2: the banner no longer names the Python version
    # issue #2, finding 1: a socket timeout for every read, so a client that
    # sends half a request cannot hold this thread (StreamRequestHandler sets
    # it in setup()). MasterServer also cuts any connection after CONN_DEADLINE.
    timeout = READ_TIMEOUT
    head = False

    # -- plumbing --

    def log_message(self, fmt, *args):
        pass  # the server's own Log does this; BaseHTTPRequestHandler's is noise

    def _send(self, code, payload, retry_after=None):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if retry_after:
            self.send_header("Retry-After", str(int(retry_after)))
        self.end_headers()
        if self.head:
            return   # issue #2, finding 4: HEAD gets the headers only
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def _limited(self, kind, limit):
        """True (and a 429 sent) when this address is over `limit` for `kind`."""
        if not self.server.window.over(kind, self.peer, limit):
            return False
        self.server.note("LIMIT", "%s %s over %d per %d s" % (self.peer, kind, limit[0], limit[1]),
                         (kind, self.peer))
        self._send(429, {"ok": False, "error": "slow down"}, retry_after=limit[1])
        return True

    def send_error(self, code, message=None, explain=None):
        """The stock error page as JSON with a correct length (an unknown
        method, a malformed request line, a request line too long)."""
        self.close_connection = True
        try:
            self._send(code, {"ok": False, "error": message or self.responses.get(code, ("error",))[0]})
        except OSError:
            pass

    def version_string(self):
        return self.server_version

    def do_HEAD(self):
        self.head = True
        self.do_GET()

    def _body(self, limit=MAX_BODY):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return None
        if n <= 0 or n > limit:
            return None
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except Exception:
            return None

    @property
    def peer(self):
        """The client's address: the socket's, or behind a reverse proxy named
        with --trusted-proxy, the last hop of its X-Forwarded-For (issue #2).
        The header is never read from anyone else, so a client cannot name an
        address for itself."""
        ip = self.client_address[0]
        if ip in self.server.trusted_proxies:
            hops = [h.strip() for h in (self.headers.get("X-Forwarded-For") or "").split(",") if h.strip()]
            if hops and (is_v4(hops[-1]) or ":" in hops[-1]):
                return hops[-1]
        return ip

    # -- routes --

    def do_GET(self):
        srv = self.server
        now = time.time()
        srv.sweep(now)
        path = self.path.split("?")[0].rstrip("/") or "/"
        if self._limited("get", RATE_GET):
            return

        if path in ("/", "/health"):
            return self._send(200, {"ok": True, "service": "legobattles-master",
                                    "lobbies": len(srv.store.lobbies(now))})
        if path == "/list":
            # issue #2, finding 2: at most MAX_LIST lobbies in one reply (the
            # oldest first, as before), with the total beside them.
            out = []
            every = srv.store.lobbies(now)
            for lob in every[:MAX_LIST]:
                lob.pop("token", None)
                lob.pop("peer", None)
                lob["options"] = json.loads(lob.get("options") or "{}")
                lob["players_list"] = srv.store.players(lob["code"])
                self._local_view(lob)
                out.append(lob)
            return self._send(200, {"ok": True, "lobbies": out, "total": len(every)})
        if path.startswith("/lobby/"):
            code = path.rsplit("/", 1)[-1].upper()
            lob = srv.store.lobby(code)
            if not lob:
                return self._send(404, {"ok": False, "error": "no such lobby"})
            lob.pop("token", None)
            lob.pop("peer", None)
            lob["options"] = json.loads(lob.get("options") or "{}")
            lob["players_list"] = srv.store.players(code)
            self._local_view(lob)
            return self._send(200, {"ok": True, "lobby": lob})
        return self._send(404, {"ok": False, "error": "no such endpoint"})

    def do_POST(self):
        srv = self.server
        now = time.time()
        srv.sweep(now)
        path = self.path.split("?")[0].rstrip("/") or "/"

        if srv.store.is_banned(self.peer):
            srv.note("REFUSED", "banned address %s tried %s" % (self.peer, path), ("ban", self.peer))
            return self._send(403, {"ok": False, "error": "banned"})
        if self._limited("post", RATE_POST):
            return
        if path == "/announce" and self._limited("announce", RATE_ANNOUNCE):
            return
        if path in ("/join", "/leave") and self._limited("member", RATE_MEMBER):
            return
        if path == "/punch" and self._limited("punch", RATE_PUNCH):
            return

        body = self._body(MAX_IMAGE_BODY if path == "/match_image" else MAX_BODY)
        if body is None:
            return self._send(400, {"ok": False, "error": "bad or oversized body"})

        if path == "/announce":
            return self._announce(body, now)
        if path == "/heartbeat":
            return self._heartbeat(body, now)
        if path == "/remove":
            return self._remove(body)
        if path == "/punch":
            return self._punch(body, now)
        if path == "/poll":
            return self._poll(body, now)
        if path == "/match_image":
            return self._match_image(body)
        if path == "/join":
            return self._member(body, now, True)
        if path == "/leave":
            return self._member(body, now, False)
        return self._send(404, {"ok": False, "error": "no such endpoint"})

    # -- docs/400: the introduction that opens two routers for each other --
    #
    # A joiner's /punch names its public UDP port (from STUN); its address is
    # the one this server sees, never one it names, so a host can only be
    # pointed back at whoever asked. The host's /poll, every 2 s while it
    # waits, collects them. Anyone whose address equals the host's is behind
    # the same router and is given the LAN pair instead. All of it is kept in
    # memory for 30 s: nothing is written to the database.

    def _match_image(self, body):
        """POST /match_image {code, token, png (base64), map_name}: the host's
        picture of its match (rosters, the map's preview, the bans), drawn by
        the game. Once per match, a PNG of a sensible size, posted to the
        Discord channels; the server never looks inside it."""
        import base64
        srv = self.server
        code = str(body.get("code") or "").upper()
        lob = srv.store.lobby(code)
        if not lob or lob.get("token") != str(body.get("token") or ""):
            return self._send(404, {"ok": False, "error": "unknown lobby"})
        with srv.lock:
            m = srv.meta.get(code)
            if m and m["image"]:
                return self._send(409, {"ok": False, "error": "already posted for this match"})
        try:
            png = base64.b64decode(str(body.get("png") or ""), validate=True)
        except Exception:
            return self._send(400, {"ok": False, "error": "png is not base64"})
        if len(png) < 64 or len(png) > MAX_IMAGE_BYTES or png[:8] != b"\x89PNG\r\n\x1a\n":
            return self._send(400, {"ok": False, "error": "not a PNG of an accepted size"})
        if m and m.get("quiet"):
            return self._send(200, {"ok": True, "quiet": True})   # the other server posts it
        with srv.lock:
            if m:
                m["image"] = True
            host = m["host"] if m else lob.get("host_name", "")
            map_name = (m["map"] if m else "") or clean_name(body.get("map_name"), "")
        srv.discord.image("\U0001F5FA\uFE0F **%s**'s match%s."
                          % (plain(host), " on **%s**" % plain(map_name) if map_name else ""), png,
                          source=self.peer)
        return self._send(200, {"ok": True})

    def _ended(self, code, m, body):
        """The Discord line for a lobby its host removed: the match's end (with
        who won and lost, as the host reports them) or the lobby disbanded."""
        now = time.time()
        if m.get("quiet"):
            return

        def names(v):
            v = v if isinstance(v, list) else []
            return [n for n in (clean_name(x, "") for x in v[:8]) if n]

        def bold(lst):
            return ", ".join("**%s**" % plain(n) for n in lst)
        winners, losers = names(body.get("winners")), names(body.get("losers"))
        result = ""
        if winners:
            result = " %s won" % bold(winners) + (", %s lost." % bold(losers) if losers else ".")
        elif losers:
            result = " %s lost." % bold(losers)
        if m["state"] == "playing" or str(body.get("reason") or "") == "end":
            self.server.discord("\u23F9\uFE0F **%s**'s match%s ended after %s.%s"
                                % (plain(m["host"]), " on **%s**" % plain(m["map"]) if m["map"] else "",
                                   lasted(now - (m["started"] or m["created"])), result),
                                source=m.get("peer", ""))
        else:
            self.server.discord("\u274C **%s** disbanded the lobby `%s` after %s."
                                % (plain(m["host"]), code, lasted(now - m["created"])),
                                source=m.get("peer", ""))

    def _local_view(self, lob):
        lan = self.server.lan.get(lob.get("code"))
        if lan and self._same_router(lob, lan[2]):
            lob["address"], lob["port"] = lan[0], lan[1]

    def _same_router(self, lob, host_seen):
        # Behind the host's router: the same public address, or -- for a
        # server on a home network, which sees its own network's machines by
        # their private addresses -- both on that network.
        if self.peer == lob.get("address"):
            return True
        return is_local(self.peer) and is_local(host_seen)

    def _punch(self, body, now):
        srv = self.server
        code = str(body.get("code") or "").upper()
        lob = srv.store.lobby(code)
        if not lob:
            return self._send(404, {"ok": False, "error": "unknown lobby"})
        port = lan_port(body.get("port"))
        if not port:
            return self._send(400, {"ok": False, "error": "port out of range"})
        entry = (self.peer, port)
        la, lp = lan_address(body.get("lan_address")), lan_port(body.get("lan_port"))
        host_lan = srv.lan.get(code)
        if la and lp and self._same_router(lob, host_lan[2] if host_lan else ""):
            entry = (la, lp)
        with srv.lock:
            keep = [p for p in srv.punches.get(code, [])
                    if now - p[2] < 30 and (p[0], p[1]) != entry]
            keep.append((entry[0], entry[1], now))
            srv.punches[code] = keep[-8:]
        srv.log("PUNCH", "%s  a player at %s:%d wants in" % (code, entry[0], entry[1]))
        return self._send(200, {"ok": True})

    def _poll(self, body, now):
        srv = self.server
        code = str(body.get("code") or "").upper()
        token = str(body.get("token") or "")
        lob = srv.store.lobby(code)
        if not lob or not token or lob.get("token") != token:
            return self._send(404, {"ok": False, "error": "unknown lobby"})
        with srv.lock:
            keep = [p for p in srv.punches.get(code, []) if now - p[2] < 30]
            if keep:
                srv.punches[code] = keep
            else:
                srv.punches.pop(code, None)
        return self._send(200, {"ok": True,
                                "punch": [{"address": a, "port": p} for a, p, _ in keep]})

    def _announce(self, body, now):
        srv = self.server
        try:
            port = int(body.get("port") or 0)
        except (TypeError, ValueError):
            port = 0
        if not (1 <= port <= 65535):
            return self._send(400, {"ok": False, "error": "port out of range"})

        try:
            maxp = int(body.get("max_players") or 0)
            players = int(body.get("players") or 1)
        except (TypeError, ValueError):
            return self._send(400, {"ok": False, "error": "bad player counts"})
        # The game's own cap is not established from the ROM, so the host states
        # it and the operator ceiling only stops nonsense.
        if not (1 <= maxp <= srv.max_players):
            return self._send(400, {"ok": False,
                                    "error": "max_players must be 1..%d"
                                             % srv.max_players})
        players = max(0, min(players, maxp))

        # issue #2, finding 2: lobbies from one address, and in all.
        if srv.store.count(now, self.peer) >= MAX_LOBBIES_PER_IP:
            srv.note("LIMIT", "%s already lists %d lobbies" % (self.peer, MAX_LOBBIES_PER_IP),
                     ("lobbies", self.peer))
            return self._send(429, {"ok": False, "error": "too many lobbies from this address"},
                              retry_after=TTL_SECONDS)
        if srv.store.count(now) >= MAX_LOBBIES:
            srv.note("FULL", "the list is full (%d lobbies)" % MAX_LOBBIES, ("full",))
            return self._send(503, {"ok": False, "error": "the server is full"}, retry_after=60)
        try:
            code = srv.store.new_code()
        except RuntimeError:
            return self._send(503, {"ok": False, "error": "no room code is free"}, retry_after=60)
        token = secrets.token_hex(16)
        fields = {
            "name": clean_name(body.get("name"), "LEGO Battles"),
            "host_name": clean_name(body.get("host_name"), "Player"),
            "address": host_address(body.get("address"), self.peer),
            "peer": self.peer,
            "port": port,
            "players": players,
            "max_players": maxp,
            "map_name": clean_name(body.get("map_name"), ""),
            "mode": clean_name(body.get("mode"), ""),
            "version": str(body.get("version") or "")[:32],
            "options": body.get("options") or {},
        }
        srv.store.announce(code, token, fields, now)
        # docs/400: the host's LAN pair, for players behind its router.
        la, lp = lan_address(body.get("lan_address")), lan_port(body.get("lan_port"))
        with srv.lock:
            if la and lp:
                srv.lan[code] = (la, lp, self.peer)
            else:
                srv.lan.pop(code, None)
        if isinstance(body.get("players_list"), list):
            srv.store.set_players(code, body["players_list"][:maxp])
        if body.get("address") and fields["address"] != str(body.get("address")):
            srv.note("ADDRESS", "%s named %r; listed at %s" % (code, str(body.get("address"))[:64],
                                                               fields["address"]), ("addr", self.peer))
        srv.log("ANNOUNCE", "%s  %s hosted by %s at %s:%d  (%d/%d)"
                % (code, fields["name"], fields["host_name"],
                   fields["address"], port, players, maxp))
        with srv.lock:   # the Discord lines' record, in memory only
            srv.meta[code] = {"host": fields["host_name"], "created": now, "state": "open",
                              "started": 0.0, "map": fields["map_name"],
                              "max": maxp, "members": [], "image": False, "peer": self.peer,
                              # A host on Both lists the same lobby on a second server
                              # quietly: the first one already posts it to the channel.
                              "quiet": bool(body.get("quiet"))}
        settings = settings_text(fields["options"])
        if not srv.meta[code]["quiet"]:
          srv.discord("\U0001F3AE **%s** is hosting a LEGO Battles lobby `%s` (up to %d players%s)%s"
                    % (plain(fields["host_name"]), code, maxp,
                       ", build " + plain(fields["version"]) if fields["version"] else "",
                       "\n" + settings if settings else ""), source=self.peer)
        return self._send(200, {"ok": True, "code": code, "token": token,
                                "ttl": TTL_SECONDS})

    def _heartbeat(self, body, now):
        srv = self.server
        code = str(body.get("code") or "").upper()
        token = str(body.get("token") or "")
        try:
            players = int(body.get("players") or 0)
        except (TypeError, ValueError):
            players = 0
        if not srv.store.heartbeat(code, token, max(0, players), now):
            srv.log("STALE", "heartbeat for unknown or expired lobby %s" % code)
            return self._send(404, {"ok": False, "error": "unknown lobby"})
        if isinstance(body.get("players_list"), list):
            srv.store.set_players(code, body["players_list"][:srv.max_players])
        self._match_state(code, body, now)
        return self._send(200, {"ok": True, "ttl": TTL_SECONDS})

    def _match_state(self, code, body, now):
        """The lobby's state from the heartbeat ("open", "starting", "playing"),
        for the Discord lines only."""
        srv = self.server
        state = str(body.get("state") or "")
        line = None
        with srv.lock:
            m = srv.meta.get(code)
            if not m:
                return
            if body.get("map_name"):
                m["map"] = clean_name(body.get("map_name"), m["map"])
            if state == "starting" and m["state"] == "open":
                m["state"] = "starting"
            if state == "playing" and m["state"] != "playing":
                m["state"] = "playing"
                m["started"] = now
                names = [m["host"]] + list(m["members"])
                if len(names) < 2 and isinstance(body.get("players_list"), list):
                    names = [clean_name(p.get("name"), "") for p in body["players_list"]
                             if isinstance(p, dict)]
                who = ", ".join("**%s**" % plain(n) for n in names if n)
                line = ("\u25B6\uFE0F **%s**'s lobby `%s` started a match%s: %s."
                        % (plain(m["host"]), code,
                           " on **%s**" % plain(m["map"]) if m["map"] else "", who))
            if state == "open" and m["state"] != "open":
                m["state"] = "open"
                m["image"] = False   # the next match gets its own picture
            if m.get("quiet"):
                line = None
            source = m.get("peer", "")
        if line:
            srv.discord(line, source=source)

    def _member(self, body, now, joining):
        """POST /join and /leave {code, name}: a player in a lobby, as the
        game's lobby reports it. Only for the Discord lines."""
        srv = self.server
        code = str(body.get("code") or "").upper()
        name = clean_name(body.get("name"), "")
        if not srv.store.lobby(code) or not name:
            return self._send(404, {"ok": False, "error": "unknown lobby"})
        line = None
        with srv.lock:
            m = srv.meta.get(code)
            if m:
                had = name.lower() in [n.lower() for n in m["members"]]
                if joining and not had and name.lower() != m["host"].lower():
                    m["members"].append(name)
                    line = ("\u2795 **%s** joined **%s**'s lobby `%s` (%d/%d)."
                            % (plain(name), plain(m["host"]), code,
                               min(m["max"], 1 + len(m["members"])), m["max"]))
                elif not joining and had:
                    m["members"] = [n for n in m["members"] if n.lower() != name.lower()]
                    line = ("\u2796 **%s** left **%s**'s %s."
                            % (plain(name), plain(m["host"]),
                               "match" if m["state"] == "playing" else "lobby `%s`" % code))
                if m.get("quiet"):
                    line = None
        if line:
            srv.discord(line, source=self.peer)
        return self._send(200, {"ok": True})

    def _remove(self, body):
        srv = self.server
        code = str(body.get("code") or "").upper()
        token = str(body.get("token") or "")
        if srv.store.remove(code, token):
            srv.log("CLOSE", "lobby %s closed by its host" % code)
            with srv.lock:
                m = srv.meta.pop(code, None)
            if m:
                self._ended(code, m, body)
            return self._send(200, {"ok": True})
        return self._send(404, {"ok": False, "error": "unknown lobby"})


class MasterServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 64

    def __init__(self, addr, store, log, max_players=DEFAULT_MAX_PLAYERS, discord=None):
        super().__init__(addr, Handler)
        self.store = store
        self.log = log
        self.max_players = max_players
        self.discord = discord or Discord([], log)
        self.meta = {}       # code -> the Discord lines' record (state, members...)
        self._last_sweep = 0.0
        # docs/400: in memory only.
        self.lock = threading.Lock()
        self.lan = {}        # code -> (lan_address, lan_port)
        self.punches = {}    # code -> [(address, port, when)]
        # issue #2: the request limits, and the connections being handled.
        self.window = Window()
        self.conn_lock = threading.Lock()
        self.conns = {}      # socket -> [address, accepted (monotonic), cut]
        self.per_ip = {}     # address -> connections being handled
        self.noted = {}      # note() key -> [last logged, dropped since]
        self.trusted_proxies = set()   # --trusted-proxy

    def note(self, kind, message, key):
        """A log line that a flood cannot multiply: once a minute per key, with
        a count of the ones left out."""
        now = time.monotonic()
        with self.conn_lock:
            n = self.noted.get(key)
            if n and now - n[0] < 60:
                n[1] += 1
                return
            if len(self.noted) > 4096:
                self.noted = {k: v for k, v in self.noted.items() if now - v[0] < 60}
            dropped = n[1] if n else 0
            self.noted[key] = [now, 0]
        self.log(kind, message + (" (and %d more like it)" % dropped if dropped else ""))

    # issue #2, finding 1: at most MAX_CONNECTIONS handled at once, and
    # MAX_CONNECTIONS_PER_IP from one address; one more is closed at once
    # instead of taking a thread. Each has READ_TIMEOUT per read (Handler) and
    # CONN_DEADLINE in all (service_actions cuts it), so a client that trickles
    # a byte at a time cannot keep a thread either.
    def process_request(self, request, client_address):
        ip = client_address[0]
        with self.conn_lock:
            refuse = (len(self.conns) >= MAX_CONNECTIONS or
                      (ip not in self.trusted_proxies and
                       self.per_ip.get(ip, 0) >= MAX_CONNECTIONS_PER_IP))
            if not refuse:
                self.conns[request] = [ip, time.monotonic(), False]
                self.per_ip[ip] = self.per_ip.get(ip, 0) + 1
        if refuse:
            self.note("BUSY", "%s refused: %d connections being handled, %d from it"
                      % (ip, len(self.conns), self.per_ip.get(ip, 0)), ("busy", ip))
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except RuntimeError:   # no thread could be started
            self._done(request)
            self.note("BUSY", "%s refused: no thread could be started" % ip, ("thread",))
            self.shutdown_request(request)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._done(request)

    def _done(self, request):
        with self.conn_lock:
            c = self.conns.pop(request, None)
            if c:
                left = self.per_ip.get(c[0], 1) - 1
                if left > 0:
                    self.per_ip[c[0]] = left
                else:
                    self.per_ip.pop(c[0], None)

    def service_actions(self):
        """Called by serve_forever about every half second: the watchdog ping
        (systemd's WatchdogSec), so a loop that stops serving stops pinging;
        and the deadline for connections held too long."""
        _watchdog_ping()
        now = time.monotonic()
        with self.conn_lock:
            late = [(s, c) for s, c in self.conns.items() if not c[2] and now - c[1] > CONN_DEADLINE]
            for _, c in late:
                c[2] = True
        for sock, c in late:
            try:
                sock.shutdown(socket.SHUT_RDWR)   # the handler's read fails and it ends
            except OSError:
                pass
            self.note("CUT", "%s held a connection past %d s" % (c[0], CONN_DEADLINE), ("cut", c[0]))

    def sweep(self, now):
        """Expire stale advertisements, at most once a second."""
        if now - self._last_sweep < 1.0:
            return
        self._last_sweep = now
        for code in self.store.expire(now):
            self.log("EXPIRE", "lobby %s stopped heartbeating" % code)
            with self.lock:
                m = self.meta.pop(code, None)
            if m and m.get("quiet"):
                continue
            if m and m["state"] == "playing":
                self.discord("\u23F9\uFE0F **%s**'s match ended (the host stopped reporting) after %s."
                             % (plain(m["host"]), lasted(now - (m["started"] or m["created"]))),
                             source=m.get("peer", ""))
            elif m:
                self.discord("\U0001F4A4 **%s**'s lobby `%s` expired (the host stopped responding)."
                             % (plain(m["host"]), code), source=m.get("peer", ""))


# ---------------------------------------------------------------- discord ----
#
# The server's lines for its operator's Discord channel, worded as the
# Cloudflare Worker's (services/master-server-cloudflare/src/index.js): a lobby
# hosted (with its settings), a player joining or leaving, the match starting
# and ending (with who won), the lobby disbanded or gone quiet. --discord-webhook
# URL (repeatable) names the channels; without one nothing is sent. One worker
# thread posts them in order; a message that fails is logged and let go, and
# the server never waits on Discord. Needs outbound TCP 443.

def plain(s, limit=100):
    """A name as it is, not as Discord's formatting would take it."""
    s = re.sub(r"[\r\n]+", " ", "" if s is None else str(s))
    return re.sub(r"([\\*_~`|>#\[\]()<:@-])", r"\\\1", s)[:limit]


def lasted(seconds):
    s = max(0, int(round(seconds)))
    if s < 60:
        return "%d s" % s
    m = int(round(s / 60.0))
    return "%d min" % m if m < 60 else "%d h %d min" % (m // 60, m % 60)


def settings_text(options):
    if not isinstance(options, dict):
        return ""
    lines = []
    for k, v in list(options.items())[:24]:
        if v in ("", None):
            continue
        if isinstance(v, (dict, list)):
            v = json.dumps(v)
        lines.append("• " + plain(k) + ": " + plain(v, 400))
    return "\n".join(lines)


class Discord:
    """issue #2: the channel is limited, every line together and per source
    address (a lobby's host, or a joiner), so a flood of lobbies or of joins
    cannot flood it. A line over a limit is dropped and counted; once lines
    flow again, one says how many were held back (at most every 10 minutes)."""

    def __init__(self, urls, log, label=""):
        self.urls = [u for u in (urls or []) if u.startswith("https://")]
        self.log = log
        self.name = "LEGO Battles server" + (" (%s)" % label if label else "")
        self.queue = None
        self.every = Bucket(*DISCORD_GLOBAL)
        self.per_source = Bucket(*DISCORD_PER_SOURCE)
        self.lock = threading.Lock()
        self.held = 0
        self.held_said = 0.0
        if self.urls:
            import queue
            self.queue = queue.Queue(maxsize=200)
            threading.Thread(target=self._run, name="discord", daemon=True).start()

    def allow(self, source):
        """Whether a line from `source` may go now (and counts it)."""
        if source and not self.per_source.take(source):
            ok = False
        else:
            ok = self.every.take("all")
        with self.lock:
            if not ok:
                self.held += 1
                if self.held == 1:
                    self.log("DISCORD", "lines held back: too many (the latest from %s)" % (source or "?"))
                return False, ""
            now = time.monotonic()
            note = ""
            if self.held and now - self.held_said > 600:
                note = "\n\u26A0\uFE0F %d lobby line%s held back (too many at once)." % (
                    self.held, "" if self.held == 1 else "s")
                self.log("DISCORD", "%d lines were held back" % self.held)
                self.held, self.held_said = 0, now
            return True, note

    def __call__(self, text, png=None, source=""):
        if not self.queue:
            return
        ok, note = self.allow(source)
        if not ok:
            return
        try:
            self.queue.put_nowait(((str(text) + note)[:1900], png))
        except Exception:
            self.log("DISCORD", "queue full; a message was dropped")

    def image(self, text, png, source=""):
        """A message with a picture: a multipart post, the file shown by an
        embed as attachment://match.png (as the Worker's discordImage)."""
        self(text, png, source)

    def _run(self):
        import urllib.request
        while True:
            text, png = self.queue.get()
            payload = {"username": self.name, "content": text,
                       "allowed_mentions": {"parse": []}}
            if png:
                payload["embeds"] = [{"image": {"url": "attachment://match.png"}}]
                boundary = "lbmaster" + secrets.token_hex(12)
                body = b"".join([
                    b"--" + boundary.encode() + b"\r\n",
                    b'Content-Disposition: form-data; name="payload_json"\r\n',
                    b"Content-Type: application/json\r\n\r\n",
                    json.dumps(payload).encode("utf-8"), b"\r\n",
                    b"--" + boundary.encode() + b"\r\n",
                    b'Content-Disposition: form-data; name="files[0]"; filename="match.png"\r\n',
                    b"Content-Type: image/png\r\n\r\n",
                    png, b"\r\n",
                    b"--" + boundary.encode() + b"--\r\n"])
                ctype = "multipart/form-data; boundary=" + boundary
            else:
                body = json.dumps(payload).encode("utf-8")
                ctype = "application/json"
            for url in self.urls:
                req = urllib.request.Request(url, data=body, method="POST", headers={
                    "Content-Type": ctype,
                    # Discord refuses urllib's default agent.
                    "User-Agent": "LEGOBattlesMasterServer (github.com/AgitationSkeleton/legobattles-pc)"})
                try:
                    with urllib.request.urlopen(req, timeout=10) as r:
                        r.read()
                except Exception as e:   # noqa: BLE001 -- any failure: let it go
                    self.log("DISCORD", "a message could not be sent (%s)" % e)


# ---------------------------------------------------------------- systemd ----
#
# The sd_notify protocol, without libsystemd: one datagram to the Unix socket
# systemd names in NOTIFY_SOCKET (an abstract one when it starts with "@").
# With Type=notify the service counts as started at READY=1, sent once the port
# is bound; with WatchdogSec it must ping WATCHDOG=1 within that time or be
# restarted. Nothing happens when NOTIFY_SOCKET is unset (any other way of
# running it, and Windows). The unit must allow AF_UNIX for this
# (RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6); see README.md.

def sd_notify(message):
    path = os.environ.get("NOTIFY_SOCKET", "")
    if not path or not hasattr(socket, "AF_UNIX"):
        return False
    if path.startswith("@"):
        path = "\0" + path[1:]
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
            s.connect(path)
            s.sendall(message.encode("utf-8"))
        return True
    except OSError:
        return False


_watchdog_every = 0.0   # seconds between pings: half of WATCHDOG_USEC, or 0
_watchdog_last = 0.0


def _watchdog_ping():
    global _watchdog_last
    if _watchdog_every <= 0:
        return
    now = time.monotonic()
    if now - _watchdog_last >= _watchdog_every:
        _watchdog_last = now
        sd_notify("WATCHDOG=1")


def _watchdog_setup():
    global _watchdog_every
    try:
        usec = int(os.environ.get("WATCHDOG_USEC", "0"))
        pid = int(os.environ.get("WATCHDOG_PID", str(os.getpid())))
    except ValueError:
        return
    if usec > 0 and pid == os.getpid():
        _watchdog_every = usec / 2e6


# ------------------------------------------------------------------- main ----

def build(port=DEFAULT_PORT, db="master.sqlite3", logfile="logs/master.log",
          echo=True, max_players=DEFAULT_MAX_PLAYERS, host="", webhooks=None, label=""):
    log = Log(logfile, echo=echo)
    store = Store(db)
    srv = MasterServer((host, port), store, log, max_players,
                       Discord(webhooks or [], log, label))
    return srv, store, log


def main(argv=None):
    global MAX_CONNECTIONS
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--host", default="")
    ap.add_argument("--db", default="master.sqlite3")
    ap.add_argument("--log", default=os.path.join("logs", "master.log"),
                    help="the log file; '-' for none (the console only, which "
                         "systemd puts in its journal)")
    ap.add_argument("--no-log-file", action="store_true",
                    help="no log file: the console only (the same as --log -)")
    ap.add_argument("--max-players", type=int, default=DEFAULT_MAX_PLAYERS,
                    help="operator ceiling on advertised max_players; the "
                         "game's own cap is not yet read from the ROM")
    ap.add_argument("--discord-webhook", action="append", default=[], metavar="URL",
                    help="post lobby and match lines to this Discord webhook "
                         "(repeatable); also LB_DISCORD_WEBHOOK, comma-separated")
    ap.add_argument("--discord-label", default="",
                    help="shown after the sender's name, e.g. King")
    ap.add_argument("--quiet", action="store_true",
                    help="write the log file but do not echo to the console")
    ap.add_argument("--max-connections", type=int, default=MAX_CONNECTIONS,
                    help="requests handled at once (one thread each); keep it "
                         "below systemd's TasksMax (default %d)" % MAX_CONNECTIONS)
    ap.add_argument("--trusted-proxy", action="append", default=[], metavar="ADDRESS",
                    help="a reverse proxy in front of this server: from it, the "
                         "client's address is read from X-Forwarded-For (repeatable)")
    args = ap.parse_args(argv)
    MAX_CONNECTIONS = max(8, args.max_connections)
    logfile = None if args.no_log_file or args.log in ("", "-") else args.log
    if logfile is None and args.quiet:
        ap.error("--quiet with no log file would log nowhere")

    hooks = list(args.discord_webhook) + [u.strip() for u in
                                          os.environ.get("LB_DISCORD_WEBHOOK", "").split(",") if u.strip()]
    srv, store, log = build(args.port, args.db, logfile, not args.quiet,
                            args.max_players, args.host, hooks, args.discord_label)
    srv.trusted_proxies = set(args.trusted_proxy)
    log("START", "LEGO Battles master server on %s:%d"
        % (args.host or "0.0.0.0", args.port))
    log("START", "database %s, log %s, ttl %ds, operator ceiling %d players"
        % (args.db, logfile or "(none: console only)", TTL_SECONDS, args.max_players))
    log("START", "limits: %d connections (%d per address), %d lobbies (%d per address), "
        "read timeout %d s, connection deadline %d s%s"
        % (MAX_CONNECTIONS, MAX_CONNECTIONS_PER_IP, MAX_LOBBIES, MAX_LOBBIES_PER_IP,
           READ_TIMEOUT, CONN_DEADLINE,
           ", trusted proxies %s" % ", ".join(sorted(srv.trusted_proxies)) if srv.trusted_proxies else ""))
    if srv.discord.urls:
        log("START", "Discord: %d webhook(s)" % len(srv.discord.urls))

    # systemd stops a service with SIGTERM: shut down as Ctrl+C does, so the
    # database is closed cleanly.
    def on_term(signum, frame):
        raise KeyboardInterrupt
    try:
        signal.signal(signal.SIGTERM, on_term)
    except (AttributeError, ValueError):
        pass
    _watchdog_setup()
    if sd_notify("READY=1\nSTATUS=serving on port %d" % args.port):
        log("START", "told systemd it is ready%s"
            % (" (watchdog every %.0f s)" % _watchdog_every if _watchdog_every else ""))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        log("STOP", "stopping")
    finally:
        sd_notify("STOPPING=1")
        srv.server_close()
        store.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
