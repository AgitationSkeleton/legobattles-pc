# 289 — RETRACTED: the "monotonic fps decline" was a wide noise band

Commit `aba1a8fe` recorded this, flagged as not attributed:

> fps across three identical replays of the same demo ran 50.5, 43.7, 28.3, with
> durations 297, 338, 402 s -- monotonic. ... the trend is real and should not be
> read as noise without a controlled measurement.

The controlled measurement was made. **It is noise**, and the three samples were
a run of three draws from a distribution wide enough to produce them often.

## What the control was

Same demo, same window, every diagnostic environment variable cleared —
`LB_HUDLOG`, `LB_VERBOSE`, `LB_ORDERLOG`, `LB_ORDERFIX`, `LB_LAYERDUMP`:

```
fpsctl   37.47 fps   emu 23.278   present 3.279   adaptive 1.919
```

That is between the 28.3 and the 43.7, with the instrumentation **off**. So the
instrumentation was never the variable.

## The phase split said "not host load", and that was right but incomplete

Against the morning's like-for-like run (same demo, 298 s, 55.5 fps):

```
                morning     control
emu_ticks       14.719  ->  23.278   +58%
present_ticks    3.213  ->   3.279   flat
adaptive_ticks   1.167  ->   1.919
```

`present_ticks` flat while `emu_ticks` rises is the shape that rules out general
host load — the same discriminator used earlier that day to clear the camera
patch, where emu, present AND adaptive all rose ~2.2x together.

That reasoning is sound and it is why this was worth checking. What it does not
license is "therefore something I changed made emulation slower", which is where
this nearly went.

## The distribution, which is what settles it

`emu_ticks` from every run with an `fps.txt` today, newest first:

```
fpsctl 23.278   zcum 29.955   spandecl 19.964   hudreplay 17.241
brickanim4 17.585   brickanim3 19.981   brickanim 19.572   fogpatch 18.980
phantom1 18.139   soundheap 18.163   rowtable 15.843   bandsel-after 16.286
wide1 22.743
```

**Range 15.8 to 30.0 ms, and not ordered in time.** `brickanim4` at 17.585 came
after `brickanim3` at 19.981; `hudreplay` at 17.241 came after both. The three
"declining" replays were 17.241, 19.964, 29.955 — three rising draws from a band
that also produced 15.843 and 16.286 on either side of them.

## And the specific suspect is cleared by name

The obvious candidate was the touch mask fix (`963c54e0`), which made all
**thirty** `kTouchPatches` sites apply where they had all been refused — thirty
newly patched instructions in the input path, which runs every frame.

`rowtable` ran at **emu 15.843** with that fix already in, next to the morning's
14.719. So the touch patches applying cost nothing measurable, and the
hypothesis dies on evidence that was already on disk.

## The rule this cost

Three samples in one direction is not a trend when the spread is this wide. The
previous commit said the trend "is real" and asked for a controlled measurement
before calling it noise — the right instinct, and the wording was still too
strong for three points. `memory/lb-fps-harness-variance.md` already carried
"29-38 fps with no change; never A/B from one run"; this adds the phase-level
figure, `emu_ticks` 15.8-30.0 on identical code, which is the number to compare
against before any future performance claim.

**No regression is established, and none of today's commits is implicated.**
