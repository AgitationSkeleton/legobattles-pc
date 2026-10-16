# 238 — Commanding a unit into the widened columns does not work

**Goal within the project:** the expanded field of view for the touch screen
must be just as interactable as the original aspect: tapping or **commanding** a
unit, clicking-to-drag and selecting multiple units.

Selection out in the band is proven 4 of 4 in all three positions at the raised
ceiling. **Commanding had never been measured** — every band probe scored on the
selection count at `0x0215802C`, which answers "did this click select something"
and is blind to "did this click order something", and `--explore`, the only
probe that issues move orders, sends units to native x 250.

## The measurement

Both halves through the same path: `frontend_input` on a surface the probe
verified as **786 wide with 265 px bands**, target classification checked
against that surface, and the commanded entity's pointer printed so identity is
provable rather than assumed.

```
TARGET x=200 (native)                 TARGET x=400 (IN THE BAND)
entity 02307450                       entity 0230741C
  (147,95)                              (147,95)
  (189,80)                              (147,80)
  (189,80)                              (147,80)
  (189,80)                              (147,80)
  (189,80)                              (147,80)
  (171,64)                              (173,96)
  -> walks to ~189 and holds            -> never leaves 147
```

## THIS DOCUMENT WAS WRONG ONCE, AND THE FIRST VERSION IS WHY

An earlier revision claimed exactly this conclusion on **worthless evidence**,
and was retracted in full before being restored here. That history is kept
deliberately.

The first probe ordered with `lb_exp.tap`, which issues a **raw `touch`** — the
DS touchscreen path, which cannot carry a coordinate outside the native screen
and wraps it. Ordering to 400 put the unit at `400 & 0xFF = 144`, which happened
to be exactly where it already stood, so it looked frozen; ordering to 268 sent
it left to `268 & 0xFF = 12`. It had a control, a failure, and a bisected
boundary, and every number in it was arithmetic performed by the probe.

Three further faults had to be fixed before the table above could be trusted:

* **`--bandsize` only resizes inside the band trials**, so a run without
  `--bandselect` stayed at `guest=512x192` — band 128 px, valid x `-128..383` —
  and a target of 400 was **off the surface entirely**.
* **The probe took the newest `[lb_builder]` line**, which since the port logs
  every owner is frequently the *opponent's* builder, off camera.
* **`[lb_hero]` carried no `ptr=`**, so identity printed as `?` and "the unit
  did not move" could still have meant "a different unit was named".

All four share one shape: **the probe could not distinguish "the game declined"
from "my request never arrived as specified."** Each check now lives in the tool
— it prints the live surface width, the band width, whether the target is in the
band or native, and which entity it commanded.

The lesson worth carrying: the false version was *reproducible*. Reproducibility
confirmed the probe's arithmetic, not the claim. What exposed it was a
**different probe reaching the same screen region by a different route** and
disagreeing.

## What actually happens: the destination wraps modulo 256

A threshold bisect settled it, and overturned my own earlier conclusion that no
order was issued.

| target x | `x - 256` | where the unit went |
|---|---|---|
| 200 | — (native) | 189..218 — correct |
| 250 | — (native) | 238..242 — correct |
| **260** | **4** | **3, 7** |
| **300** | **44** | **55** |
| **400** | **144** | 147 |

**A move order IS issued for a band click, and its destination x is reduced
modulo 256**, dropping the unit somewhere in the native columns instead of where
it was sent. For x in 256..511 a byte mask and a subtraction of 256 are the same
arithmetic, so this is a truncation of the screen x to eight bits.

### Two ways I got this wrong first, both instructive

**x=400 is the worst possible test coordinate.** `400 - 256 = 144`, and the unit
happened to be standing at 147. Its wrapped destination was three pixels from
where it already was, so a *correctly issued, wrongly wrapped* order looked
exactly like no order at all. Almost every run on this row used 400.

**The x=300 run that seemed to disprove wrapping had no orders in it.** It
predated the command counter. Re-run with counting, three of its passes show
`order +0` -- nothing was submitted on them -- and the one pass showing `order
+2` moved the unit to 55, right where wrapping predicts. The apparent
counter-example was an absence of data, not evidence of absence.

Both errors have the same root: **reading a null result without first
establishing that the stimulus was delivered.** The command counter is what
makes that visible, and it should be on for every ordering measurement from here.

## LOCATED: the cartridge's byte fields hold the wrapped value

Read directly, on the pen edges of a real order to x=260:

```
up    press byte=(147,95) cur byte=(147,95)  port x16=147 press16=147  published=147
DOWN  press byte=(147,95) cur byte=(147,95)  port x16=260 press16=260  published=260
up    press byte=(  4,95) cur byte=(  4,95)  port x16=260 press16=260  published=260
```

The port's 16-bit shadows hold **260** throughout -- delivery is correct, as
selection already implied. The cartridge's own **byte** fields hold **4**, which
is `260 & 0xFF`. The game truncates the published coordinate when it fills its
own `+6`/`+7` (press) and `+8`/`+9` (current), because those fields are one byte
wide and always were.

So the defect is not a mangled coordinate; it is a **reader still looking at the
byte fields**. docs/229-234 redirected the readers that mattered for selection
to the 16-bit shadows at `+0x0E`/`+0x10`; a scan finds **thirteen `ldrb [r5,#6]`
and `ldrb [r5,#8]` sites still on the byte path**, spread across four functions:

| function | role | unpatched byte reads |
|---|---|---|
| `func_020AF040` | gesture recogniser (tap vs drag) | `0x020AF070`, `0x020AF0AC`, `0x020AF0D0`, `0x020AF0D2` |
| `func_020AF15C` | camera-delta handler | `0x020AF1C0`, `0x020AF1D0`, `0x020AF1DA` |
| `func_020AF23C` | tap handler | `0x020AF224`, `0x020AF22E`, `0x020AF32E` |
| `func_020AF340` | move handler | `0x020AF3AA`, `0x020AF3AE`, `0x020AF3B8` |

Two further sites, `0x020AF2DC` and `0x020AF39E`, are `ldrb +8` / `strb +6`
**copy pairs** rather than reads. Redirecting only their load would still store
through a byte, and redirecting both halves would stop maintaining `+6` for the
recogniser's own threshold -- so they are excluded from the first attempt rather
than changed on a guess.

**Thirteen is the same count as the classifier patches that once broke deselect
outright**, so the group is going in behind `LB_ORDERFIX` as a bitmask, one
function at a time, with `--fxband 260` as the pass/fail (~4 broken, ~260 fixed)
and selection's 4-of-4 as the regression guard.

## FIXED — units now walk out of the native columns

The left band settles it, because no competing explanation survives it. Ordered
to **x = -150**, with `LB_ORDERFIX=15`:

```
ordered 0230741C from (147,95) to x=-150   ->  now at (-33, 91)   OUT OF THE NATIVE COLUMNS
ordered 022DDB98 from (123,63) to x=-150   ->  now at (-12,103)   OUT OF THE NATIVE COLUMNS
ordered 022DDB98 from  (42,74) to x=-150   ->  now at (-10,103)   OUT OF THE NATIVE COLUMNS
ordered 022DDB98 from   (0,96) to x=-150   ->  now at (-20,109)   OUT OF THE NATIVE COLUMNS
ordered 0230741C from  (45,79) to x=-150   ->  now at (-50, 87)   OUT OF THE NATIVE COLUMNS
```

**A negative screen x cannot be produced by a byte truncation**, which can only
land in 0..255, **nor by any clamp toward the native width**. Repeated across two
entities and many orders, this is the destination carrying a full signed 16-bit
value. Commanding into the widened columns works.

### The "second limit near 245" was terrain, not code

Right-band orders topped out at 244..247 and that looked like a residual clamp.
It is not: a **native** order to x=250 also stops at 238..242, so ~245 is simply
as far as the unit gets in that direction on this map. The left band, where the
ground is open, shows the destination honoured well past the boundary.

That is worth stating plainly because it nearly became a third false lead: the
right band could not distinguish "the coordinate is capped" from "the unit ran
out of ground", and both readings predicted the same numbers. Choosing a
direction where the two predictions differ is what settled it -- the same
technique that broke the wrapped-vs-dropped question with x=260.

## PARTIAL FIX: the wrap is gone, a second limit remains

`LB_ORDERFIX=15` moves all thirteen remaining byte-path reads to the 16-bit
shadows, and the port confirms every site matched its stock encoding first:

```
[lb_orderfix] groups=15, 13 site(s) moved off the byte path
```

The destination changes, measurably:

| target | before (byte path) | after (16-bit shadows) |
|---|---|---|
| 260 | 3..7 | **160** |
| 400 | 144 | **247** |

**The wrap is gone.** 144 is exactly `400 & 0xFF`; 247 is not reachable by any
byte truncation of 400, because a truncated byte can only ever land in 0..255
and 247 requires the order to have carried more than eight bits at least part of
the way.

**But it is not the whole fix.** 247 sits just inside the native 256, so
something downstream still caps the destination near the native width. The port
already carries a patch described as *"event x no longer clamped to 4..251"*
(`0x0200A3C4`), and **247 is close enough to 251 to be worth checking first** --
either that clamp is not covering this path, or there is a sibling of it.

Status: **the byte truncation identified above is real and fixed by these
thirteen redirects; a second, separate limit near the native width is now the
open item.** The two were indistinguishable before, because a wrapped value and
a clamped value both land inside the native columns.

## Where the truncation is NOT — both known x sources carry 16 bits

The obvious fix target turned out to be wrong, so it is recorded rather than
left for the next person to re-derive.

**The touch slot is 16-bit.** The writer patched at `0x0203DD14` reads the
port-owned slot and stores with **`strh r2, [r0, #0xa]`** -- a halfword store --
so `0x0215376A` holds the extended x, and `func_020A8794` reads it back with
`ldrsh`. That path is clean.

**The recogniser copy is 16-bit and demonstrably works.** The tap handler's
conversion reads `+0x0E` (patched at `0x020AF27C`), which is the same field the
*pick* reads at `0x020AF29C` -- and selection in the band scores 4 of 4. If
`+0x0E` were truncated, selection would fail too.

**The arithmetic says the wrap is upstream of the metatile conversion.** With
`cam_x = 136` (cam_tx 17), `func_02039F5C` maps x=260 to metatile **16**, which
is screen ~248. A byte-truncated x=4 maps to metatile **5**, which is screen
~3..7. The unit went to **3..7**. So the value entering that conversion was
already 4, not 260 -- and its own `and #0xff` pair operates on metatiles
(0..63 on this map) and is not the culprit.

So the truncation happens in a **third** place, before the conversion and off
both paths already checked. One candidate worth testing first: the patched sites
assume `r5` is the recogniser object, and if the order path enters
`func_020AF23C` with a different object then `+0x0E` there is not the port's
16-bit copy at all.

**The next step is empirical, not static**: `LB_TOUCH_FORCE` already exists to
publish a fixed extended X while the pen is down, which isolates coordinate
delivery from the click mapping, and `--fxband 260` is a sharp pass/fail (~4
broken, ~260 fixed).

## Superseded: an earlier guess at where the truncation must be

The cartridge keeps the touch position in **byte** fields on the recogniser
object -- `+8`/`+9` current, `+6`/`+7` press -- and this port publishes 16-bit
copies at `+0x0E` and `+0x10` (the object was enlarged from 0x10 to 0x18 bytes
for exactly that, `0x020A87A8`). docs/229-234 redirected the reads that mattered
for **selection**; a scan of the region finds **eleven `ldrb [r5,#8]` sites still
unpatched**, several inside the tap handler `func_020AF23C`.

One of them is `0x020AF2DC`:

```asm
0x020AF2DC  ldrb r0, [r5, #8]     ; the truncated current x
0x020AF2DE  strb r0, [r5, #6]     ; into the truncated press x
```

so the byte fields are refreshed from each other while the port's 16-bit copies
are not, and any consumer reading `+6` or `+8` gets a wrapped coordinate.

**The test is sharp**, which is what makes this tractable: with `--fxband 260`,
broken sends the unit to ~4 and fixed sends it to ~260.

## The order path, as far as it is traced

* `func_020A889C` is a **handler registration table**: it calls
  `func_020AA70C(event_id, handler, flags)` for a long list of ids.
* **Event id `0x0B` is registered to `func_020A8F28`**, and that function is the
  move issuer: it copies the destination point (`func_020AA814`), builds a
  one-entry entity array (`func_020EC5A8`/`func_020E2A04`), fetches the command
  sink from `func_020A186C()->[0x64]`, calls the `MoveEntitiesCommand` factory
  `func_02005884`, and submits with **`func_020EB910`** — the same submit
  `docs/137` read and verified.

So the open question is now narrow and specific: **is event `0x0B` posted for a
click in the widened band?** Everything downstream of it is understood.

### And event 0x0B is reached through a state machine, not posted directly

`func_020AA130` (0x5D8 bytes) is a table of `func_020AA034(a, b, c, d, ...)`
calls with small integer arguments -- state ids -- and event `0x0B` appears in it
three times as a trigger, alongside ids like `0x47`, `0x49`, `0x4A`, `0x4F`,
`0x65`, `0x67`. So the click-to-order flow is a **state machine**: input drives
transitions, and one transition raises `0x0B`, which the handler table binds to
the move issuer.

That is why no single "x fits in a byte" gate has turned up for orders the way
one did for the field-click branch (`0x020A87DC`, already patched). The next
measurement should therefore be **runtime, not static**: establish whether
`func_020A8F28` executes at all for a band click, which separates "the state
machine never transitions" from "it transitions and the order is rejected
later". Everything downstream of `0x0B` is already understood.

## Two attempts at the MECHANISM, both inconclusive — recorded so they are not repeated

The symptom is settled. **Where the order is dropped is not**, and two
instruments have failed at it. Both failures are worth keeping, because both
looked like they were about to work.

### A dispatch override on the move issuer never fired

`func_020A8F28` **is** in the generated dispatch table --
`{0x020A8F28u, 1u, lb_arm9_tfunc_020A8F28, &g_validation_lb_arm9__5593}` -- and
`nds_register_dispatch` appends while lookup takes the first match, with this
port's overrides registered before the banks. So an observer on it should win.

It never fired, **not even for a native order that demonstrably worked** (the
unit walked 147 -> 189 in the same run). The likely reason is structural: event
`0x0B`'s handler is registered by address at startup through
`func_020AA70C`, so the state machine holds a pointer captured before overrides
are consulted. **An override is the wrong tool for a function reached through a
registration table.**

Running the NATIVE case first is what caught this. A silent band run would have
been read as "no order issued" and published as the mechanism.

### The global command counter is too noisy to answer it

`func_020EB910` bumps a counter at **`0x021585AC`** after appending a command to
its player's list, so reading it around the two clicks needs no override at all.
It works -- and it cannot resolve this question:

```
native x=200   order deltas  +0 +0 +1 +1 +2   total 4
band   x=400   order deltas  +0 +1 +1 +0 +1   total 3
```

The counter is **global and the AI submits commands too**: it climbs 24 -> 40 ->
55 -> 69 -> 89 between passes on its own, about 15-20 per 25 s. A click window of
roughly a second therefore expects 0.5-0.8 commands from background alone, so
every `+1` above is consistent with noise and the two columns are
indistinguishable. **This measurement supports neither conclusion.**

What would work, in rough order of cost: count only entries whose command tag is
`0x17` by walking the player's list at `0x021585CC + (slot << 4)`; or find the
unit's own destination field and watch that instead of the command stream; or
run a null control with no clicks at all to establish the background rate and
compare three conditions rather than two.

### A structural limit worth knowing: overrides cannot see inside lifted code

This is why the override failed, and it generalises well beyond this bug.

The cartridge's code is **lifted to C ahead of time**. Within lifted code a `bl`
to a known function becomes a **direct C call**, so it never consults the
dispatch table at all. A dispatch override therefore only fires where the guest
reaches the function *indirectly* -- a virtual call, a function pointer, a
resolved handle. `Entity::SetOwner` has an override that demonstrably fires
because it is reached that way; `func_020A8F28` is reached from a handler table
populated at startup, and it did not.

That reframes `docs/143`'s "zero dispatching call sites" for gameplay functions:
it is not that those addresses are cold, it is that **lifted code calls them
directly**. So for observing an internal call path in this port the usable
instruments are memory watches, memory polling and behavioural measurement --
not overrides.

### The coordinate does reach the order path, so that hypothesis is dead too

The field-click handler `func_020A8794` reads the click from fixed addresses:

```asm
0x020A87FC  ldrsh r2, [0x0215376C]     ; y
0x020A8800  ldrsh r1, [0x0215376A]     ; x   -- SIGNED 16-bit
0x020A8808  bl    func_020AA8E4        ; the rectangle builder
0x020A880E  ldrh  r1, [0x0215376C]     ; re-read, UNSIGNED
0x020A8812  ldrh  r0, [0x0215376A]
0x020A8814  strh  r0, [r5, #0x20]      ; into the event object
0x020A881C  bl    func_020AA0A4        ; dispatch the event
```

`0x0215376A` is `kTouchBase + 0x0A`, and that is **exactly the slot the port
already feeds**: the writer patch at `0x0203DD14` repoints the game's own load at
the port-owned slot at `+0x18`, so the game's writer copies the extended X into
`+0x0A` itself. It is then read `ldrsh` -- signed, 16-bit. **The order path
receives the extended coordinate.** The attractive theory that selection works
because it uses the patched recogniser path while ordering reads an unpatched
one is therefore wrong.

Noted for later: the re-read at `0x020A880E`/`0x020A8812` uses **`ldrh`,
unsigned**. That is harmless for a positive right-band x, and it round-trips
correctly through the 16-bit `strh` if every consumer reads it back signed -- but
it is the same shape as the defects docs/229-234 fixed, and the LEFT band is
where it would show.

## What is read from the ROM

`MoveEntitiesCommand` is command tag **0x17**, vtable `0x0213B2A0`, typeinfo
`0x0213B28C`, constructor `func_020067A4`, and it carries its **destination
point at +0x2C** (the handle vector goes to +0x14, as `WaitingToDieCommand`'s
does). Its factory `func_02005884` has 18 callers, which is why this was
bisected behaviourally rather than traced.

`func_02039F5C(out, view, x, y)` is the screen→world conversion the tap handler
uses: it adds the camera, divides to a tile and then by three to a metatile, and
masks each with `and rN, #0xff`. **That mask is not a truncation bug** — this
map is 192 tiles, so metatile coordinates run 0..63 and fit a byte with room to
spare.

## Status

**Open, with a reproduction.** `lb_exp --fxband <x>` is the test: fix it and the
band column above should walk toward its target the way the native column does.
