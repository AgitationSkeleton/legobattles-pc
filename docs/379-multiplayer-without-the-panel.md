# 379 — Multiplayer from the game's own screens

Status: **in the build. The name, Join-screen and hosting fixes were seen
live on one PC. The two-PC half of joining is untested here** (one machine),
and the panel checkboxes keep the `UNSEEN` badge until a live two-PC run.

Four multiplayer defects (2026-09-25):

1. The player name isn't derived from the save's profile name.
2. The Join menu doesn't fetch lobbies from the master server.
3. Hosting doesn't auto-broadcast to the master server.
4. Joining an advertised server doesn't work.

The patches were developed and tested outside the repository as layered diffs,
then applied cleanly (`git apply --check`, then the cumulative diff). The
optional multi-peer bridge, needed only for a third player over a network, is
**not** included.

## What the cartridge says (read-only, at every vblank)

`report_net_state` (lb_patches.cpp) reads two things. Nothing writes guest
memory.

* **The network task's state word, `0x0215B7BC`.** Its only writer is
  `FUN_020F57C4`.
  * **Host side:** 10 = setup; 2 = the host lobby, open; 3 and 4 = starting;
    11 = in a match.
  * **Join side:** 9 = the Join screen asking for a scan; 6 = scanning, the
    Join Game list live; 7 = connecting; 5 = in the host's lobby; 12 = in a
    match.
  * **Leaving:** 8, then 0 or -1.
  * **Cross-check:** the cartridge's own "am I the host" test (`FUN_020F6654`)
    is exactly states 2, 3, 4 and 11.
* **The loaded profile's name, `0x02154E2D`** (11 bytes). The profile loader
  writes it there, and the Multiplayer menu copies it into the lobby record
  the other players see.

## The four defects

* **1: the name.** It was a typed setting that defaulted to "Player". A
  `master.log` from a live run says "hosted by Player", and later the typed
  name once one was entered, although the save already held a profile name. A
  new setting, "Use the save's profile name" (on), advertises the profile's
  own name; the typed name is the fallback.
* **2: the Join screen.** Only the panel's Browse button fetched lobbies.
  While the game's Join screen is open (state 9 or 6), the list is now
  fetched every 5 s and the newest listed host becomes the bridge's peer,
  unless the player picked one. Also fixed: a lobby that carried a player
  list parsed into phantom rows.
* **3: hosting.** Advertising needed a button. When the game opens a host
  lobby (state 2) it is now advertised under the profile name with the
  **real** bridge port. It is withdrawn when Start is pressed or the host
  leaves, and a manual Stop stays stopped.
* **4: joining.** Four faults, verified from live-run logs and by
  running the bridge class alone:
  * **The shipped `play.bat` never turned the wireless bridge on**
    (`--network off`, no `--local-wireless on`). With the bridge off,
    Advertise published a placeholder port 28700 that nothing binds. It
    passes `--local-wireless on` now, and Advertise refuses without a real
    port.
  * **Two copies started the same way both took instance 0**: the same UDP
    port and the same console MAC. A launch that names no index now claims
    the first free one (a named mutex per index).
  * **Across two PCs every frame was dropped.** Both default to index 0,
    and the receiver discarded frames carrying its own index. "Our own
    frame" now means one from our own port. The same condition had stopped
    a host learning such a client.
  * **The master server on the host's own PC lists the host as
    127.0.0.1**, which to another PC means itself. A client now substitutes
    the master server's own address. Also: Join by code set the bridge's
    peer from a worker thread, so it now hands the peer to the frame thread;
    and a same-PC client no longer receives every frame twice.

**Not done:** UPnP or port forwarding; a Windows Firewall rule (the first
remote bind prompts); two PCs that were given the same
`bios\generated-identity.bin`; two copies run from one folder sharing a save.

## Verification

* **Counts** (exp-gate379): identical to gate370b; the eight pictures are
  identical to gate377; 59.92 fps. Everything new either reads guest memory
  or is network code.
* **With `--local-wireless on`**, as `play.bat` now starts the game
  (exp-gate379lw): counts and pictures are still identical. The bridge
  claimed instance 0 and bound port 26710. Single-player emulation does not
  notice it.
* **Vanilla** (exp-van379): 240/240 identical.
* **The docs/216 regression** (`lb_mp.py mp379 --instances 2 --route mp
  --load --desync`): both instances reached a battle, IN SYNC in 5 of 5
  samples of a moving match.
* **Live, defects 1-3**, with the master server running on loopback and
  the same two-instance route (mp-mp379m):
  * **The master server logged:**

        ANNOUNCE  41W5  LEGO Battles hosted by Tester at 127.0.0.1:28900  (1/3)
        CLOSE     lobby 41W5 closed by its host

  * **The host logged:** `the game opened a lobby; advertising it on UDP
    28900`, then `the game closed its lobby (state 3); advertisement
    withdrawn` at Start.
  * **The joiner logged:** `Join screen: bridge pointed at lobby 41W5
    (127.0.0.1:28900)`.
  * **Both** then reached the battle, IN SYNC in 3 of 3 samples.
  * **What this shows:** "Tester" is the seeded save's profile (defect 1). The
    port is the real bridge port, not 28700 (defect 3). Nobody pressed a
    panel button (defects 2 and 3).

## What a live test must still show

Start the master server, run `play.bat` twice (give the second copy
`--save-path saves\p2.sav`), and check each of these:

* **Launch:** each log says `[local_mp] instance index N (the first free
  one)`.
* **Hosting:** host from the game's own Multiplayer > Host.
  * The panel shows the profile's name, read-only.
  * The log says "the game opened a lobby; advertising it on UDP <real
    port>".
  * `master.log` lists it under that name, not port 28700.
* **Joining:** open the game's Join screen on the other copy. It logs
  "Join screen: bridge pointed at lobby ...", and the game lists the host.
* **The match:** joining reaches a battle, and Start withdraws the
  advertisement.
* **Two PCs:** the same, with the master server on one of them.
