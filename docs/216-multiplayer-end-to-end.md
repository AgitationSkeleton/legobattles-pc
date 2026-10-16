
---

## Addendum, 2026-09-05: the table is incomplete, and where it stops

Three runs, chasing an actual battle so the music setting could be
measured in multiplayer rather than argued for. None reached one, but they
establish two things the table above does not say.

**The flow does not end at the host's Continue.** The table's last row is
"host presses Continue" at index 11, which reads as if the match starts there.
It does not. After the lobby there is a **minifig / army select** screen — a
5x5 grid with a button at the bottom right — and both players must ready up on
it before the battle loads. The run that got furthest ended with the host
showing `Waiting...` and the client `Ready` on that screen, which is a state the
table has no row for.

**The host's Continue is at 212,174 on the map-select screen too, and 205,169
does not hit it.** The table has the host tapping `205,169` four times at
indices 2-5 to "walk map -> rules -> army -> start"; the capture at index 5
shows it still on map select, with a Continue button plainly at the bottom
right. So those four taps are not doing what the row says they do. What the
lobby capture at index 11 shows is that the host arrives there anyway, which is
why this was never noticed.

**And the join is genuinely timing-flaky, in both directions.** Two variants:

| variant | client search before its first tap | result |
|---|---|---|
| the table's, 4 x `w:15` | 60 s | client joined; both reached minifig select |
| host taps Continue at index 2, client waits 30 s | 30 s | client never appeared in the lobby |
| host taps Continue at index 2, client waits 60 s | 60 s | client never appeared in the lobby |

The third is the interesting one: restoring the full search time did NOT restore
the join once the host had advanced quickly. So the client's search window has
to overlap something the host is doing, and walking the host forward faster
closes it. That is a different failure from the "fourteen seconds is not enough"
one this document already records, and it means the two sides are coupled rather
than independently timed.

**A fifth run, and retrying is the wrong fix.** Having the client tap the lobby
list four times at ten-second intervals -- on the theory that its search window
simply has to overlap the host's advertising -- produced
*"Could not join this game, please try another."*, which is the failure this
document already describes one section up: a tap landing on a list
mid-populate. So the tap must land ONCE, on a populated list, and neither
tapping earlier, later, faster nor more often achieves that reliably.

**Five runs, one join.** The earlier claim -- "two players in one battle, done
and now reproducible" -- is too strong. One of five reached a two-player lobby and then minifig select; the other
four failed in three different ways.

Recorded rather than solved. What it costs, concretely: the multiplayer row of
the music audit stays unmeasured, and `lb_music_set_local_owner` cannot be wired
until a battle can be reached, because the game's own local-player index is what
it needs and that index only exists once a match is running.

**What would actually fix it** is the same move that fixed every other flaky
wait in this project: stop timing it and watch for the condition. `b:S` already
waits until an instance is IN A BATTLE rather than sleeping a guessed interval.
The client needs the equivalent for the lobby list -- wait until the list has a
row, THEN tap it once -- which is a screen test on the capture the harness
already takes at every step. That is the next change here, and it is a harness
change rather than another guess at coordinates.

## Addendum 2: the timing question is removed, and the taps still do not join

`lb_mp.py` gained `l:S` -- wait until the Join Game list has a row, then tap it
-- built the same way as `b:S`. The detector is a colour test rather than a
timer: while searching, the box holds WHITE text centred low; once a host is
listed, its name is ORANGE at the top-left. Orange pixels in that corner exist
in neither the searching state nor the empty box.

It works, and it fires at the right moment in every run.

**With the timing question removed, the taps still do not join.** On a list this
document's own coordinates say is populated:

* one tap at `128,47` -- the client stays on the list;
* select-then-confirm at `75,47`, on the row TEXT, 1.2 s apart -- the client
  stays on the list;
* and no capture shows a selection highlight at any point.

That matters because the table above credits those taps with the join. They were
separately timed and fifteen seconds apart, so a run that reached the lobby could
always be explained as "the timing happened to work" -- and a run that did not,
as "the timing did not". Removing the timing removes that explanation, and what
is left is that tapping the row does not appear to consume the tap at all.

**Not a port regression.** Menu taps work: the Free Play route, which is nothing
but menu taps, reached a battle with entities on this same build.

## Addendum 3: it was the harness's tap, and the join works now

The taps were being ignored and the reason was the harness, not the game.
`lb_mp.py`'s new `l:S` handler pressed at the row and **released at (0,0)** --
eighty-eight pixels away. The cartridge's gesture recogniser calls anything
further apart than about twelve pixels a DRAG rather than a tap: that is
`FUN_020AF040`'s threshold, the same one docs/234 traced from the other side
while fixing band clicks.

`lb_exp.tap`, which every working route step already used, releases at the same
coordinate. The new handler did not. Using it, **the client joins on the first attempt**,
every run.

So addendum 2's conclusion -- "tapping the row does not join" -- is retracted.
It does. The tap was never a tap.

**Also corrected: which screen is which.** Every capture in this harness is
engine B, and it was worth a picture rather than a memory. Engine A during
menus is the LEGO Battles title art -- the top screen -- so **engine B is the
touch screen**, and the Join Game list has been on the interactive screen all
along. The note that "Engine A = BOTTOM screen" holds for GAMEPLAY, not for
menus; the DS can swap which engine drives which physical panel.

## Where it stops now

With the join fixed, both instances reliably reach **minifig / army select**
together, and stop there:

* host: a minifig is selected and the bottom-right button reads `Waiting...`
* client: the bottom-right button reads `Ready`

Neither advances. Tapping `212,174` and `205,169` on either side changes
nothing, so neither is the control that starts the match, and the two labels do
not disambiguate themselves -- "Ready" could be a button to press or a status
already reached, and the same for "Waiting...".

That is the whole of what is left, and it is a much smaller question than the
one this document opened with. The next step is to find the start control on
that screen, which now costs one run rather than five: the route reaches the
screen reliably, so a sweep of candidate coordinates with a capture after each
will name it.

## Addendum 4: the complete sequence, and a battle

Both instances reach a live match: `inst0: in a battle after 0s`,
`inst1: in a battle after 0s`.

Two things were wrong, and neither was timing.

**The harness's lobby tap was not a tap** -- it released at (0,0), 88 px from
the press, and the cartridge calls anything over about twelve a drag. See
addendum 3.

**And this document's table stopped three screens early.** Its last row is the
host's Continue. What actually follows:

| whose | what | where |
|---|---|---|
| client | **Ready** in the Game Lobby | `212,174` |
| both | this opens **minifig / army select** | |
| client | pick a minifig | `120,25` |
| client | **Ready** again | `212,174` |
| host | its button now reads **Start** | `212,174` |

The host readies itself when it presses Continue in the lobby, which is why its
button shows `Waiting...` throughout and no tap on the host does anything until
the client has readied twice. Sweeping six candidate coordinates on the host
with a capture after each showed every one inert -- which is what pointed at the
client being the side with work to do.

The whole sequence, as run:

```
--tap0 72,84 --tap0 72,84 --tap0 205,169 x4 --tap0 w:15 --tap0 w:15 --tap0 w:10
--tap0 w:8 x4 --tap0 212,174 --tap0 w:8 x5 --tap0 212,174 --tap0 b:120

--tap1 173,84 --tap1 173,84 --tap1 w:5 --tap1 l:90 --tap1 w:8 x4
--tap1 212,174 --tap1 w:8 x5 --tap1 120,25 --tap1 w:3 --tap1 212,174
--tap1 w:8 x3 --tap1 b:120
```

`l:90` is the lobby-list wait from addendum 3: watch until the list has a row,
then tap it once. It is what makes the join reliable rather than lucky.
