# 314 — Item 3: the right band takes orders; the left band is untested, not broken

Defect item 3 is that builders refuse to path/build across the extended space,
recorded OPEN and unverified. docs/238 left the reproduction ready:

> `lb_exp --fxband <x>` is the test: fix it and the band column above should
> walk toward its target the way the native column does.

Two substrate fixes have landed since it was written — the `kTouchPatches`
collision, which had refused all thirty touch sites, and the vertical order
path repoint at `0x0203DD20`. So the test is worth running.

## The right band works

```
fxband: ordered 023076A4 from (147,95) to (375,95)  commands 24 -> 26
fxband:   023076A4 is now at (381,79)  <- OUT OF THE NATIVE COLUMNS
fxband: ordered 02307460 from (123,63) to (375,63)  commands 51 -> 52
fxband:   02307460 is now at (381,47)  <- OUT OF THE NATIVE COLUMNS
```

Both the hero (`023076A4`) and the **builder** (`02307460`) accept an order to
a column outside the native 256 and walk there. docs/238's reproduction now
passes in the right band, which is the half of item 3 about *pathing* into the
extended space.

## The left band: a reproduction that was not one, twice

A left-band run at `x=-110` moved nothing — no unit budged and most orders did
not even create a command. Next to the right-band result that reads as a
left-band defect, and docs/238 even predicts one there:

> the re-read at `0x020A880E`/`0x020A8812` uses **`ldrh`, unsigned** ... the
> LEFT band is where it would show.

**Both halves of that reading are wrong.**

**The `ldrh` is not the bug.** Disassembled as Thumb — the region is Thumb and
an ARM disassembly of it is garbage — the pair is:

```
020A880E  ldrh  r1, [r0]
020A8812  ldrh  r0, [r0]
020A8814  strh  r0, [r5, #0x20]
020A8816  strh  r1, [r5, #0x22]
```

The value is immediately `strh`-truncated to sixteen bits, so `ldrh` and
`ldrsh` store **identical bits**. Patching it would change nothing. (The same
coordinates are read `ldrsh` eight instructions earlier, at `020A87FC`/
`020A8800`, for the call that follows — so the function uses both forms and
neither is wrong.)

**And the left-band trial was not a fair test.** The camera in that run took
x values 0, 88 and 136 on a 1536-px map, so a target at native `-110` is world
x `-110..26` — **off the left edge of the map for most of the run**. Refusing
to walk off the map is correct. The right-band trial it was being compared
against had its target at world x ≈ 592, comfortably inside. The two were not
the same experiment.

The probe now computes and prints the world position of every order and
flags an off-map one, so this cannot be misread again:

```
fxband: ordered ... to (-60,95)  ... world=(28,239)
fxband: ordered ... to (-60,63)  ... world=(76,207)
```

Re-run at `-60`, every target on-map — and the units still do not move. That
looks like the defect surviving the correction, and it is still **not** a
demonstration, for a third reason: world x 28 and 76 on a 1536-px map are
metatiles 0 and 2, inside the impassable rock border that rings the playable
area in every screenshot of this map. A refusal to path into rock is also
correct.

With this route's camera never exceeding x=136, the **entire** left band lies
within world x < 88 — all border. The left band cannot be tested from here at
all.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** orders into the right band are accepted and executed, by a
builder as well as a hero. Item 3's pathing half is not reproducible there.

**ESTABLISHED:** docs/238's `ldrh` candidate cannot be the cause of anything,
because the `strh` that follows makes the signed and unsigned reads identical.

**NOT ESTABLISHED:** anything about the left band. Every trial so far has had
its target either off the map or in the impassable border, and both refusals
are correct behaviour.

**NOT ADDRESSED:** the *building* half of item 3 — whether a builder can place
a structure in the extended space. That is item 4's placement path, parked in
docs/307 pending a way to enter placement mode.

## Next

A left-band test needs the camera far enough right that the band covers
passable ground — camera x above roughly 250 on this map. `aim_unit` pans by
holding a d-pad key and the camera here returns to its own resting position, so
the probe needs to park the camera deliberately rather than park a unit.

## The left band works too, once the test is fair

The fix was to park the **camera**, not a unit. A left-band target is
`world x = camera + (a negative offset)`, so the camera decides whether the
band covers real ground at all. The probe now pushes it right until the target
would land at world x >= 200 — about metatile 8, clear of the border — and says
so, including when it fails to get there:

```
fxband: camera parked at x=279 (want >=260 so the target lands at world x>=200,
        clear of the border)
fxband: ordered 02307604 from (147,95) to (-60,95)  world=(316,239)
fxband:   02307604 is now at (-141,95)  <- OUT OF THE NATIVE COLUMNS
```

**The unit accepted the order and walked out into the left band**, from native
x 147 to -141. That is the same behaviour the right band already showed, and it
is the opposite of the three earlier left-band runs where nothing moved.

Checking arrival in world terms rather than native, since native x is
camera-relative and the camera keeps moving:

```
target world x = 316
hero 02307604 final world = (257,256)   dx = -59
```

It started at world 426 and travelled to 257 — past the target by about 59 px,
two and a half metatiles. Whether that is overshoot, pathing around the
terrain, or simply a unit still walking when the probe sampled six seconds
later cannot be separated here, and is not claimed either way.

## Item 3, pathing half: not reproducible

**Both bands accept orders and both execute them**, for a builder as well as a
hero. Every earlier left-band failure was the test, not the game: two had
targets off the map, one had them inside the impassable border.

Three wrong readings in a row on the same question, each looking like a clean
reproduction, is the thing to carry forward from this page. The probe now
prints the world position and the camera it was taken with, so the next reader
does not have to reconstruct any of it.

**Still not addressed:** the *building* half of item 3.
