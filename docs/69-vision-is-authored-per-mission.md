# Vision is authored per mission

**Status: `docs/63`'s open question is answered. The vision radius is not a unit
property at all — it is a literal passed by mission code, at 53 call sites
across the overlays, and nothing in the ARM9 calls the setter.** Two new checks;
**91 of 91 pass**. `docs/65`'s open list is down to **one**.

`docs/63` looked for a blueprint field and found none:

> A search for any function storing a blueprint byte into `+0x10` of the source
> record found nothing, so the radius is not copied straight from
> `UnitBlueprint` the way `resource_yield` is. Flagged, not guessed — and it is
> the obvious next step for anyone continuing this thread.

It also said where to restart: **from the source constructor rather than from
field writes.** That is what worked.

## The source is a `User::FogCircle`

`func_020A182C` is the fog map's add:

```c
if (!ListContains(map, source))   ListPushBack(map, source);
map->dirty_25 = 1;
```

Sources are **passed in, not allocated here** — and every caller lives in the
`Battles::User::FogCircle` translation unit. `docs/63` called `FogCircle` "the
rendering of fog, not the simulation of vision" and moved on. **That was wrong.**
`FogCircle` *is* the vision source: the object carrying the tile, the radius and
the footprint flags that `FogMap_Update` walks. Corrected in `docs/63` in place.

## The radius is an argument

`func_02048A94` iterates a list of fog circles and, for each:

```asm
ldr  r1, [r4]           the source record
ldr  r0, [sp, #4]       the function's SECOND ARGUMENT
strb r0, [r1, #0x10]    source->radius = arg
```

The radius is not read from anywhere. It is handed in.

The same function also settles a detail `docs/63` described without explaining:

```asm
ldrb r1, [r7, #4]  ; ldrb r2, [r0, #0xc]   new x against current x
cmp  r2, r1  ;  bne dirty
ldrb r2, [r0, #0xd] ; ldrb r1, [r7, #5]    then y
cmp  r2, r1  ;  beq clean
dirty:  r1 = 1
clean:  r1 = 0
strb r1, [r0, #0x11]                        mark_explored = tile changed
```

**`+0x11` is set exactly when the tile changed.** `docs/63` observed the flag was
one-shot; this is *why* a stationary unit does not re-stamp the explored map —
the flag is only ever raised by movement.

## Nothing in the ARM9 calls it

`func_02048A94` has **no callers, no vtable slot, and no word reference anywhere
in the ARM9 image.** By the standards of the rest of this project that reads as
dead code.

It is not. The overlays are missions (`docs/06`), they all load at `0x0216A2E0`
(`docs/39`), and a Thumb `bl` reaches back into the ARM9 comfortably. Disassembling
all 92 overlay images finds **53 call sites**:

```
ov000 x1   ov005 x3   ov035 x5   ...
```

Every one of the 53 loads `r1` with an **immediate** before the call:

| radius | call sites |
| ---: | ---: |
| 1 | 1 |
| 2 | **20** |
| 3 | 1 |
| 4 | 8 |
| 5 | **16** |
| 6 | 4 |
| 8 | 2 |
| 15 | 1 |

**Vision in LEGO Battles is authored per mission, not per unit type.** A mission
script sets a radius on a group of fog circles — 2 tiles most often, 5 next, and
one outlier at 15 that is presumably a scripted reveal.

That is unusual for an RTS, and it explains the negative `docs/63` recorded:
there is no sight field in `UnitBlueprint` because sight is not a unit stat.

## The lesson that got here

`docs/63` ended by writing down **where the search stopped and what to try
instead**. That note is what made this pass ten minutes instead of an hour:
"start from the constructor" pointed straight at `FogMap_AddSource`, whose
callers named `FogCircle` as the source, whose setter took the radius as an
argument, whose absent callers pointed at the overlays.

Four hops, each one implied by the last. **A dead end written down with its
direction is most of the next attempt.**

And one general rule: **"no callers" is a claim about where you looked.** This
project has scanned the ARM9 for fifty passes and the overlays hardly at all;
a function that looks dead in the ARM9 is a function to look for in the 91
mission images.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 53.8% | 2,501 listed + 2,506 derived, 53 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
