#!/usr/bin/env python3
"""Drive TWO runner instances at once, so multiplayer can be measured.

    python tools/lb_mp.py <label> [--tap0 X,Y[,S] ...] [--tap1 X,Y[,S] ...]

The master server and multiplayer have to be implemented and TESTED, and
nothing about multiplayer can be tested with one instance. This is
lb_exp.py's launcher and route, twice, with the two things that have to differ
between the two processes:

    --instance-index 0 / 1          which loopback UDP port the wireless
    --local-wireless on             bridge binds and sends to
    --port 19861 / 19862            a private debug port each

WHY LOCAL WIRELESS AND NOT THE MASTER SERVER FIRST. ndsrecomp already carries a
DS-wireless transport between instances (LocalMpTransport in wifi_net.cpp): a
loopback UDP socket per instance at base_port + index, broadcasting every frame
the guest's Wi-Fi hardware sends to all the other instances' ports. That is
exactly the channel two DS systems in the same room would have. The master
server is a DIRECTORY -- its own README says so, "a directory, not a relay" --
so it can only tell a client where a host is. Whether the cartridge's own lobby
can find a peer at all has to be answered on loopback first, because if it
cannot, an address from a directory is no use.

BOTH INSTANCES GET THEIR OWN SAVE. They are copies of the same seed, so both
have the same profile; sharing one file would have the second process open a
save the first has mapped and the route diverge for reasons nothing to do with
networking.
"""

import argparse
import binascii
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lb_warp  # noqa: E402   (its Dbg writes a PNG from the framebuffer)
import lb_exp  # noqa: E402

# Two ports, neither of them lb_exp's 19857, so a stray lb_exp run and a stray
# lb_mp run cannot be mistaken for each other. Same reasoning as lb_exp's own
# comment about 19842.
# THREE, because the cartridge supports three. `Play3PlayerMP` is one of
# its own unlockable-tracking entries (docs/97), which is independent
# confirmation of the three-player maximum, and the multiplayer audit had
# carried "three players" as never tried. A tool that can only run two
# instances cannot try it.
#
# --instances picks how many actually launch; the default stays 2 so every
# existing invocation behaves exactly as before.
_OFF = int(os.environ.get("LB_HARNESS_OFFSET", "0") or 0)   # see lb_exp.py
PORTS = tuple(p + _OFF for p in (19861, 19862, 19863))
# The local-wireless base port. Instance N binds base + N.
WIRELESS_BASE = 28900 + 100 * _OFF


def launch(out, index, port, extra_env, base_port=None):
    run_save = os.path.join(out, "lb%d.sav" % index)
    if os.path.exists(lb_exp.SEED_SAVE):
        shutil.copyfile(lb_exp.SEED_SAVE, run_save)

    env = dict(os.environ)
    env["NDS_DEBUG_ALLOW_WRITES"] = "1"
    # OWNER RULE (2026-10-21): an automated test never plays under the owner's
    # names. The seeded save's profile carries one, and an online test lists
    # it on the public lobby servers and in their Discord log, so each
    # instance renames its profile at the main menu (LB_TEST_RENAME, saved by
    # the game itself) to a neutral test name. --env LB_TEST_RENAME=... wins.
    env.setdefault("LB_TEST_RENAME", "Tester%d" % (index + 1))
    # Each instance records its session, which the game no longer does by
    # default (a desync or crash replays from it).
    env.setdefault("LB_DEMO_RECORD", "1")
    # On the test display, as lb_exp.py (docs/396).
    env.setdefault("LB_DISPLAY_W", "1920")
    env.setdefault("LB_DISPLAY_H", "1080")
    env["PATH"] =(os.path.join(ROOT, "build") + os.pathsep +
                   "C:\\devkitPro\\msys2\\usr\\bin" + os.pathsep +
                   env.get("PATH", ""))
    for kv in extra_env:
        k, _, v = kv.partition("=")
        env[k] = v

    log = open(os.path.join(out, "stderr%d.log" % index), "wb")
    proc = subprocess.Popen(
        [lb_exp.EXE, "dummy", "4000000000", "--rom", lb_exp.ROM,
         "--config", os.path.join(ROOT, "port", "game.toml"),
         "--boot", "direct", "--save-path", run_save,
         "--interactive", "--serve", "--port", str(port),
         "--generated-firmware", "--freebios",
         # network off keeps slirp out of it; the local-wireless bridge is a
         # separate path and is what carries DS frames between instances.
         "--network", "off",
         "--local-wireless", "on",
         "--instance-index", str(index),
         "--local-wireless-port", str(base_port or WIRELESS_BASE)],
        cwd=ROOT, env=env, stdout=log, stderr=log)
    return proc, log


def in_battle(d):
    """Is this instance in a live match?

    The same test the desync sampler uses: the entity pool exists, its array
    and count look sane, and the count is non-zero. Shared so "wait until in a
    battle" and "is this sample comparable" can never disagree about what being
    in a battle means.
    """
    try:
        def u32(a):
            return int.from_bytes(bytes.fromhex(
                d.cmd("read_mem", cpu=9, addr=a, len=4)["hex"]), "little")
        pool = u32(0x02155028)
        if not (0x02000000 <= pool < 0x02400000):
            return False
        arr = u32(pool + 8)
        count = u32(pool + 0xC)
        return (0x02000000 <= arr < 0x02400000) and 0 < count <= 4096
    except Exception:                                  # noqa: BLE001
        return False


def lobby_has_row(d):
    """Does the Join Game list have an entry yet?

    THE JOIN IS THE ONE STEP THAT CANNOT BE TIMED, and docs/216 records both
    ways of getting it wrong: tap too early and the box is still empty or
    mid-populate, and the game answers "Could not join this game, please try
    another"; tap too late, or let the host walk ahead, and the window is gone.
    Five runs, one join.

    So watch for the condition instead, exactly as `b:S` watches for a battle
    rather than sleeping a guess.

    The two states are visually unambiguous and this is what tells them apart:
    while searching, the box holds WHITE text centred low ("Searching for
    Games..."); once a host is listed, its name appears as ORANGE text at the
    TOP-LEFT of the box. So the test is orange pixels in that corner -- high
    red, middling green, low blue -- which the searching state has none of and
    the empty box has none of either.
    """
    try:
        r = d.cmd("framebuffer", engine="B")
        raw = binascii.unhexlify(r["rgb"])
        w, h = r["w"], r["h"]
        hits = 0
        for y in range(38, 58):
            if y >= h:
                break
            for x in range(55, min(215, w)):
                i = (y * w + x) * 3
                rr, gg, bb = raw[i], raw[i + 1], raw[i + 2]
                if rr > 170 and 90 < gg < 200 and bb < 90:
                    hits += 1
        return hits >= 12
    except Exception:                                  # noqa: BLE001
        return False


def connect(proc, port, label):
    for _ in range(90):
        if proc.poll() is not None:
            print("%s: runner exited %s before serving" % (label, proc.returncode))
            return None
        try:
            return lb_warp.Dbg(port=port)
        except Exception:                              # noqa: BLE001
            time.sleep(1)
    print("%s: no debug server on %d" % (label, port))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--env", action="append", default=[])
    # Per-instance, because the interesting settings are asymmetric: one side
    # advertises a lobby and the other browses the directory and joins it.
    ap.add_argument("--env0", action="append", default=[])
    ap.add_argument("--env1", action="append", default=[])
    # Route both instances to the main menu by default; the multiplayer path
    # past it is walked with --tap0/--tap1 and photographed.
    ap.add_argument("--route", choices=("menu", "mp", "none"), default="mp")
    ap.add_argument("--tap0", action="append", default=[], metavar="X,Y[,S]")
    ap.add_argument("--tap1", action="append", default=[], metavar="X,Y[,S]")
    ap.add_argument("--tap2", action="append", default=[], metavar="X,Y[,S]")
    ap.add_argument("--env2", action="append", default=[])
    ap.add_argument("--instances", type=int, default=2, choices=(2, 3))
    # Test the REMOTE path on one machine.
    #
    # The transport finds siblings by broadcasting to base..base+15, so two
    # instances sharing a base always see each other and an explicit peer would
    # prove nothing. --remote gives them DIFFERENT bases, putting each outside
    # the other's broadcast range, then wires them together with LB_MP_LISTEN
    # on the host and LB_MP_PEER on the client. If they still reach a lobby, it
    # is the peer that carried it.
    ap.add_argument("--remote", action="store_true")
    # Start the second instance N seconds late. A client browsing a directory
    # can only find a lobby that is already IN it, and both instances otherwise
    # query at the same moment during boot.
    ap.add_argument("--stagger", type=float, default=0.0)
    ap.add_argument("--linger", type=int, default=10)
    ap.add_argument("--load", action="store_true",
                    help="order units to move during the linger, so the desync check has a MOVING match to compare")
    ap.add_argument("--desync", action="store_true",
                    help="sample both worlds during the linger and report the first divergence")
    args = ap.parse_args()

    # --load NEEDS the entity log: it reads the unit's position out of it. It
    # used to be the caller's job to remember, and forgetting produced a run
    # that looked like a clean pass ("IN SYNC throughout") on a world that had
    # never moved.
    if args.load:
        args.env = list(args.env) + ["LB_ENTLOG=1"]
    out = os.path.join(ROOT, "screenshots", "mp-" + args.label)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)

    # TEE STDOUT, for the same reason lb_exp does. A run is twenty minutes and
    # the interesting line is often not the last one; piping through `tail`
    # lost the detail of a divergence that took twenty minutes to produce.
    class _Tee(object):
        def __init__(self, stream, path):
            self.stream = stream
            self.fh = open(path, "w", encoding="utf-8")

        def write(self, t):
            self.stream.write(t)
            self.fh.write(t)
            self.fh.flush()

        def flush(self):
            self.stream.flush()
            self.fh.flush()

    sys.stdout = _Tee(sys.stdout, os.path.join(out, "stdout.log"))

    procs, logs, dbgs = [], [], []
    try:
        # ONE BASE PER INSTANCE. This was a 2-tuple, so a third instance
        # indexed past the end and the run died with IndexError before it
        # launched anything -- which the caller never saw, because the command
        # sent stderr to /dev/null and only grepped the log the crash had
        # prevented from being written.
        #
        # --remote gives each instance its own base so none of them is inside
        # another's broadcast range; otherwise they share one and find each
        # other by broadcast, which is the ordinary local-wireless case.
        bases = ([WIRELESS_BASE + 100 * i for i in range(args.instances)]
                 if args.remote else [WIRELESS_BASE] * args.instances)
        for i, port in enumerate(PORTS[:args.instances]):
            env = list(args.env) + list(
                (args.env0, args.env1, args.env2)[i])
            if args.remote:
                if i == 0:
                    env.append("LB_MP_LISTEN=1")
                elif not any(e.startswith("LB_MP_JOIN") for e in env):
                    # Only hand the client the address directly when it is not
                    # being asked to get it from the DIRECTORY. Doing both
                    # would make a directory test unfalsifiable.
                    env.append("LB_MP_PEER=127.0.0.1:%d" % bases[0])
            if i and args.stagger:
                print("stagger: %.1fs before instance 1" % args.stagger)
                time.sleep(args.stagger)
            p, lg = launch(out, i, port, env, bases[i])
            procs.append(p)
            logs.append(lg)
        for i, port in enumerate(PORTS[:args.instances]):
            d = connect(procs[i], port, "inst%d" % i)
            if d is None:
                return 2
            dbgs.append(d)
        print("%d instances serving on %s"
              % (args.instances, PORTS[:args.instances]))

        # Wait for both title screens, the same colour census lb_exp uses.
        for i, d in enumerate(dbgs):
            for _ in range(60):
                try:
                    if lb_exp.title_blue(d) > 0.5:
                        break
                except Exception:                      # noqa: BLE001
                    pass
                time.sleep(1)
            else:
                print("inst%d: never saw the title screen" % i)

        base = {"menu": lb_exp.ROUTE[:4], "mp": lb_exp.MP_ROUTE,
                "none": []}[args.route]
        # "X,Y[,S]" taps, and "w:S" to WAIT instead.
        #
        # The two routes are stepped in lockstep by index, so the client needs
        # somewhere to stand while the host walks map select, rules, army and
        # start. Padding with a harmless tap is not the same thing: there is no
        # coordinate on these screens that is reliably inert.
        # THE WORKING CHOREOGRAPHY LIVES HERE NOW, not in somebody's shell
        # history.
        #
        # This is the direct cause of a whole session's detour. A run DID put
        # three instances in one battle, and its command was never written
        # down; rebuilding it from docs/216 produced a route that failed with
        # all three still in the lobby, and two more runs went into working out
        # why. A result that cannot be re-run is barely a result.
        #
        # So `--route mp` with no --tapN supplies these. They are the routes
        # that are measured to work, and passing --tapN still overrides them
        # completely for one-off experiments.
        #
        #   host    map select, rules, army, Continue, then Start attempts
        #   client  join, wait for a real lobby row, Ready, minifig, Ready
        #
        # The host's Start MUST be many short `s:` steps rather than one long
        # one: each route step blocks every other instance for its duration, so
        # a single long wait starves the client that still has to ready up.
        # The client-2 stagger is five `w:20` at the FRONT -- two clients
        # searching at once is what stops a join -- which makes its route five
        # steps longer, which is why the host needs 20 attempts and not 8.
        MP_HOST = (["72,84", "72,84"] + ["205,169"] * 4
                   + ["w:15", "w:15", "w:10"] + ["w:8"] * 4
                   + ["212,174"] + ["s:10"] * 20)
        MP_CLIENT = (["173,84", "173,84", "w:5", "l:90"] + ["w:8"] * 4
                     + ["212,174"] + ["w:8"] * 5
                     + ["120,25", "w:3", "212,174"] + ["b:10"] * 30)
        MP_CLIENT2 = ["w:20"] * 5 + MP_CLIENT

        given = (args.tap0, args.tap1, args.tap2)
        if args.route == "mp" and not any(given[:args.instances]):
            given = (MP_HOST, MP_CLIENT, MP_CLIENT2)
            print("using the built-in mp routes (pass --tapN to override)")

        routes = []
        for taps in given[:args.instances]:
            r = list(base)
            for spec in taps:
                if spec.startswith("w:"):
                    r.append((None, None, float(spec[2:])))
                    continue
                # "b:S" -- WAIT UNTIL THIS INSTANCE IS IN A BATTLE, up to S
                # seconds. A fixed sleep is a guess about how long a join
                # takes, and with three emulators contending for one machine
                # that guess is wrong in both directions: fourteen seconds had
                # a client tap an empty lobby list, and lengthening every wait
                # then pushed the host's Continue so late that NONE of the
                # three reached a battle. Two runs, two opposite failures, one
                # cause -- the model, not the numbers.
                #
                # Waiting on the observable state instead costs nothing when
                # the join is quick and does not fall apart when it is slow.
                # "l:S" -- WAIT UNTIL THE LOBBY LIST HAS A ROW, up to S
                # seconds, THEN TAP IT ONCE. One tap, on a populated list, is
                # the only thing that works; see lobby_has_row.
                if spec.startswith("l:"):
                    r.append(("lobby", None, float(spec[2:])))
                    continue
                if spec.startswith("b:"):
                    r.append(("battle", None, float(spec[2:])))
                    continue
                # "s:S" -- PRESS START UNTIL THE MATCH ACTUALLY STARTS, up to S
                # seconds.
                #
                # The host's Start is the last timing guess left in this route,
                # and staggering a third player is exactly what breaks it. The
                # host's button only becomes Start once EVERY client has readied
                # twice (docs/235), so a Start on a fixed step index is placed
                # correctly for two players and a hundred seconds too early for
                # three -- the tap lands on a button still reading "Waiting...",
                # does nothing, and is never repeated. That is precisely how the
                # first rebuilt three-player route failed: all three instances
                # sat in the lobby and even the HOST, which had reached a battle
                # in every previous run, reported STILL not in a battle.
                #
                # Pressing until the condition holds costs nothing when the
                # match starts at once and does not care how long the clients
                # take, which is the same reasoning behind l: and b:.
                if spec.startswith("s:"):
                    r.append(("start", None, float(spec[2:])))
                    continue
                q = spec.split(",")
                r.append((int(q[0]), int(q[1]),
                          float(q[2]) if len(q) > 2 else 2.5))
            routes.append(r)

        # IN LOCKSTEP, one step at a time on both instances, so a host that
        # has to be advertising before a client searches actually is. Running
        # one route to completion and then the other would make every timing
        # question unanswerable.
        # max over EVERY route, not just the first two. With --instances 3 the
        # third client's route is the one that needs leading waits -- a second
        # client searching while the first is mid-join is what stops the join
        # (see below) -- so it is reliably the LONGEST, and bounding the loop
        # by routes[0] and routes[1] silently dropped its tail. The run that
        # first reached three players lost its last five steps this way and
        # only succeeded because the battle wait that follows the route caught
        # up. Nothing about that was visible in the output.
        started = {}
        warned = {}
        for n in range(max(len(r) for r in routes)):
            for i, d in enumerate(dbgs):
                if n >= len(routes[i]):
                    continue
                x, y, wait = routes[i][n]
                if x == "lobby":
                    t_end = time.time() + wait
                    saw = False
                    while time.time() < t_end:
                        if lobby_has_row(d):
                            saw = True
                            break
                        time.sleep(1.0)
                    if saw:
                        # BOTH SCREENS, once, at the moment the row appears.
                        #
                        # Every capture in this harness is engine B, and the
                        # tap that ought to join is delivered correctly to the
                        # touch struct and ignored. "Which screen am I looking
                        # at" is therefore a question worth answering with a
                        # picture rather than from memory.
                        for eng in ("A", "B"):
                            d.shot(os.path.join(out,
                                                "lobby-i%d-%s.png" % (i, eng)),
                                   engine=eng)
                        # Settle briefly so the tap lands on a drawn row rather
                        # than on one mid-draw, which is the "mid-populate"
                        # failure docs/216 describes.
                        time.sleep(1.5)
                        # SELECT, THEN CONFIRM. docs/216's table taps the list
                        # twice for a reason: the first tap highlights the row
                        # and the second joins it. Its two taps were fifteen
                        # seconds apart and separately timed, which is why one
                        # of them kept landing on an empty or half-drawn list;
                        # both land on a populated one now.
                        #
                        # On the row TEXT, not the middle of the box. The name
                        # is drawn at the left and a tap at x 128 selected the
                        # row without ever joining it.
                        # RELEASE WHERE THE PRESS WAS. This is why the taps
                        # were being ignored, and it was mine, not the game's:
                        # releasing at (0,0) puts the release 88 px from the
                        # press, and the cartridge's gesture recogniser calls
                        # anything further than about twelve a DRAG rather than
                        # a tap (docs/234's FUN_020AF040 -- the same threshold
                        # this session traced for the band clicks). lb_exp.tap,
                        # which every working route step uses, releases at the
                        # same coordinate; this did not.
                        # RE-CHECK BEFORE EACH TAP, not once before both.
                        #
                        # With two instances a single settle was enough. With
                        # THREE the machine is slower, the list can be redrawn
                        # between the wait and the tap, and the tap then lands
                        # on a half-populated row -- which is exactly the
                        # "Could not join this game, please try another" that
                        # both clients hit in the three-player run.
                        #
                        # Waiting for the row and then trusting it is still a
                        # timing assumption, just a shorter one. Confirming it
                        # is still there immediately before each tap removes
                        # the assumption entirely.
                        for k in range(2):
                            t2 = time.time() + 20.0
                            while not lobby_has_row(d) and time.time() < t2:
                                time.sleep(0.5)
                            if not lobby_has_row(d):
                                print("inst%d: row vanished before tap %d; "
                                      "not tapping" % (i, k + 1))
                                break
                            lb_exp.tap(d, 75, 47, 1.2)
                        print("inst%d: lobby row seen, selected and confirmed"
                              % i)
                    else:
                        print("inst%d: NO lobby row after %.0fs -- not tapped, "
                              "because tapping an empty list is what produces "
                              "\"Could not join this game\"" % (i, wait))
                elif x == "battle":
                    # SHORT AND REPEATED, for the same reason as "s:".
                    #
                    # A single `b:300` blocks every other instance for five
                    # minutes. That is how a three-player run ended with client
                    # 1 reporting "STILL not in a battle after 300s" while its
                    # final capture plainly showed it IN one: its wait ran to
                    # completion BEFORE the host had been allowed to reach the
                    # Start attempts that began the match. The instance was
                    # never late; the report was early, and the run had starved
                    # the very thing it was waiting for.
                    if started.get(i):
                        pass
                    elif in_battle(d):
                        started[i] = True
                        print("inst%d: in a battle" % i)
                    else:
                        t_end = time.time() + wait
                        while time.time() < t_end:
                            if in_battle(d):
                                started[i] = True
                                print("inst%d: in a battle" % i)
                                break
                            time.sleep(1.0)
                        else:
                            if not warned.get(i):
                                warned[i] = True
                                print("inst%d: not in a battle yet; still "
                                      "watching" % i)
                elif x == "start":
                    # ONE press, then wait up to S seconds -- and S must be
                    # SMALL, with the step repeated, rather than one long step.
                    #
                    # The first version of this blocked for the whole timeout
                    # inside a single route step, and the loop it lives in runs
                    # step n on EVERY instance before step n+1. So a host step
                    # that waits 180 seconds starves every other instance for
                    # 180 seconds: the host pressed Start 29 times before the
                    # client had been given the steps that pick a minifig and
                    # ready up. Both a two-player and a three-player run failed
                    # exactly that way, with the host's button reading `Start`,
                    # the clients still on `Waiting...`, and nothing wrong with
                    # either coordinate.
                    #
                    # Short and repeated interleaves correctly: every other
                    # instance advances one step between two presses.
                    if in_battle(d):
                        if not started.get(i):
                            started[i] = True
                            print("inst%d: in a battle" % i)
                    else:
                        try:
                            lb_exp.tap(d, 212, 174, 1.0)
                        except Exception:              # noqa: BLE001
                            pass
                        t_end = time.time() + wait
                        while time.time() < t_end:
                            if in_battle(d):
                                started[i] = True
                                print("inst%d: in a battle after a Start press"
                                      % i)
                                break
                            time.sleep(1.0)
                elif x is None:
                    time.sleep(wait)
                else:
                    lb_exp.tap_verified(d, x, y, wait)
            for i, d in enumerate(dbgs):
                d.shot(os.path.join(out, "i%d-%02d.png" % (i, n)))

        if args.linger and args.desync:
            # DESYNC, MEASURED DIRECTLY.
            #
            # Multiplayer has to be tested, and the item left open in the
            # audit is "desync over a long match". The
            # cartridge has its own SyncCheckCommand (docs/87) carrying two
            # 32-bit fields, but reading that means parsing the game's command
            # stream out of the wireless payload -- and it would only tell us
            # what the GAME thinks, which is a second thing to trust.
            #
            # Desync means the two machines' worlds have diverged, so compare
            # the worlds. Both instances expose a debug server, so the same
            # entity state can be read from each and compared: the pool's
            # count, and every entity's position. Identical is in sync;
            # anything else is the divergence, with the sample that first
            # showed it.
            #
            # Positions, not just the count. Two sides can hold the same number
            # of units while one has walked somewhere the other has not, which
            # is the ordinary way an RTS desyncs.
            def state_sig(d):
                def u32(a):
                    return int.from_bytes(bytes.fromhex(
                        d.cmd("read_mem", cpu=9, addr=a, len=4)["hex"]),
                        "little")
                try:
                    pool = u32(0x02155028)
                    if not (0x02000000 <= pool < 0x02400000):
                        return None
                    arr = u32(pool + 8)
                    count = u32(pool + 0xC)
                    if not (0x02000000 <= arr < 0x02400000) or count > 4096:
                        return None
                    parts = []
                    for i in range(min(count, 24)):
                        e = u32(arr + i * 4)
                        if not (0x02000000 <= e < 0x02400000):
                            parts.append("-")
                            continue
                        parts.append("%08X:%08X" % (u32(e + 0xEC),
                                                    u32(e + 0xF0)))
                    return (count, ",".join(parts))
                except Exception:                      # noqa: BLE001
                    return None

            # GIVE THE MATCH SOMETHING TO DO.
            #
            # The first load attempt tapped guessed coordinates and the tool
            # reported "the world never changed between samples" -- the taps
            # selected nothing and both worlds sat frozen, which makes an
            # in-sync verdict worth very little.
            #
            # This aims at where the units actually ARE. Both instances print
            # [lb_builder] and [lb_hero] with LB_ENTLOG=1, so each one's own log
            # says where its own units stand; tap the unit, then tap a patch of
            # ground away from it, which is a move order. Repeated through the
            # linger so the match keeps moving rather than settling.
            #
            # +12 in y, because the sprite is drawn BELOW the position the
            # entity reports -- measured for the click probe (docs/226's
            # calibration sweep: dy 0 misses, dy 6 and 12 select).
            # WHOSE unit, and how that was wrong for every client.
            #
            # This used to require "owner=0" in the line, which is right for the
            # host and wrong for everybody else: a client is owner 1 or 2, and
            # owner 0's builder is nowhere near its camera. The three-player run
            # showed it exactly --
            #
            #     inst0  [lb_builder] sx=187  sy=36   owner=0
            #     inst1  [lb_builder] sx=-769 sy=-496 owner=0
            #     inst2  [lb_builder] sx=-769 sy=32   owner=0
            #
            # -- so both clients found only a unit 769 pixels off the left of
            # their screen, rejected it, and ordered nothing. Two of the three
            # worlds sat still while the tool reported them "in sync".
            #
            # Pick by what is ON SCREEN instead, which needs no owner index and
            # is the same reasoning docs/235 used for the music latch: each
            # instance opens centred on its OWN base, so the unit in view at the
            # start of a match is its own. Nearest the centre wins when several
            # are visible.
            def unit_at(i, tag):
                try:
                    with open(os.path.join(out, "stderr%d.log" % i), "rb") as f:
                        t = f.read()[-400000:].decode("latin-1", "replace")
                except OSError:
                    return None
                best, best_d = None, None
                seen = set()
                for ln in reversed(t.splitlines()):
                    if not ln.startswith(tag):
                        continue
                    try:
                        ptr = ln.split("ptr=")[1].split()[0]
                        if ptr in seen:          # one entry per entity, newest
                            continue
                        seen.add(ptr)
                        x = int(ln.split("sx=")[1].split()[0])
                        y = int(ln.split("sy=")[1].split()[0])
                    except (IndexError, ValueError):
                        continue
                    if not (0 <= x < 256 and 0 <= y < 192):
                        continue
                    d2 = (x - 128) ** 2 + (y - 96) ** 2
                    if best_d is None or d2 < best_d:
                        best, best_d = (x, y), d2
                    if len(seen) > 60:
                        break
                return best

            def stir(n):
                if not args.load:
                    return
                for i, d in enumerate(dbgs):
                    p = unit_at(i, "[lb_builder]") or unit_at(i, "[lb_hero]")
                    if p is None:
                        # SAY SO. This read the unit's position out of the
                        # port's own entity log, which only exists with
                        # LB_ENTLOG=1 -- so without it `--load` found nothing,
                        # tapped nothing, and the desync run then reported "the
                        # world never changed" without ever saying why. A
                        # generator that does nothing in silence is worse than
                        # one that is absent.
                        if n % 4 == 0:
                            print("desync: inst%d has no unit on its own screen "
                                  "to order (LB_ENTLOG=1 is set by --load; if "
                                  "the log is empty that is the reason)" % i)
                        continue
                    try:
                        # ON the unit, not twelve pixels below it. That offset
                        # dates from when selection was unreliable and every
                        # probe carried an aim fudge; docs/234 fixed the cause,
                        # and --bandselect now taps the reported position
                        # directly and selects 4 of 4.
                        lb_exp.tap(d, p[0], p[1], 0.8)
                        # Somewhere else on the map, alternating so the unit is
                        # sent back and forth rather than parked at one edge.
                        tx = 40 if (n // 2) % 2 else 210
                        ty = 150 if n % 2 else 40
                        lb_exp.tap(d, tx, ty, 0.8)
                    except Exception:                  # noqa: BLE001
                        pass

            print("linger: %ds, sampling both worlds for divergence%s"
                  % (args.linger, ", ordering units to move" if args.load
                     else ""))
            first_bad = None
            samples = 0
            # HOW MANY SAMPLES ACTUALLY COMPARED TWO BATTLES.
            #
            # Without this the summary is a lie waiting to happen: the first
            # run of this reported "IN SYNC throughout" after 36 consecutive
            # samples that each said "one side not in a battle yet". Nothing
            # was ever compared, and the tool announced success -- the same
            # shape as every other measurement this project has had to throw
            # away, and the reason every probe here now carries a control.
            compared = 0
            # DID THE WORLD MOVE AT ALL?
            #
            # Two worlds that agree prove nothing if neither is doing anything.
            # A twelve-minute run reported 75 samples in sync with 16 entities
            # each, which is a real result only if those entities were CHANGING
            # -- otherwise it says two static snapshots match, and a desync test
            # that cannot see motion cannot see a desync.
            #
            # So count the samples whose signature differs from the one before.
            # Zero means the match sat idle and the run is weak evidence, and
            # the summary has to say so rather than let the reader assume.
            moved = 0
            bad = 0
            skewed = 0
            prev = None
            t0 = time.time()
            while time.time() - t0 < args.linger:
                # EVERY instance against the first, not just a pair. With three
                # players two of them can agree while the third has drifted, and
                # comparing only 0 against 1 would call that in sync.
                sigs = [state_sig(d) for d in dbgs]
                # SKEW IS NOT DESYNC, and this is the only way to tell.
                #
                # The instances are read one after another over a debug socket,
                # so a MOVING entity legitimately differs between them: the
                # first instance read is simply older than the last. A
                # three-player run under load reported 7 of 18 samples diverged
                # -- and the detail gave it away, because inst1 and inst2 agreed
                # with each other while inst0, the one read FIRST, was behind by
                # about three map units. That is the signature of a stale read,
                # not of two worlds disagreeing.
                #
                # So re-read the first instance afterwards. If it changed while
                # the others were being read, the whole sample straddles a move
                # and cannot distinguish skew from divergence -- so it is thrown
                # out rather than counted either way. Under load that discards
                # real samples, which is the correct trade: a desync test that
                # reports motion as divergence is worse than one that waits.
                recheck = state_sig(dbgs[0]) if len(dbgs) > 1 else sigs[0]
                samples += 1
                if (sigs[0] is not None and recheck is not None
                        and recheck != sigs[0]):
                    skewed += 1
                    print("desync: sample %d SKEWED -- the world moved while "
                          "the instances were being read; not counted"
                          % samples)
                    stir(samples)
                    time.sleep(5.0)
                    continue
                if any(g is None for g in sigs):
                    print("desync: sample %d -- %d of %d not in a battle yet"
                          % (samples, sum(1 for g in sigs if g is None),
                             len(sigs)))
                elif any(g != sigs[0] for g in sigs[1:]):
                    compared += 1
                    bad += 1
                    if first_bad is None:
                        first_bad = samples
                        print("desync: DIVERGED at sample %d (%.0fs in)"
                              % (samples, time.time() - t0))
                        for k, g in enumerate(sigs):
                            print("desync:   inst%d count=%s" % (k, g[0]))
                        for k, g in enumerate(sigs[1:], start=1):
                            for j, (p, q) in enumerate(zip(
                                    sigs[0][1].split(","), g[1].split(","))):
                                if p != q:
                                    print("desync:   inst0 vs inst%d  "
                                          "entity %d  %s vs %s"
                                          % (k, j, p, q))
                else:
                    compared += 1
                    if prev is not None and sigs[0] != prev:
                        moved += 1
                    prev = sigs[0]
                    print("desync: sample %d in sync across %d, %d entities"
                          % (samples, len(sigs), sigs[0][0]))
                stir(samples)
                time.sleep(5.0)
            # THE MOTION LINE ALWAYS PRINTS, whatever the verdict. It was
            # inside the in-sync branch, so a run that diverged reported no
            # motion information at all -- and "did the world move" is exactly
            # what decides whether a divergence is meaningful.
            if skewed:
                print("desync: %d sample(s) discarded as SKEWED -- the world "
                      "moved mid-read, so they could not tell skew from "
                      "divergence" % skewed)
            if compared:
                if moved == 0:
                    print("desync: WARNING -- the world never changed between "
                          "samples. Two IDLE worlds agreeing is weak evidence; "
                          "a desync shows up when units move.")
                else:
                    print("desync: the world changed in %d of %d comparisons, "
                          "so this was a moving match, not a frozen one"
                          % (moved, compared))
            if compared == 0:
                print("desync: %d sample(s) but NONE compared two battles -- "
                      "this run measured nothing. Both instances have to be IN "
                      "a match; the mp route only reaches the menu."
                      % samples)
            elif first_bad is None:
                print("desync: %d sample(s), %d compared two live battles, "
                      "IN SYNC throughout" % (samples, compared))

            else:
                # A DIVERGENCE AT THE START IS NOT THE SAME AS A DESYNC.
                #
                # The two instances do not enter the battle on the same frame,
                # so the first sample or two can catch one side with a world
                # the other has not built yet. That is a startup artifact.
                # A real desync appears once both are running and STAYS.
                # Reporting only "first divergence at sample 1" cannot tell
                # those apart, so report how many samples diverged in total and
                # whether the last one did.
                print("desync: %d sample(s), %d compared, %d DIVERGED "
                      "(first at sample %d, last sample %s)"
                      % (samples, compared, bad, first_bad,
                         "diverged" if prev is not None and bad and
                         first_bad == samples else "in sync"))
                if bad == 1 and first_bad <= 2:
                    print("desync: one divergence, at the very first sample -- "
                          "that is the two instances entering the battle a "
                          "moment apart, not a desync. The rest were in sync.")
        elif args.linger:
            print("linger: %ds" % args.linger)
            # docs/396: EACH INSTANCE'S FRAME RATE IN THE MATCH, and where its
            # frame goes, over the linger -- the measurement the first online
            # playtest ("both dip to 30 fps") needed and this tool did not take.
            # Ticks are SDL performance-counter ticks; the counter's rate is
            # read from frontend_stats when it reports one, else 10 MHz.
            keys = ("emu_ticks", "present_ticks", "drain_ticks", "swap_ticks")
            before = []
            for d in dbgs:
                try:
                    before.append(d.cmd("frontend_stats"))
                except Exception:              # noqa: BLE001
                    before.append({})
            t0 = time.time()
            time.sleep(args.linger)
            dt = time.time() - t0
            for i, d in enumerate(dbgs):
                try:
                    b = d.cmd("frontend_stats")
                except Exception:              # noqa: BLE001
                    continue
                a = before[i]
                frames = b.get("frames", 0) - a.get("frames", 0)
                hz = float(b.get("perf_freq", 10_000_000) or 10_000_000)
                parts = []
                for k in keys:
                    if k in b and k in a and frames > 0:
                        parts.append("%s %.2f ms" % (
                            k.replace("_ticks", ""),
                            (b[k] - a[k]) / hz * 1000.0 / frames))
                print("inst%d: %.1f fps over %.0fs; per frame: %s"
                      % (i, frames / dt if dt else 0.0, dt, ", ".join(parts)))
        for i, d in enumerate(dbgs):
            d.shot(os.path.join(out, "i%d-final.png" % i))

        # What the wireless bridge actually carried. "[local_mp] enabled" says
        # the socket opened; a frame count says the guests spoke.
        for i in range(args.instances):
            with open(os.path.join(out, "stderr%d.log" % i), "rb") as fh:
                txt = fh.read().decode("latin-1", "replace")
            hits = [l for l in txt.splitlines() if "local_mp" in l]
            print("inst%d: %d [local_mp] line(s)%s"
                  % (i, len(hits), (": " + hits[0]) if hits else ""))
        print("captures in %s" % out)
        return 0
    finally:
        for p in procs:
            try:
                p.terminate()
            except Exception:                          # noqa: BLE001
                pass
        for lg in logs:
            lg.close()


if __name__ == "__main__":
    raise SystemExit(main())
