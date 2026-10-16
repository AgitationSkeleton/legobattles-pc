# 221 — The demo recorder works, and how you use it

**Goal within the project:** a demo recorder that captures a person's own
inputs -- selecting units, commanding them to build -- so a manual session can
be replayed by the harness instead of the harness trying to synthesise those
inputs itself.

"Ready" has to mean *proven*, not *written*. A recorder that has never
round-tripped is a measurement tool of unknown state, and an unverified
measurement tool is a prerequisite to fix, not something to trust. So it was
tested end to end, and it failed, and this
records the failure and the fix.

## How to use it

**To record.** Open the overlay, go to the **Demo** tab, type a filename, press
**Record**, then play. Press **Stop** when you are done — or just close the
window, which is the point below. From the command line the same thing is
`LB_DEMO_REC=path`.

**To replay.** Same tab, same filename, press **Play**. Or `LB_DEMO_PLAY=path`,
which is how a replay can be driven with no UI interaction at the front of it.

**What a demo needs to reproduce.** The same save and the same starting point. A
recording started at the title screen replays the whole session from boot; one
started mid-match needs that match reached first.

## Failure 1: the file only existed if you shut down tidily

The recorder buffered every event and wrote the file in `lb_demo_stop`. A player
finishing a recording will close the window, and a session that ends that way
never reaches a tidy shutdown — so the file would have been empty exactly when
it mattered. Losing a player's recording to a missing `fclose` is not
acceptable.

Now the file is opened on Record and each event is written and flushed as it
happens. A kill costs at most the event in flight, and what is left on disk is a
valid demo because the format has no trailer.

## Failure 2: every replayed tap went to the far-left column

The round-trip test: record the menu route to a battle, then replay it with
nothing else driving the game. It should reach the same battle.

It reached the **name-entry keyboard** instead. The periodic captures show why —
at ten seconds the replay is sitting on "Choose Profile" having selected nothing,
and it then confirmed an empty slot.

The cause is a sentinel collision. `lb_patches` uses `INT32_MIN` for "this port
has nothing to say about the touch x, hand the game the native one":

```cpp
if (x == INT32_MIN) x = g_native_touch_x;
```

The recorder stored "no out-of-band coordinate" as **0**, on the reasoning that
an unset coordinate may as well be zero. But 0 is a perfectly legal out-of-band
x, so the replay called `lb_touch_set_extended(0)` for every event and published
x = 0 to the touch slot for every tap in the demo. Every tap in the demo landed
in the far-left column of the screen.

Fixed by carrying the real sentinel through the file, and by having
`lb_touch_clear_extended` tell the recorder about the clear — without that, a
demo keeps publishing the last out-of-band x for every later tap, including taps
the player made inside the native columns.

## Measured, after

```
[lb_demo] playing demos/route-free.lbdemo, 22 event(s), last at frame 5014
[lb_demo] finished at frame 5014
```

with `--no-route`, and the run reaches the battle: `[lb_widemap] on:` appears and
the final capture is the field. The demo in `demos/route-free.lbdemo` is the
menu route from boot to a Free Play battle and is committed, so it doubles as a
regression fixture — any change that stops it reaching the field broke the input
path.

## Why the recording is taken where it is

At the guest boundary — the `nds_set_touch` / `nds_set_key_mask` calls — and
indexed by vblank rather than wall clock.

Window pixels would tie a demo to the window size it was made at, and SDL events
would tie it to the layout, the HUD anchor and the ImGui panel positions, all of
which this project changes weekly. "The console received this on frame N" stays
true however the port draws, at any window size, at any host frame rate. That is
also what makes a replay a fair test of a change: if two builds replaying one
demo draw different pictures, the change did it.
