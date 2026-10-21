# 216 — multiplayer works

**Goal this serves.** The port's requirement that the master server and
multiplayer features be thoroughly implemented and tested. This is the first half of the "test": two
instances of the port reach a live two-player battle, host and client, through
the cartridge's own multiplayer menus. No game code is patched and no protocol
is reimplemented.

## What was already there, and had never been tried

ndsrecomp carries a DS local-wireless transport: `LocalMpTransport` in
`runner/src/wifi_net.cpp`, reached with

```
--local-wireless on --instance-index N --local-wireless-port P
```

Each instance binds a loopback UDP socket at `P + N` and broadcasts every frame
the guest's Wi-Fi hardware sends to the other fifteen instance ports, with a
24-byte header carrying a magic, the sender index, a type and a timestamp. That
is the channel two DS systems in the same room would have.

`services/master-server` is **a directory, not a relay** — its own README says
so — so it can only tell a client where a host is. Whether the cartridge's lobby
can find a peer at all therefore had to be answered on loopback first, because
if it cannot, an address from a directory is no use.

## The harness

`tools/lb_mp.py`: lb_exp's launcher and route twice over, with a private debug
port each (19861/19862), a private copy of the seed save each, and the two
routes stepped **in lockstep by index** — a host that has to be advertising
before a client searches actually is. `w:SECONDS` is a wait step, because there
is no coordinate on these screens that is reliably inert to pad with.

`wifi_net.cpp` gained a frame counter. `[local_mp] enabled` says the socket
opened; it does not say the guest ever spoke, and counting sends and receives
separately separates the three failures that look alike from outside: nobody
transmitting, one side transmitting into silence, and both transmitting with the
game not recognising what arrives.

## The route, every screen photographed

The main menu is five icons — King (Single Player) top left, a pair of pirates
(**Multiplayer**) top right, then a gift, a trophy and gears along the bottom.
Two taps, select then confirm, like every menu in this game.

| screen | host (instance 0) | client (instance 1) |
|---|---|---|
| Multiplayer | **Host**, the left icon at (72, 84) | **Join**, the right icon at (173, 84) |
| | map select → rules → army → start | "Join Game" lists `Tester` |
| Game Lobby | `Tester` red | taps the entry, joins |
| | `Tester` red + `Tester` blue, "Waiting..." | both listed, "Ready" |
| minifig select | "Waiting..." | "Ready" |
| **battle** | red base and units | blue base and units |

The Join icon is at x ≈ 173, not the 143 a look at the screenshot suggests: a
row scan at y = 84 puts the left icon's panel at x 45..106 and the right one's
at 149..198, with the gap between them at 113..145. The first attempt tapped
into that gap and nothing moved, which reads exactly like "Join does not work".

## Measured

```
host   [local_mp] frames sent=6598 recv=2
client [local_mp] frames sent=3130 recv=6520
```

The asymmetry is the protocol, not a fault: the host beacons continuously and
the client's replies come back through `RecvReplies` on the reply queue rather
than through `RecvPacket`, so they are not counted in the host's `recv`.

`screenshots/mp-hj5/i0-final.png` and `i1-final.png` are the two halves of one
battle — red base and units on the host, blue on the client, same map, different
camera.

## What this does and does not show

**Does:** the cartridge's own lobby discovery, joining, minifig select and match
start all work over the bridge, and both simulations run. Nothing about the
game's netcode needed reimplementing — docs/87-89's command codec is the
game's, and the game is speaking it to itself across two processes.

**Does not:** anything about two MACHINES — the transport is `INADDR_LOOPBACK`
and Windows-only. Part two below closes that. Nor is desync tested: both
instances ran the same build from the same save and no long match has been
played out.

---

# Part two: the same thing, by ADDRESS

The transport sent to `127.0.0.1:(base + i)` for every other instance index and
nowhere else. Reaching another machine is one addition -- also send where it is
told -- and that is the whole gap between "multiplayer works" and "multiplayer
works over the internet", because the master server already knows how to hand a
client the host's address.

## The shape

**The client knows the host; the host learns the client.** A client that picked
a lobby has an address. A host has none until someone talks to it, so it
remembers the source of the first well-formed foreign datagram and answers
there. No rendezvous protocol, no second port, nothing for the directory to
relay.

**The bind address is the one real decision.** `INADDR_ANY` makes the socket
reachable from the network, which the loopback default deliberately is not. So
it stays loopback until remote play is actually asked for -- a peer set, or
advertising a lobby -- and the socket is reopened when that changes rather than
being silently wrong.

## The hello, which the first attempt needed and did not have

With the two instances given disjoint broadcast bases and the client given the
host's address, the client sat on **"Searching for Games..."** and the host's
`recv` stayed at **0**.

The host beacons. **The client only listens while it scans.** So
learn-from-the-first-datagram cannot work: the side that knows the address never
sends anything. The side that HAS a peer therefore says hello to it -- a header
with no payload, type `LBHO`, rate-limited to four a second, which the receiver
uses only to learn an address and then drops. It carries nothing, so it can
never be mistaken for a game frame, and it stops mattering the moment real
traffic starts. It is sent from the drain, which is the one place that runs
whenever the guest is doing wireless *at all*, including while it is only
listening.

## Measured, with the loopback path disabled

`tools/lb_mp.py --remote` gives the two instances **different broadcast bases**
(28900 and 29000), which puts each outside the other's `base..base+15` sweep, so
sibling discovery cannot be what carries anything. Then `LB_MP_LISTEN=1` on the
host and `LB_MP_PEER=127.0.0.1:28900` on the client.

```
host   [local_mp] peer learned 127.0.0.1:29001
client [local_mp] frames sent=3127 recv=6523
host   [local_mp] frames sent=6598 recv=2
```

Those counts are the loopback run's to within three frames, and the run ends the
same way: `screenshots/mp-remote3/i0-final.png` and `i1-final.png` are the two
halves of one battle. The client's Join screen listed the host by name first
(`mp-remote2/i1-final.png`), which is the step the first attempt could not
reach.

## What the button does

`Join by code` is WIRED now. It is `GET /lobby/<code>`, take `address` and
`port`, hand them to the bridge -- and then the player uses the GAME's own
Multiplayer > Join screen, which sees a remote host exactly as it saw a local
one. `Advertise a lobby` is the mirror: it turns listening on and publishes the
UDP port this instance is actually bound to, rather than the placeholder 28700
that predated there being a bridge to point at.

`LB_MP_PEER=host:port` and `LB_MP_LISTEN=1` do the same from the command line,
so two machines can be tested without a human clicking anything.

## Still open

* **An actual second machine.** Everything above is one machine with the
  loopback shortcut disabled, which is as close as this environment gets. The
  mechanism is address-based and identical, but NAT and firewalls are not
  exercised, and the master server cannot help with either -- it is a directory.
* **Desync.** `SyncCheckCommand` is in the command set (docs/87) and the game
  already sends it; whether the port stays in lockstep over a long match is
  unmeasured.
* **More than two players.** Three is the cap the ROM confirms
  (`Play3PlayerMP`, docs/97) and only two have been run.


---

# Part three: through the directory

Parts one and two both handed the client its peer directly. The point of a
master server is that it should not have to be.

`services/master-server` runs, its own 18 tests pass, and the round trip is:

```
inst0  [lb_net] hosting as room NX7D
inst0  [lb_net] self-test: host status: Advertised as NX7D
inst1  [lb_net] self-test: 1 lobby(ies).
inst1  [lb_net]   NX7D  Self-test Lobby  host=SelfTest  map=Castle  2/3  at 127.0.0.1:28900
inst1  [lb_net] join lobby 0 -> peer 127.0.0.1:28900
inst1  [lb_net] self-test: join first lobby -> Peer set to 127.0.0.1:28900
```

and `screenshots/mp-ms3/i1-final.png` is the client's **Join Game** screen
listing `Tester`. The client was given **no address** — only the directory's.

Three things had to change for that to be true rather than nearly true:

* **The advertised port is the one the instance is actually bound to.** It was
  the constant `28700`, which predated there being a bridge to point at. A
  lobby advertising a port nobody is listening on is a directory entry and
  nothing else.
* **The lobby record carries the address and port.** `LbLobby` had code, name,
  host, map and a player count — everything needed to look at a listing and
  nothing needed to join one. The Multiplayer tab's table shows them now, with
  a per-row **Join** button that needs no second round trip because browsing
  already fetched the address.
* **The test had to be falsifiable.** `lb_mp --remote` normally hands the
  client `LB_MP_PEER`; it now withholds it when the client is being asked to
  find the host through `LB_MP_JOIN` instead. Otherwise a "directory test"
  would pass with the directory unplugged. `--stagger` starts the client late,
  because a client can only browse a lobby that is already in the list.

## The multiplayer requirement, audited

| item | state |
|---|---|
| master server built and tested | **done** — 18/18, and now exercised by the port rather than only by its own suite |
| advertise a lobby | **done**, with the real UDP port |
| browse lobbies | **done**, with addresses and a per-row Join |
| join by code | **done** — `GET /lobby/<code>`, badge WIRED |
| two players in one battle | **done** (parts one and two) |
| reach a host by address rather than by loopback sibling | **done** (part two) |
| an actual second machine | **not shown** — one machine with the loopback shortcut disabled is as close as this environment gets |
| desync over a long match | **not measured** |
| three players | **not tried** — three is the cap the ROM confirms (`Play3PlayerMP`, docs/97) |

---

## Addendum, 2026-09-05 — the tap sequence, which this document should have carried

This document recorded WHICH SCREENS the host and client traverse and two
coordinates, and that turned out not to be enough to reproduce the result. The
captures are numbered rather than coordinate-named, so the sequence was not
recoverable from disk either, and re-deriving it cost four runs of twenty
minutes each. Written down here so the next reader does not pay that again.

`--tap0` is the host and `--tap1` the client; they are stepped **in lockstep by
index**, and `w:S` waits instead of tapping so one side can stand still while
the other walks.

| idx | host (instance 0) | client (instance 1) | screen |
|---|---|---|---|
| 0-1 | `72,84` twice | `173,84` twice | Multiplayer: Host / Join |
| 2-5 | `205,169` x4 | `w:15` x4 | host walks map -> rules -> army -> start |
| 6 | `w:15` | `128,47` | client taps the listed lobby |
| 7-9 | `w:15` | `w:8`, `128,47`, `w:8` | the join |
| 10 | `w:10` | `205,169` | client ready |
| 11 | `212,174` | `w:8` | **host presses Continue** |

**The client must SEARCH for about a minute before tapping.** The first attempt
gave it fourteen seconds, and the capture at that step shows the list still
saying "Searching for Games..." -- so the tap hit an empty box, the next one
landed on a list mid-populate, and the game answered *"Could not join this game,
please try another."* That reads exactly like "joining is broken" and is not.

**The entry row is at y = 47, not 60.** The list box spans roughly y 35..150 and
the first row sits at its top; 60 is inside the box but below the row. Same
class of error as the Join icon being at x 173 rather than the 143 a look at the
screenshot suggests, which this document already records one paragraph up.

**`212,174` is Continue**, bottom right of the Game Lobby once a second player
is listed. It does not exist before that, which is why the host's list has
nothing there until index 11.

### What the captures prove at each stage

`screenshots/mp-hj8/i0-final.png` is the host's Game Lobby with `Tester` red AND
`Tester` blue, the blue one check-marked, both slots filled, and Continue offered.
`i1-final.png` is the client's view of the same lobby with both players
check-marked and its own button reading "Not Ready" -- which means it IS ready.

So discovery, joining and readiness all work over the loopback wireless, and
the traffic says so too: `[local_mp]` line counts went from 17 and 5 on the run
whose join failed to **206 and 301** on the run whose join succeeded.
