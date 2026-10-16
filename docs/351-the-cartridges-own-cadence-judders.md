# 351 — The cartridge's own cadence judders, and the port puts it there

Status: **decided and deployed.** Steady 30 Hz is the default (section 5).
First deployed 19:17 (`7875b377`) as a 33 ms threshold; that version stalled
heavy frames, and the mechanism was replaced by a VBlank clock (section 6),
deployed 20:23 in `build/LegoBattles.exe` sha256 `b6e68e13...`.

The defect (docs/347 item 4): the camera was still wonky, particularly in
motion, and felt as if it was not actually moving, or was being pulled back
while it moved.

docs/348 fixed the layers disagreeing about the camera, and docs/349 fixed the
port presenting frames in pairs. Both were real. This is the third cause, and
it is not in the port's presentation at all: **the game itself advances in
uneven steps** -- one screen refresh, then two, then one, then two -- and every
step moves the camera and the units by one simulation tick. Fast, slow, fast,
slow, eighty-four times in a hundred: "moving, then pulled back".

## 1. How the cartridge paces itself (decomp)

NitroMain (`020F6730`), disassembled from the ROM:

```
020F68B0  mov   r4, #0x14            20 (ms), set once before the loop
020F68D0  bl    OS_GetMilliseconds   t0
020F68F8  bl    OS_WaitVBlankIntr    wait for the next VBlank
020F6900  bl    TaskList_Run         the whole frame: input, sim, camera, 3D, BGs
020F6904  bl    OS_GetMilliseconds   t1
020F6910  subs  r2, r0, r8 / sbc r1, r1, sl / cmp r1, #0 / cmpeq r2, r4
020F6924  bhs   skip                 (t1 - t0) >= 20 ms: go round again
020F6928  bl    OS_WaitVBlankIntr    otherwise wait for one more VBlank
```

Everything happens in `TaskList_Run`, once per pass: there is no separate
logic and render rate. And the simulation advances one step per pass: task
0x0A (`020835F0`) processes the commands, and when every one of the four
player slots is ready -- a lockstep turn barrier, run even in single player --
calls the step, `020836A0`. So **game speed is the pass rate**.

With T the time `TaskList_Run` takes, the loop's own arithmetic gives three
regimes:

* T < 3.3 ms: the second wait always happens -- one pass per 2 VBlanks, an
  even **30 Hz**.
* 3.3 < T < 16.7 ms: a pass that starts on a VBlank finishes past 20 ms and
  skips the second wait; the next starts mid-frame, finishes under 20 ms and
  takes it -- **1, 2, 1, 2 VBlanks apart, 40 Hz on average and uneven**.
* T > 16.7 ms: an even **30 Hz** again (20 Hz where T passes 33 ms).

## 2. Where the port lands (measured)

`LB_LIVE3D_PROBE=1` stamps every SWAP_BUFFERS the geometry engine executes --
one per pass (task 0x25) -- with the frame and the scanline. Over the reference
replay (a recorded 2026-09-22 game) at an 842x474 surface:

| interval to the next pass | share |
|---|---:|
| 1 VBlank (16.7 ms) | 45.3% |
| 2 VBlanks (33.3 ms) | 48.2% |
| 3 VBlanks | 6.2% |
| 4+ | 0.3% |

37.1 passes a second, and **the interval changes from one pass to the next 84%
of the time** -- the commonest three-pass patterns are 2-1-2 (1,088) and 1-2-1
(1,065). That is the middle regime, and the swap scanlines say why: the task
list ends a median **13.7 ms** after the VBlank (p10 1.8, p90 16.0) -- just
under one frame. (Later in a long unattended game, with ~45 entities, T passes
16.7 ms: ~30 Hz, mostly 2 VBlanks, with 10-18% of passes taking 3 -- the 20 Hz
stalls of a heavy frame.)

## 3. Why the port, and probably not the DS

T is the EMULATED ARM9's time, and the port charges ARM9 cycles the way
melonDS does (`bus.cpp`, `runtime_mem_cycles` / `runtime_code_cycles`, each
commented "melonDS: ..."). In the melonDS 1.0 source, a code fetch from a
cached region costs a flat `kCodeCacheTiming` -- the real lookup is commented
out, `CodeCycles = kCodeCacheTiming;//ICacheLookup(addr);` (CP15.cpp:805) -- and a
cacheable data access a flat `kDataCacheTiming` (CP15.cpp). **No cache miss is
ever charged.** A real ARM946E-S refills a line from the 16-bit main-RAM bus on
every miss, and an RTS walking dozens of entity structs a frame misses often.

So the real DS almost certainly takes longer per pass than 13.7 ms -- past
16.7 ms is all it takes -- which puts it in the even 30 Hz regime, where this
judder does not exist. That is an inference, not a measurement: no hardware
here, and melonDS, the only reference, has the same model. What is measured is
that the port sits just under the one-frame line, where the cartridge's own
arithmetic produces the uneven cadence.

(It also means the port currently runs the game's simulation at ~37 steps a
second in these scenes where a 30 Hz DS would run 30: about a quarter faster.)

## 4. The option, and the decision

**Steady 30 Hz:** make the second wait's threshold two frames instead of 20
ms. Then a pass under one frame always waits for the second VBlank and a pass
over one frame never does: every pass 33.3 ms apart, every game frame on
screen for exactly two presents, the regime the DS is most likely in. The cost
is pace: in scenes like the reference replay the game would run 30 simulation
steps a second instead of ~37 -- slower than the port runs now, and plausibly
the DS's own.

It can be done the way the port already patches guest code (the gather rect,
docs/350's effect bounds). Not at `mov r4, #0x14` (020F68B0): that runs once,
before the loop. But the per-pass compare at 020F6920, stock `01520004`
(`cmpeq r2, r4`), can become `03520021` (`cmpeq r2, #0x21`; both encodings
checked with capstone against the ROM image), and the change is seen: the
loop's back-edge is generated as
`runtime_dispatch_literal_branch(0x020F68B4u); return;`
(`port/generated/lb_arm9_11.c`), so every pass re-enters through the
dispatcher, whose content validation then sends NitroMain's 33-instruction
loop (020F68B4..020F6934) to Tier 3 -- the calls out of it (TaskList_Run and the rest) stay lifted.

**Measured, not just argued.** `LB_CADENCE=30` does exactly this (measured
while it was still an opt-in; section 5 made it the default). The reference replay, same surface, stamped the same way:

| threshold | passes/s | 1 VBlank | 2 VBlanks | 3+ | interval changes |
|---|---:|---:|---:|---:|---:|
| 20 ms (cartridge) | 37.1 | 45.3% | 48.2% | 6.5% | 84.1% |
| 33 ms (`LB_CADENCE=30`) | 30.4 | 11.5% | 79.9% | 8.6% | 31.1% |

Better, not perfect, and a model of the loop says why. Simulating the loop
on the replay's own task-list times reproduces both rows (39.0 Hz / 52-42-6 /
79% and 30.4 Hz / 14-74-12 / 40%), and at 40 or 50 ms gives 28.0 Hz with 86%
two-VBlank and 14% three-VBlank intervals and no one-VBlank catch-ups at all.
What no threshold removes is the three: the loop's FIRST wait always costs a
whole frame, so a pass whose task list runs past 16.7 ms (6-9% of them here)
is always shown three VBlanks after the one before. A perfectly even 30 Hz
needs every pass under one frame as well -- the ARM9 doing its work faster
than the port now models, which is a scheduler change, not a patch.

**What is not proposed:** a steady 60 Hz. The simulation steps once per pass,
so 60 passes a second would run the game at double speed.

**The choice:** keep the cartridge's cadence as the port emulates it
(~37 Hz, uneven -- the judder), or run an even 30 Hz (smooth, and a slower game
than now). Either can also be a setting.

## 5. Decided: steady by default, a setting to go back

Steady 30 Hz was chosen as the default. What shipped:

* **The setting.** Display tab, "Steady game pacing (30 Hz)", on by default;
  `steady_cadence` in settings.json (a file without the key -- every existing
  one -- gets the default). Off restores the cartridge's 20 ms.
* **One policy,** `lb_cadence_effective` (lb_patches.cpp), read every VBlank
  by `apply_cadence`: `LB_CADENCE=20/30` for a measurement, else a PLAYING
  demo's own cadence, else the setting.
* **Recordings carry their pacing.** Inputs are indexed by VBlank but read
  once per pass, so a demo replayed at the other cadence would hand each input
  to a different simulation step -- or drop a one-VBlank tap between two
  passes -- and replay a game its player never played. New recordings write
  `lbdemo 1 cadence=30` (and a `<frame> c <20|30>` line if the setting changes
  mid-recording); a file without it predates the default and replays at 20.
  Every recording on disk therefore still reproduces.

Verified on the build that shipped:

| check | result |
|---|---|
| exactness gate, the 09-22 reference recording (no cadence field) | identical at all five frames; `[lb_cadence] 20 -> 33 ms` printed only after the replay ended |
| a live battle (harness route, no demo) | steady from startup: 30.1 passes/s, 91.6% two-VBlank, 4.3% one, 3.9% three; interval changes 12.4% |
| the recording that battle made | header `lbdemo 1 cadence=30`; replayed, the steady threshold is applied at once (log line 48 of 179) |

The live battle is a lighter scene than the reference replay, which is why it is
steadier than the 80% measured there with the same threshold (section 4).

## 6. The threshold was the wrong tool; a VBlank clock is the right one

Window-size tests of the 19:17 build (the reference replay at 1280x720,
1920x1080, 2560x1080 and 1080x1350, then the harness's post-replay phase,
which pans the camera and makes the game heavier) found 24-33% of passes
THREE VBlanks apart at every size. The loop's own arithmetic says why: after a
pass that overran its frame, the next pass starts mid-frame, its first wait is
shorter by exactly the overrun, and it measures ~33.4 ms whatever the task list
took -- right on a 33 ms threshold, where OS_GetMilliseconds' whole-millisecond
rounding sometimes adds a wait. The cartridge's own 20 ms handles that heavy
regime evenly and judders the light one; 33 ms does the reverse. No single
millisecond threshold serves both, because the elapsed time includes the first
wait.

The rule the loop is reaching for is "wait again only if no VBlank came
while the task list ran": passes two VBlanks apart for any task list under two
frames. It is exactly expressible without touching guest code.
`get_ms_override` (lb_patches.cpp) answers OS_GetMilliseconds (020F2648) at
NitroMain's two call sites only -- return addresses 020F68D4 and 020F6908 --
with the cartridge's own VBlank counter (the word at 02158A58, incremented by
the VBlank ISR 020F22E8) times 17. A pass whose task list ends inside its frame
measures 17 (< 20: the second wait); one that ran past a VBlank measures 34
or more (no second wait). The cartridge's 20 ms compare stays as shipped and
NitroMain stays lifted; the 24 other callers get the real clock, the
cartridge's own body run unchanged. The threshold patch is gone
(`apply_cadence` now only restores the stock word if an earlier build left
its own).

| | pacing | passes/s | 1 VBlank | 2 | 3+ | interval changes |
|---|---|---:|---:|---:|---:|---:|
| reference replay | cartridge (20 ms, real clock) | 37.1 | 45.3% | 48.2% | 6.5% | 84.1% |
| | 33 ms threshold (19:17) | 30.4 | 11.5% | 79.9% | 8.6% | 31.1% |
| | **VBlank clock (20:23)** | 29.9 | 5% | **90%** | 5% | 15% |
| heavy phase, 1920x1080 | 33 ms threshold | 26.6 | 17% | 50% | 33% | 68% |
| | **VBlank clock** | 29.7 | 16% | 67% | 18% | 48% |

What is left in the heavy phase is genuinely long passes -- task lists past
two frames, and the one-VBlank catch-up after a pass that ended late -- which
no pacing rule can remove; a faster emulated ARM9 would. The exactness gate is
identical on the new build: a recording without a cadence field replays on
the cartridge's own clock, and the override then runs the cartridge's body.

Window sizes, measured on the way (the replay, the cartridge's pacing):

| window | surface | effect test bounds | task list ends, median | host fps / present sd / underruns |
|---|---|---|---:|---|
| 1280x720 | 640x360 | x -384..640, y -168..360 | 13.4 ms | 59.97 / 0.52 ms / 0 |
| 1920x1080 | 960x540 | x -704..960, y -348..540 | 13.2 ms | 59.81 / 1.82 ms / 0 |
| 2560x1080 | 1280x540 | x -1024..1280, y -348..540 | 6.9 ms* | 59.78 / 2.54 ms / 10 |
| 1080x1350 | 540x674 | x -284..540, y -484..676 | 13.0 ms | 59.80 / 0.31 ms / 0 |

(*a median of ends after the VBlank: at the widest surface more passes overran
their frame -- more of the map is gathered and drawn -- so more ends land just
past the next VBlank. 3+ intervals 10% there against 6-8% elsewhere.)

**One open item from the same pictures.** At 1280x720 only (surface 640x360,
native field from (192,84)), composite frames 1279-1290 of the replay carry a
1-pixel-tall, 16-pixel line at surface (0..15, 35) -- dark blue, dark red,
then greens, changing slowly -- over unexplored shroud in the top-left band.
It is not in the 3D layer (LB_DUMP3D: no opaque pixel there), and engine A's
OBJ layer is off in the field (DISPCNT 0x2F08), so it is a BG layer of the
band: the port's band fill or guest BG content it carries. None of the other
three sizes shows anything like it in their dumped frames (their replays are
different battles by then). Not attributed yet: the port has no per-BG-layer
dump, and that is the probe the next pass needs.

**Resolved in docs/352; two statements above were wrong.** The port already
had a per-layer dump, `LB_LAYERDUMP`, and it attributes the line. It is not
content of the band: it is BG1, the terrain, showing through a 1-px row where
BG2, the fog, had been cut out. The cut-out was for a relocated HUD corner
element, and it outlived the element because `lb_hud_shift_line` left its
thread-local cover mask stale on its early returns. Fixed; the fog is
restored at exactly those pixels, and there was a second such row, on the
surface's last row.
