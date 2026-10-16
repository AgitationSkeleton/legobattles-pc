# 214 — Making the widened columns clickable

> **RETRACTED IN PART — see docs/218.** This document's central negative
> result, that a click on a unit outside the native 256 columns does not
> select it, is WRONG. It does, and the touch extension is what makes it
> work: with the player's base parked at screen x −61, 4 of 12 clicks across
> its sprite select it with the extension on and 0 of 12 with it off, and the
> top screen names the selection as "Eagle Command 1500/1500".
>
> Three things hid it, and each is worth reading here rather than only in
> docs/218: the harness could not click at all (docs/217); the entity
> position the port reports is not the clickable one, so even IN-band clicks
> on a building missed; and a CLAMPED click lands at native x 0, so a target
> whose hit area covers 0 is hit whether or not the extended coordinate does
> anything.
>
> Everything else below — the delivery chain, the addresses, the patch
> encodings — stands, and is what the working feature is built on.


User feedback:

> Make sure the expanded field of view for the touch screen is just as
> interactable as the original aspect, eg. tapping or commanding a unit,
> clicking-to-drag and select multiple units, etc.

This is a real gap and the port currently does the opposite deliberately. The
enhanced touch mapping ends:

    // The guest still believes its screen is 256 wide: extra
    // adaptive columns are extra VIEW, not extra touch, so the
    // touchable band is 256 columns centred in the guest width.
    if (gx < band || gx >= band + kScreenWidth) {
        nds_set_touch(0, 0, false);
        return;
    }

So a click in a widened column is **dropped**. Units are visible out there
(docs/210) and cannot be selected or ordered. Half a feature.

## Why the hardware path cannot carry it

    void nds_set_touch(uint16_t x, uint16_t y, bool down) {
        g_tsc_x = static_cast<uint16_t>(x << 4);

The touchscreen controller's field is 12 bits and the firmware's scale is 16x,
so the representable range is exactly the 256-px panel. A coordinate left of 0
or right of 255 cannot be expressed as an ADC reading at all. Passing 300 would
overflow the 12-bit field and wrap to a wrong, in-range value -- worse than
rejecting it.

**The extension therefore has to be applied above the hardware, in the game's
own touch state.**

## Where that state is, found by correlation

Not guessed: `--findtouch` injects known touches and looks for them.
`(200, 100)` and `(40, 150)` alternately, twice each, so a counter that happens
to pass through a value once cannot be mistaken for the real thing:

    Y @02153768 = 100/150/100/150      X @0215376A = 200/40/200/40
    Y @0215376C = 100/150/100/150      X @0215376E = 200/40/200/40

Two `(y, x)` pairs of `s16`, sixteen bytes into the same region as the view
pointer (0x02152034) and the entity pool (0x02155028).

## The finding that makes this feasible: the game reads them SIGNED

`func_020A8794`, which consumes them:

    020A87F8  ldr   r1, [pc, #0x30]   ; -> 0215376A
    020A87FA  movs  r0, #0
    020A87FC  ldrsh r2, [r1, r0]      ; touch X  -- SIGNED halfword
    020A87FE  ldr   r1, [pc, #0x30]   ; -> 0215376C
    020A8800  ldrsh r1, [r1, r0]      ; touch Y  -- SIGNED
    020A8806  str   r2, [sp]
    020A8808  bl    #0x20aa8e4

**`ldrsh`, not `ldrh`.** The game sign-extends its own touch coordinates, so a
negative X -- a click in the LEFT widened column -- is representable in the very
path that consumes it, and so is an X past 255. Nothing needs to be widened or
re-encoded; the value simply has to be written.

(The two `ldrh` at 020A880C..16 copy the pair into the consumer object at +0x20
and +0x22 unsigned, which is worth remembering: that copy would NOT carry a
negative correctly, so the extension may need to reach the consumer rather than
only this global. To be checked when implementing.)

## What is still open

**When to write it.** The port must place the extended coordinate after the
game samples the panel and before `func_020A8794` reads it. Writing from the
vblank hook is likely to lose the race -- the project already has
`external-poke-loses-the-race` recorded for the camera, where an injected value
was rewritten within a frame.

Two candidates, neither yet tested:

1. **Patch the literal pool.** The reader takes its address from the word at
   `020A8830` (`0215376A`). Repointing it at an address the port owns would make
   the game read the port's value by construction, with no timing to lose. The
   cost is finding a guest address that is genuinely free.
2. **Find and patch the writer.** The exact literals `0215376A` and `0215376C`
   appear in only one pool, this reader's, so the sampler forms the address some
   other way -- a struct base plus offset. It has not been located yet.

Also unresolved, and required by the goal:

* **Drag-select across the boundary.** A drag that starts in the native band and
  ends in a widened column has to carry both endpoints, so whatever fix lands
  has to hold for a stream of positions and not just a tap.
* The **touch area test** already accepts the whole field rect
  (`lb_overlay_mouse_in_touch_area`), so only the coordinate mapping rejects
  these clicks. That part needs no change.

---

## Tested: poking the touch state does not work -- CORRECTED, the first test was broken

`--pokemouse` holds a touch, writes an extended X, and reads it straight back --
because `external-poke-loses-the-race` says read it back before believing
anything:

    held (200,100): X=200 Y=100
    wrote   300 -> immediate 200, +50ms 200, +400ms 200
    wrote   -40 -> immediate   0, +50ms 200, +400ms 200

The value does not survive even to the **immediate** read-back. The game
re-derives its touch state faster than an external write can be observed, so the
"write it every vblank" option is dead -- exactly as predicted, and now measured
rather than assumed.

**That run measured nothing.** `write_mem` takes `value` and `width`; the probe
passed `hex=`, so `value` defaulted to **0** and every "poke" wrote a zero. The
`-40` case reading back as 0 was not the game zeroing it -- it was the probe.
This is `verify-the-capture-harness` exactly: a broken tool produced a confident
result, and the giveaway was there to be read (a write of `-40` cannot plausibly
land as `0`).

Re-run with `value=`/`width=16`:

    held (200,100): X=200 Y=100
    wrote   300 -> immediate   300, +50ms 200, +400ms 200
    wrote   -40 -> immediate   200, +50ms 200, +400ms 200

**The conclusion survives, on evidence this time.** A write does land -- 300 is
read straight back -- and the game overwrites it within 50 ms. So the value has
to be placed where the game will not immediately replace it, which still means a
code patch rather than a poke.

**So a code patch is required.** That narrows the options to the two already
listed above, and settles which problem to solve first: the port needs a guest
address the GAME NEVER WRITES to use as the scratch the repointed pool reads
from. Any address the game also writes loses the same race.

### Next step, precisely

1. Find a scratch: sample candidate addresses across a whole battle and keep one
   that never changes. The top of main RAM is the obvious place to look first,
   but it has to be demonstrated, not assumed -- the heap's extent is not known.
2. Repoint the pool word at `020A8830` from `0215376A` to that scratch.
3. Each vblank, the port writes `scratch = (game's own sampled X) + offset`,
   where offset is 0 unless the click was in a widened column. One frame of
   latency, which is invisible for a click and for a drag.
4. Check the unsigned `ldrh` copy at `020A880C..16` into the consumer object at
   `+0x20`/`+0x22` -- it would truncate a negative X, so it may need the same
   treatment.
5. Verify with a DRAG that starts in the native band and ends in a widened
   column, not just a tap.

---

## Built, and it does not work yet: the reader is the wrong one

The plumbing is in and the test is decisive. Both halves matter.

### What was built

* A scratch address the game never writes, **measured** rather than assumed:
  `--scratch` writes a magic to ten candidates and re-reads them after 45 s of
  play. `0x0215FFF8` was zero before, took the write, and still held it after.
  (The first run of this probe reported every candidate as unwritable; that run
  was invalid -- see the `write_mem` retraction above.)
* The literal at `020A8830` is repointed from `0215376A` to that scratch, so the
  game reads a value this port owns and no write can race it.
* The enhanced touch mapping no longer drops out-of-band clicks: it publishes
  the true screen x -- negative or past 255 -- while still handing the touch
  controller a value inside the panel so pen state stays vanilla.
* `LB_TOUCH_FORCE=<n>` publishes a fixed extended x while the pen is down, so
  the MECHANISM can be tested without depending on the frontend mapping.

### The test, and the three broken versions before it

    CONTROL: in-band sx=147   -> hudquads=6  SELECTED
    after deselect            -> hudquads=1  (cleared)
    out-of-band, forced -45   -> hudquads=1  NOT SELECTED

**So an extended X reaching `func_020A8794` does not select a unit.** That
function is not the consumer the world-selection path uses. The mechanism is
sound and pointed at the wrong reader.

Getting to a test that could say that took four attempts, and each failure was
the probe rather than the feature:

1. `frontend_input action=touch` did not select even INSIDE the native band --
   the first click landed on the Top Screen panel's corner and ImGui ate it.
2. Resized and aimed clear of the panel; still nothing, and the out-of-band
   click computed a window x of **-123** because the cover-crop puts the field
   rect at a negative origin.
3. Switched to raw taps with a forced coordinate. The control selected, but the
   **deselect did not**, so the final reading was a leftover selection. Five
   empty-ground taps do not deselect in this game; the test correctly refused to
   report a result it could not falsify.
4. The deselect button is only reachable with anchoring OFF -- this port has
   moved it out to the widened edge -- but with anchoring off the HUD scan
   returned early and the selection detector went silent. The scan now counts
   HUD quads whether or not it moves them, which broke that circle.

The selection detector itself is worth keeping: **the in-field HUD does not
exist until something is selected**, so `hudquads` IS the signal.

### Where to look next

The unsigned copy at `020A880C..16`, which writes the pair into the consumer
object at `+0x20`/`+0x22` with `ldrh`, is the obvious next suspect -- it would
truncate a negative X even if the value arrived correctly. Beyond that, the
world-selection path reads the touch from somewhere this document has not
found, and the fast-path problem means `LB_WATCH` cannot point at it: main-RAM
accesses skip `watch_push`, so only `bus_write_u16_slow` is instrumented.
Instrumenting the fast path, or tracing reads of `0215376A` specifically, is the
next tool to build.

The feature is behind `LB_TOUCHEXT=1` and **off by default** until the right
reader is found: it redirects a game read to port-owned memory for no benefit,
and that reader is used for something.

---

## Decisive: `func_020A8794` is not in the selection path at all

The previous section concluded "the reader is the wrong one" from a failure to
select out of band. That is a weak inference -- an out-of-band failure has many
possible causes. This is the strong version of the test.

**Force a WRONG coordinate where selection is known to work.** With
`LB_TOUCH_FORCE=250` publishing X=250 on every touch, and the hero standing at
screen x 147, tap him:

    CONTROL: in-band sx=147 -> hudquads=6  SELECTED

He is still selected. If this function fed the selection logic, publishing 250
while tapping 147 would have selected nothing, or something else. It changed
nothing, so **the unit-selection path never reads this value**.

Both consumers in that function were checked first, so this is not a case of
patching one of two: the `ldrsh` pair at 020A87F8..8800 and the `ldrh` copy at
020A880C..16 that writes the pair into the consumer object at `+0x20`/`+0x22`
load from the **same two pool words**, 020A882C and 020A8830. Repointing 8830
redirects both. And the copy is bit-preserving -- `ldrh` then `strh` keeps
0xFFD3 intact -- so a negative X would have survived into the object.

### What this costs and what it leaves

The plumbing is sound and stays, behind `LB_TOUCHEXT=1`, off by default: the
scratch is measured, the repoint lands, and the port's value demonstrably
becomes what that function reads. What is missing is the **right** function.

`LB_WATCH` cannot find it. Main-RAM accesses take a fast path that skips
`watch_push` entirely -- only `bus_write_u16_slow` is instrumented -- so the
existing watchpoint sees nothing for these addresses. That is the tool that has
to be built or the search reframed:

* instrument the fast path behind the same `LB_WATCH` gate, accepting the
  slowdown while the probe is on; or
* work from the other end -- find where the selection result is stored (a
  pointer to the selected entity, findable by the same correlation `--findtouch`
  used), then find what writes it, and walk back to the touch.

Either is a substantial piece of work. Recorded here so the next one starts from the strong
negative rather than re-deriving it.

---

## FOUND: the real touch path, and a one-instruction way in

The watchpoint does work on main RAM. The earlier "fast path skips `watch_push`"
conclusion was half right and led nowhere: `bus_read_u16` takes an inline fast
path **only when `g_runtime_deep_trace` is clear**, and it defaults SET, so the
slow path -- the instrumented one -- was available all along. The first probe
returned zero events for some other reason; asking for `deep_trace` explicitly
made it work.

    whowrites: 100 events, 4 writing PCs, 8 reading PCs

**`func_020A8794` does not appear among them.** Independent confirmation of the
strong negative, from a completely different direction.

### The structure

Base **0x02153760**, and the fields are not two pairs:

| offset | address | meaning |
|---|---|---|
| +0x08 | 02153768 | Y previous |
| +0x0A | 0215376A | **X current** |
| +0x0C | 0215376C | Y current |
| +0x0E | 0215376E | X previous |

### The writer, 0203DD14

    0203DD14  E1DD20B0  ldrh  r2, [sp]        ; the sampled X
    0203DD1C  E1C020BA  strh  r2, [r0, #0xa]  ; -> X current
    0203DD20  E1DD20B2  ldrh  r2, [sp, #2]
    0203DD24  E1C020BC  strh  r2, [r0, #0xc]  ; -> Y current

`r0` is already the struct base here, which is what makes a one-instruction
substitution possible.

### The readers -- tap and drag, and both truncate

Tap:

    0203DE4C  E1D210BA  ldrh  r1, [r2, #0xa]   ; X
    0203DE50  E1D220BC  ldrh  r2, [r2, #0xc]   ; Y
    0203DE58            bl    #0x200a350       ; build the event
    0203DE64            bl    #0x20099e4       ; dispatch

Drag:

    0203DE9C  E1D050BA  ldrh  r5, [r0, #0xa]   ; X current
    0203DEA0  E1D010BE  ldrh  r1, [r0, #0xe]   ; X previous
    0203DEA4  E1D040BC  ldrh  r4, [r0, #0xc]   ; Y current
    0203DEA8            sub   r0, r5, r1       ; dx
    0203DEB8  E1D100B8  ldrh  r0, [r1, #8]     ; Y previous
    0203DEC4            add   r0, r6, r0       ; |dx| + |dy|
    0203DEC8            cmp   r0, #3           ; a drag needs 3 px of movement
    0203DEE4            bl    #0x200a350       ; event type 2

So the DRAG that is needed is right here, and it is the same struct.

**Every read is `ldrh` -- unsigned.** A negative X, i.e. a click in the left
widened column, would read as 65491 and every subtraction after it would be
wrong. In ARM, `LDRH` and `LDRSH` differ by one nibble: `...B` vs `...F`.

### The plan, precise enough to execute

1. Port writes the extended X to the struct's **+0x10** (0x02153770), which
   nothing in the four-address scan ever touched -- to be verified the same way
   the scratch was.
2. Patch `0203DD14` from `ldrh r2, [sp]` to `ldrsh r2, [r0, #0x10]`
   (`E1DD20B0` -> `E1D021F0`). One instruction, same slot, and `r0` is already
   the base. The game's own writer then copies the port's value into +0xA, so
   there is no race: nothing but the port writes +0x10.
3. Flip the readers to `ldrsh` so the sign survives: `0203DE4C`, `0203DE9C`,
   `0203DEA0`, `0203DEFC` (B -> F in the low nibble).
4. Verify a tap AND a drag that crosses the boundary, with `hudquads` as the
   selection detector.

Not implemented yet -- this is four coordinated code patches and it should not
be rushed. The plan is exact and the encodings are
worked out.

---

## Implemented: the coordinate now reaches the game. Selection still does not.

All five patches are in and the delivery chain is proven end to end. The feature
is still not working, and the honest split is: **transport solved, consumption
not**.

### What is now true, measured

    [lb_touch] 0203DD14 writer sources X from the port
    [lb_touch] 0203DE4C tap X sign-extended
    [lb_touch] 0203DE9C drag X current sign-extended
    [lb_touch] 0203DEA0 drag X previous sign-extended
    [lb_touch] 0203DEFC X carry-forward sign-extended
    [lb_touch] published -45 -> slot=-45  struct +0A=-45 +0E=-45

A negative screen x -- a click in the LEFT widened column -- is delivered into
the cartridge's own touch struct with its sign intact. That was the part
docs/214 called impossible through the hardware, and it now works.

### Corrections the implementation forced

**The slot is +0x18, not +0x10.** The plan named +0x10 on the strength of a
`--scratch` run. That run was worthless: it sat still for 40 s, so the touch
code never executed, and it duly reported the touch X ITSELF as free. The
known-written fields are now permanent controls in that probe -- any run calling
them free is measuring nothing. Driving the screen during the hold shows +0x10,
+0x12 and +0x14 all change; **+0x18 does not**.

**"Fall back to the struct" is circular.** Once 0203DD14 reads the port's slot,
the game's sampled X never reaches +0x0A by itself, so reading +0x0A back as a
fallback re-publishes the last value forever and the coordinate freezes. The
first build did exactly that and every tap in the route landed on a stale x, so
the run never even reached a battle. `nds_set_touch` is now hooked to report the
native x from any source -- frontend mapping or debug command alike.

**The test aid broke the test.** `LB_TOUCH_FORCE` applied unconditionally sent
every menu tap to x = -45, so the route never reached gameplay and the run
reported "not selected" for a reason unrelated to the feature. It is now gated
on being in a battle with the pen down.

**A fixed force against a moving target.** One run forced -45 at a hero standing
at -61 and called the 16-px miss a failure. The aim band is now -49..-41 to
match.

### What remains

With the hero at -45, -45 delivered, and `+0A` reading -45, tapping still does
not select him. The value arrives and nothing acts on it. Remaining suspects, in
order:

1. **A bounds check downstream of the event.** `0203DE58` calls `0x0200A350` to
   build the event and `0x20099e4` to dispatch it; either could validate the
   coordinate against the 256-px screen and drop it. This is the most likely
   answer and the next thing to disassemble.
2. **The Y at the moment of the tap.** The diagnostic is rate-limited and its
   samples are not aligned to the test tap -- one showed `+0C=178`, which is the
   deselect tap's y, not the hero's 95. The log needs to fire ON the tap.
3. **A different path for world hit-testing** than the tap event.

The patches are behind `LB_TOUCHEXT=1` and **off by default**: the delivery is
proven but the benefit is not, and that is not a trade worth making in a normal
session for something that changes how the cartridge reads its own input.
