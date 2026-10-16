# Fog of war

**Status: the vision system is read end to end — two bitmaps, a linked list of
sources, a midpoint-circle rasteriser, and a footprint that makes a building's
reveal a capsule rather than a circle.** Seven functions named into tier R; 84
of 84 checks pass.

`docs/62` closed the decompilation segment at a measured ceiling and `docs/58`–
`60` covered the economy, combat and build systems. Vision is the last large
gameplay system with nothing written about it, and `docs/28`'s red-brick table
points at it as usual: `RedBrick_TurnOffFow` — "Reveal Map" — lists eleven
readers.

## Two maps, not one

`func_020A15D8` is the whole update:

```c
void FogMap_Update(FogMap *self) {
    self->dirty_24 = self->dirty_25;
    BitArray2D_Clear(self->visible_14);          /* every frame */
    for (Source *s = self->sources_04; s; s = s->next_04) {
        if (!s->radius_10) continue;
        FillCircle(self, self->visible_14, w, h,
                   s->x_0C, s->y_0D, s->radius_10, 1,
                   s->wide_12, s->tall_13);
        if (s->mark_explored_11) {
            FillCircle(self, self->explored_10, w, h, ...);
            self->dirty_24 = 1;
            s->mark_explored_11 = 0;             /* one-shot */
        }
    }
    self->dirty_25 = 0;
}
```

**`+0x14` is what is visible now and is cleared on every update. `+0x10` is what
has ever been explored and is only ever written.** That is the classic
three-state RTS fog — black for never seen, grey for explored, clear for
currently visible — and the code proves it rather than the screenshots
suggesting it: one map is cleared each pass and refilled from live sources, the
other never is.

The `mark_explored` flag at `+0x11` is **one-shot**: set it, and the next update
stamps the explored map once and clears it. A unit standing still does not
re-stamp permanent territory every frame; only movement (or spawning) sets it
again.

## The vision source record

```
+0x04  next source in the list
+0x0C  u8   tile x
+0x0D  u8   tile y
+0x10  u8   radius, in tiles -- zero means this source sees nothing
+0x11  u8   stamp the explored map on the next update (one-shot)
+0x12  u8   footprint spans in x
+0x13  u8   footprint spans in y
```

## A circle, and why buildings are different

`func_020A16B4` is a **midpoint circle** — Bresenham's, with the decision
variable starting at `(1 - radius) << 1` and stepping by 2 — issuing **four
horizontal spans per iteration**, the standard eight-way symmetry collapsed into
span pairs.

What is not standard is the two extra parameters. The rasteriser takes the
radius **and** the source's `wide`/`tall` flags, and adds them to the centre
before drawing. That turns the circle into a **capsule**: a circle swept along
the building's footprint.

So a 1×1 unit reveals a disc, and a 2×2 building reveals a rounded square of the
same radius — every tile the building occupies gets full vision range, rather
than the building being treated as a point. It is a small thing that would be
easy to get wrong in a port and produces visibly different edges.

`func_020A1670` underneath it is a **clipped** horizontal span fill: it clamps
`x0` to 0 and `x1` to `width - 1`, rejects rows outside the map, and only then
walks `BitArray2D_Set`. The clipping is in the span primitive, not in the
circle — so the circle code never has to think about map edges.

## The bit array

The fog is `Battles::BitArray2D`, one bit per tile, and the three primitives are
each a handful of instructions:

```asm
Get       ldr r0,[r0,#0x14] ; ldr r2,[r0,r2,lsl #2] ; ldrb r1,[r2,r1,lsr #3]
          tst r1, r0, lsl r3                            rows[y][x>>3] & (1<<(x&7))
Set       the same addressing, orr to set and bic to clear
InBounds  point.x at +4 against width +8, point.y at +5 against height +0xC
```

A row-pointer table at `+0x14`, so rows are separately allocated — which is why
`BitArray2D`'s destructor frees `+0x10` and `+0x14` separately.

Worth noting for a port: **a map point here is not a bare pair.** `InBounds`
reads x at `+4` and y at `+5` of the object it is handed, so the point is
embedded in something larger.

## What the cheat actually does

`func_02025C14` is the per-entity visibility test used by the renderer:

```c
int visible = BitArray2D_Get(fog, tile.x, tile.y);
if (rules[0x1E] || rules[0x00] || rules[0x1F] || rules[0x0F])
    visible = 1;
```

`rules[0x00]` is `RedBrick_TurnOffFow`. **The cheat does not clear the fog map —
it short-circuits the query.** The simulation's fog state is untouched, which is
the right way to do it: a cheat that wrote to the map would desync a lockstep
game (`docs/25`), and this one cannot.

Three other flags do the same thing, which is why the function tests four.

## Open, and stated as open

> **Answered by `docs/69`, and one claim above corrected.** `Battles::User::FogCircle`
> is not the renderer -- **it is the vision source**, the object this document
> describes as a list entry. And the radius is **a literal passed by mission
> code**: `func_02048A94` takes it as an argument, nothing in the ARM9 calls
> that function, and all 53 call sites are in the mission overlays, passing
> immediates of 1 to 15 tiles (2 and 5 dominating). Vision is authored per
> mission, not per unit -- which is why no blueprint field carries it.

**Where a source's radius comes from is not established.** A search for any
function storing a blueprint byte into `+0x10` of the source record found
nothing, so the radius is not copied straight from `UnitBlueprint` the way
`resource_yield` is (`docs/58`). It may be set by a per-category constant, by
the entity's own stats block, or by the mission script. Flagged, not guessed —
and it is the obvious next step for anyone continuing this thread.

`UnitBlueprint+0x70` and `+0x71` are still unidentified; `+0x71` is
`interaction_range` in `lb_blueprints.py` and is read by the harvest and command
code, so neither of them is the sight radius.

## Tips and pitfalls

* **Two maps means two meanings.** Anything reading "the fog" has to say which
  one; the visible map is a frame-local scratch buffer and the explored map is
  save state.
* **A one-shot flag is a performance decision with visible consequences.**
  `+0x11` means the explored map is stamped on change, not continuously.
* **Cheats that read are safe; cheats that write are not.** Reveal Map is
  implemented as an OR in the query, and that is what keeps it lockstep-safe.
* The red-brick table has now named **five** subsystems across five passes: the
  unit cap, the harvest payout, the damage applier, the construction tick, and
  the fog query.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 53.6% | 2,501 listed + 2,494 derived, 35 read by hand |
| decompilation | 0.271% | 789 functions byte-verified, ceiling measured |
