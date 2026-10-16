# 210 — Naming the unit in the widened column, and the nibble that hid a bug

Goal: the adaptive-view requirement "modelled special unit rendering". docs/209 showed *a*
unit in a widened column. That is not the same claim. "Modelled special unit"
is a statement about **which** entity is drawn, and a grey shape in a
screenshot cannot settle it.

## Ask the data, not the picture

The entity pool already carries the answer. From `lb_field_entity_at`:

    pool    = *(u32*)0x02155028
    array   = *(u32*)(pool + 8)      count = *(u32*)(pool + 0xC)
    entity  = *(u32*)(array + 4*i)
    record  = *(u32*)( *(u32*)(entity + 0x160) + 8 )
    owner   = *(u8*)(entity + 0xE4)      category = *(u8*)(record + 0x5C)
    world x = *(s32*)(entity + 0xEC) >> 12    (minus 29, docs/169)

`record` is the part that names it. Blueprint records sit at
`base + 4 + index * 0x7C` — the same constants `lb_patches.cpp` already uses to
patch them — so `(record - base - 4) / 0x7C` indexes `out/eur/entities.json`.
That file has 551 named records, and its categories include **category 0 =
"Hero"**, 25 of them: the game's own word for its special units.

New: `lb_field_entity_blueprint()` exposes the record address, and `LB_ENTLOG=1`
prints every entity that lands in a widened column with its blueprint index,
plus a `[lb_hero]` line for every category-0 entity wherever it is.

## Aiming instead of hoping

`[lb_hero]` exists so the harness can close the loop. `--aim-hero` reads the
hero's screen x out of the port's own log, presses the key that moves the camera
the right way, and repeats until the hero is inside the left widened column
(target guest x −88..−8). It converges in three presses:

    aim: sx=147 -> held RIGHT 0.40s
    aim: sx=35  -> held RIGHT 0.31s
    aim: HERO IN THE LEFT WIDENED COLUMN at sx=-45

The run before this one panned eight fixed steps and never got the hero closer
than 381 px off the left edge. Fixed-step panning was never going to answer the
question.

## The bug this found: one condition nibble

With the hero measured at screen x −61, **nothing was drawn there**. Grass.

`[lb_cull]` said why, once it was asked to:

    [lb_cull] gather rect right edge camx+256 -> camx+352 px

...and nothing about the left edge. The guard for the left-margin block had
never matched, because docs/208's fix transcribed the third instruction as
`E2400001` when the ROM holds:

    02038FB0  E3500000  cmp   r0, #0
    02038FB4  03A00000  moveq r0, #0
    02038FB8  12400001  subne r0, r0, #1      <- cond nibble 1, not E

It is `subne`. Its condition nibble is 1. `E2400001` is the unconditional `sub`,
which is not in this function at all. So the three-word compare failed, the
patch declined, and **the left half of the entity gather rect was never
widened** — through a build, a run, and a write-up that claimed it was.

This is the same trap as the struct-field-width one: the address was right, the
encoding was not, and everything downstream inherited it.

### The guard now says when it declines

A guard that refuses in silence is indistinguishable from one that fired. The
right edge printed its success line, so the rect *looked* patched. It now prints
what it actually found:

    [lb_cull] LEFT EDGE NOT PATCHED: F12DF5F1 F12DF12D 2B652061 at 02038FB0 ...

That line still appears once per run, early — the ARM9 image is not decompressed
into main RAM yet on the first vblank, so the address holds data. The patch
lands on a later frame and says so. Both lines together are the honest account;
only the second one used to exist.

## The result

`screenshots/exp-hero2/hero-zoom.png`, aimed and measured:

    [lb_ent] LEFT-EXT sx=-45 sy=95 owner=0 cat=0 bp=0 rec=0226A134 base=0226A130

`bp=0` is `K_King`, category `Hero`, display name "King". The capture shows him
— crown, blue and gold robes — standing in the **left** widened column with his
guard around him. The same frame has Guardsmen and Knights out there too. The
identical spot in the previous run was empty grass.

Note `rec - base = 4`, so blueprint index 0, and the first version of the
diagnostic tested `rec > base + 4` and reported `bp=-1` for it. Blueprint 0 is
exactly the entity the diagnostic exists to identify. Fixed to `>=`.

## What this changes

* **Modelled special unit rendering: confirmed, by name.** Not "a unit"; a
  Hero, blueprint 0, `K_King`.
* **docs/208 and docs/209 were both over-confident about the left column.**
  docs/209's capture showed a unit in the RIGHT widened column, which was true,
  and the left column was never separately checked. It did not work until now.
* Units render in **both** widened columns as of this change.

## Still open

* **Effects and particles** in the widened columns: still unaudited. The entity
  log covers units; effects are not entities in this pool.
* Fog **edge-tile textures** (docs/203).
* HUD repositioning and scaling.
