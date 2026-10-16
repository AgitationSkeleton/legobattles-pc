# Which units are actually "modelled"

The project's requirements list "unit rendering, modelled special unit rendering" as two
separate things to prove in the widened screenspace. Until now I had been
treating "modelled special unit" as a synonym for category 6 (Siege/Special) by
inference, and hunting for a category-6 unit to photograph in a widened column.
The inference was right, but it was still an inference, and it was aimed at the
wrong half of the problem.

`docs/data/entities.tsv` carries an `asset` column. Grouping all 551 entity rows
by `category_id` and listing the distinct asset prefixes settles it from ROM data
rather than from reasoning:

| category | count | asset prefix |
|---|---|---|
| 0 Heroes, 1 Builder, 2 Melee, 3 Ranged, 4 Mounted | 58 | `Sprites/…` |
| **5 Transport Ship** | **7** | **`Models/…`** |
| **6 Siege/Special** | **25** | **`Models/…`** |
| 7–16 structures | 81 | `Sprites/…` |

**Only categories 5 and 6 are model-backed. Everything else in the game is a
sprite.** So "modelled special unit rendering" names exactly those two
categories, and the phrase is precise rather than loose: the game really does
have two distinct asset pipelines, and the question is whether the
model-backed one survives the widened viewport.

The important consequence is that **category 6 is not the only way to exercise
that pipeline**. Category 5 is a Transport Ship, it is model-backed by the same
`Models/` prefix, and it costs **250** against the cheapest category-6 unit's
**350**. Both go through whatever renders `Models/`. Photographing either one in
a widened column tests the same code path.

## Why no run had ever produced one

Three failed attempts, each for a different and now-understood reason:

- **Act 3 story mission (`siege1`, economy cheats, 8 min, 327 census samples).**
  The roster never changed once: `0:1 1:2 2:12 3:3 4:3 15:2`. The Level Select
  blurb reads "Explore the maze to find an exit on the other side" — it is an
  **exploration mission with a fixed roster and no base building**, so no amount
  of economy cheating can produce a unit that the mission never builds.

- **Free Play default map (`siege2`, economy cheats, 9 min, 77 samples).** Here
  the economy *does* run: builders 2→10, melee 0→5, and structure categories
  10/11/12/13 appearing. The tech tree simply had not reached specials in nine
  minutes. This run is the proof that the approach works, just not fast enough.

- **`siege3` and `naval1` (long runs) died within a minute.** Not evidence of
  anything about units — see the harness note at the bottom.

The common thread: **a landlocked map cannot field a naval unit at all**, and
every category-5 unit and most category-6 units are ships.

## The map is the variable that matters

`docs/data/maps.tsv` groups the 122 maps into three tilesets — `KingTileset`
(41), `PirateTileset` (41), `MarsTileset` (40). Ten `PirateTileset` maps are
`mp*`, which is the Free Play/multiplayer set: mp03, mp08, mp11, mp14, mp21,
mp23, mp25, mp26, mp27, mp30.

Rather than guess which Free Play entry maps to which `mp` file, the map-select
screen was enumerated directly. The screen has a name bar flanked by two arrows;
a pixel scan of the row at y=133 in the 512-wide surface finds three bright
runs — `(172,182)`, the name bar `(187,326)`, and `(329,339)` — giving arrow
centres at image x≈177 and x≈334. The surface is 512 wide with the menu centred,
so the band is `(512-256)/2 = 128` and native touch x is image x − 128:

- left arrow **(49, 133)**, right arrow **(206, 133)**

That transform is corroborated by the Continue button, which lands at native
x≈204 against the route's long-established 205.

Ten right-arrow taps, each photographed by the harness's per-step capture, gave
the head of the Free Play map list. Measuring the water fraction of each
thumbnail (blue-dominant pixels inside the preview frame):

| taps from default | water | map |
|---|---|---|
| 1 | 2.1% | Entrenched |
| 3 | **67.7%** | **River Crossing** |
| 6 | 53.3% | (naval) |
| 9 | 30.5% | (part-naval) |

**River Crossing** — four land quadrants separated by water channels — is three
right-arrow taps from the default. On a map where the land is split by water,
transports are not a luxury the AI may or may not get around to; crossing
requires them. And since every one of the six factions has a category-5 Transport
Ship at 250, the faction chosen at army select does not matter.

## Harness note: background launches were dying

`siege3` and `naval1` both died within a minute with no route directory and an
empty stdout, which looked like a game failure and is not one. Both were launched
through the Bash tool's `nohup … &` background form. The two runs that succeeded
(`mapsel`, `maplist`) were foreground `timeout … python …`. The child does not
survive the background parent shell exiting, so **long instrumented runs must be
launched from PowerShell**, which is what the existing note about the Bash tool
losing env at the `.exe` boundary was already pointing at.

`mapsel` also turned up a stray `nds_runner.exe` (PID 58456) still holding the
debug port, which is the more likely proximate cause of `siege3` specifically —
the same failure mode already recorded as "stray instances share the debug port
and fake a broken read path".

## A model-backed unit exists on the field — measured

`naval3` ran the route above on River Crossing with the economy cheats for 780
seconds and produced **143 census samples, 19 of which contain `5:`**. The final
roster:

```
[lb_ent] categories: 0:2 1:10 2:6 3:3 5:1 7:2 8:1 10:4 11:1 12:1 13:5 16:1
```

`5:1` is a **Transport Ship** — `Models/…`-backed, category 5. This is the first
model-backed unit observed anywhere in this project, and it settles the question
the three earlier runs could not: the modelled pipeline does instantiate units at
runtime, and the reason no earlier run saw one was the map, not the renderer.

The route capture `route/08-ok-206-133.png` shows "River Crossing" selected
before the four Continues, so the map is confirmed by picture rather than by
assuming the taps landed.

Two things that run did NOT show, kept separate from what it did: the transport
was never in a widened column (`0 IN A WIDENED COLUMN` on every line), and
category 6 was still unobserved. Both were settled by the next run.

## A modelled special unit, named, in a widened column

`model1` — the same River Crossing route with the same cheats, lingering 840s —
reached **both** model-backed categories. Its census carries `5:2 6:1`, and the
port's own flag fires:

```
[lb_ent] categories: 0:2 1:10 2:8 3:2 5:2 6:1 7:2 8:2 9:1 ...   <-- SIEGE/SPECIAL PRESENT
```

and then, the line this whole thread was for:

```
[lb_ent] LEFT-EXT  sx=-39  sy=19   owner=1 cat=6   bp=27   rec=0226AE48 base=0226A130
```

Blueprint 27 resolves in `docs/data/entities.tsv` to:

| idx | category | name | faction | cost | health | asset |
|---|---|---|---|---|---|---|
| 27 | 6 | **Troll Giant** | Wizard | 500 | 550 | **`Models/W_Giant`** |

**A Troll Giant — a `Models/`-backed category-6 unit — rendered at sx=-39, in the
LEFT widened column.** Named from ROM data by blueprint index, not inferred from
its appearance. That is "modelled special unit rendering" in
extended screenspace, measured.

The corroboration is that the other unit the same run put in a band is
`cat=2 bp=23`, which resolves to **Skeleton**, `Sprites/w_mel`, also Wizard, also
`owner=1`. Same player, same faction, one sprite-backed and one model-backed,
both drawn in the left column. The two asset pipelines were exercised side by
side by the same opponent.

Note this happened during the **linger**, not the pan — the Giant walked into the
band on its own. The pan was insurance, not the cause.

### And the transport, moving, in the same column

The pan then produced the other model-backed category:

```
[lb_ent] LEFT-EXT  sx=-184 sy=137  owner=1 cat=5   bp=29   rec=0226AF40 base=0226A130
[lb_ent] LEFT-EXT  sx=-165 sy=111  owner=1 cat=5   bp=29   rec=0226AF40 base=0226A130
[lb_ent] LEFT-EXT  sx=-190 sy=148  owner=1 cat=5   bp=29   rec=0226AF40 base=0226A130
```

Blueprint 29 is **Transport Ship**, Wizard, 250, asset **`Models/W_TransportShip`**.

The finished run holds **six** such samples, and every one carries the same
`rec=0226AF40` — one entity, at five distinct screen positions:

```
(-184,137)  (-165,111)  (-190,148)  (-193,150)  (-165,111)  (-182,129)
```

It is not a static object that happens to sit in the extended column; it is being
**animated and re-rendered there across frames**. For "adaptive widescreen must
render units in the extended screenspace", a moving unit is the stronger claim,
and this is it. The identical `rec` across all six is what rules out the cheaper
explanation that six different entities each blinked through once.

Note the magnitudes: −184 and −190 lie beyond the 128-pixel band of a 512-wide
surface. These samples come from the resize phase, where the guest surface is
wider (946x512 gives a band of `(946-256)/2 = 345`), so the entity is legitimately
inside a larger left column. Consistent, not anomalous.

### The four sightings together

| category | bp | name | asset | sx |
|---|---|---|---|---|
| 6 | 27 | Troll Giant | `Models/W_Giant` | −39 |
| 5 | 29 | Transport Ship | `Models/W_TransportShip` | −184, −165, −190, −193, −182 (6 samples) |
| 2 | 23 | Skeleton | `Sprites/w_mel` | −21 |

All three are Wizard, all `owner=1`, all in the left widened column. **Both
model-backed categories and a sprite-backed one, drawn in the extended columns by
the same opponent in the same match** — which is as direct a comparison of the
two asset pipelines as this game can offer.

## Reproduced on a second run, and cross-checked against itself

`model2` — same route, same cheats, and the first run with the new `[lb_model]`
logging built in — put the Troll Giant in the left column again:

```
sx=-107  sy=1    cat=6  bp=27
sx=-29   sy=22   cat=6  bp=27
```

So the result is reproducible rather than a one-off, and this time it was
photographed by two independent mechanisms that can be checked against each
other. Over 507 `[lb_model]` samples, 139 had `sy` on screen, and of those
**exactly 2 satisfy the band test** (`kExt = (512-256)/2 = 128`, so sx in
[-128,0) or [256,384)). The band lines printed by `entity_log` also number
**2**. Two separately-written pieces of code agreeing on the count is a much
better check than either one asserting it alone.

That reconciliation also corrected a mistake made while reading the data. A first
pass counted "sx off the native screen" — `x < 0 or x >= 256` — and reported 10,
which disagreed with the 2 printed lines and briefly looked like the log dropping
hits. It was the analysis that was wrong: off-screen and in-a-band are not the
same test, and only the second one is the question. That is the same distinction
`entity_log`'s own comment warns about, having been got wrong there once too.

### A hardcoded bound, examined and left alone

The disagreement above first pointed at the scanline test, `sy >= 192`, which
uses a literal while the horizontal test uses the live surface via `kExt`. A
transport was sitting at `sx=312` — inside the right column — and being
discarded, apparently because its `sy` was 288..443.

**That was not a bug, and the check settled it.** All 385 geometry samples in
that run report `guest=512x192`, so the surface really is 192 tall and `sy=288`
really is off the bottom of the screen. The unit was beside the camera
horizontally and below it vertically; rejecting it was correct.

The literal was replaced with the live `gh_` anyway, on the narrow grounds that
two expressions meaning the same thing should not be written two different ways,
and it stays right on its own if the guest surface ever grows taller. It changes
nothing for any run to date.

### What this does not claim

No screenshot isolates either unit. The measurement is the
evidence here; `route/linger-340.png` shows River Crossing rendering edge to edge
at 1920x1080 — water, shoreline and forest continuing into the extended columns
with no letterbox and no stretch — but at that scale it does not single the unit
out. A deliberate capture wants the camera parked on the unit rather than a frame
hunted out of a sweep, which is what the `[lb_model]` logging added to
`entity_log()` is for: it prints every category-5/6 unit's screen position every
sample, so a pan can be *aimed* instead of hoped through. That logging is written
but not yet built at the time of writing.

The per-entity band line already prints `cat=` for **any** category — the
"category 0 (Hero)" in the summary is only a counter — so no port change is
needed to name a category-5 unit once one stands in a band. What was needed is
timing: `--pan` runs *after* `--linger` in the harness, so a run that lingers
past the ~11-minute mark and then pans sweeps the transport through the bands.
That is the `model1` run.

## Terrain in the extended columns, on a fresh map

Incidentally, `naval3`'s in-battle captures are a clean check of the concern that
"terrain might not appear right in extended screenspace" on a map no previous
run had loaded. `route/linger-100.png` at 1052x433 shows grass, rocky ground and
sand continuing edge to edge with no letterbox, no stretch and no seam, with
units and a structure composited correctly over them.

Fog is *not* testable in these runs: `LB_CHEATS` includes index 0, Reveal Map.
The shroud was measured separately in docs/237 (`shrouded=0/768`).
