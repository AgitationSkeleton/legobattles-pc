# Detecting mod content

**Every file each community mod changes is now enumerated, and the mods turn out
to self-identify.** This is the groundwork for offering their maps as extra
options without distributing them.

## What each mod actually changes

Against `VANILLA_MapBase.nds`. No mod adds or removes a file; all three only
replace existing ones.

| | REbalance | FactionWars | FUNLite |
| --- | ---: | ---: | ---: |
| files changed | 32 | 6 | 3 |
| `BP/Entities.ebp` | yes | yes | yes |
| `LOC/American_English.lng` | yes | yes | yes |
| `Maps/*.map` | **14** | – | – |
| minimap art (`ck3_*mini*`) | 12 | – | – |
| tileset palettes (`*.NCLR`) | 3 | 3 | – |
| `Sound/sound_data.sdat` | yes | yes | yes |

## The sound difference is not a mod change

All three mods ship a **byte-identical** `sound_data.sdat` — same SHA-1,
58,933,824 bytes against vanilla's 62,980,480. Three unrelated hacks do not
independently produce the same 58 MB audio bank; they were all built from the
same trimmed base ROM.

**So the sound diff is a repack artefact and must be ignored by any detector.**
Treating it as content would make every mod look like it changes audio when none
of them do.

## The mods self-identify, in string 17

The only language change in any of them is a version stamp appended to the
title-screen prompt:

```
id 17  "Touch the Screen to Start"  ->  "Touch the Screen to Start\nREbalance_1-15-2"
                                        "Touch the Screen to Start\nFactionWars_1-2"
                                        "Touch the Screen to Start\nFUNLite_1-7-2"
```

**That is a detector.** A candidate ROM's `LOC/American_English.lng` string 17
names the mod and its version, so the port can identify what it has been pointed
at rather than guessing from file hashes.

Note the versions differ from the standalone patch source in
`sources/LegoBattlesRebalancePatch-main`, which is 1.14.3-2 while the ROM here
carries 1-15-2. A detector should report the version it finds rather than assume
one.

> **Correction (docs/391):** this compared only the MapBase family. The
> **MapDump** ROMs (all four mods ship one) replace 18 of the 30 multiplayer
> maps, byte for byte the same in every mod, with their previews, detail
> tiles and names. MapDump adds 5 files, lowercase duplicates. REbalance's 14
> maps below are **campaign** maps (`ck*`), not skirmish maps. The loader that
> "has not been looked at" is `FUN_02035FAC` → FS_OpenFile, from
> `"Maps/" + name + ".map"`. The port now adds such maps from a mods folder
> (docs/391).

## The maps are edits, not additions

REbalance's 14 changed maps are all castle-world skirmish maps:

```
ck1_1  ck1_3  ck1_4  ck1_6
ck2_1  ck2_2  ck2_3  ck2_4  ck2_5  ck2_6
ck3_1  ck3_2  ck3_3  ck3_4
```

Their sizes move by tens of bytes either way (`ck1_1` +47, `ck2_4` −78), which is
the signature of **edited layouts rather than new maps**. That matters for the
requested mods folder: offering them "after the vanilla list" means offering
*alternative versions of existing maps*, so the list needs to distinguish
`ck2_4` from `ck2_4 (REbalance)` rather than appending an unnamed extra.

Twelve minimap graphics accompany them — `ck3_2mini`, `ck3_3mini`, `ck3_4mini`
and their `NT` variants, in both `NCBR` and `NCGR` — so the previews for the
edited maps travel with the layouts.

## The palettes change values, not size

`KingTileset.NCLR`, `MarsTileset.NCLR` and `PirateTileset.NCLR` are 552 bytes in
both vanilla and the mods that touch them. A palette that is the same size but a
different hash has had its **colours** changed, which is why the mods look
different without shipping new tiles.

FactionWars changes the three palettes and no maps at all, so its visual
difference is entirely recolouring.

## What a mods folder needs

Now known:

* **identity** — string 17 of the ROM's language file;
* **which maps differ** — a hash comparison against the retail ROM, per path;
* **what travels with a map** — its minimap art and the tileset palette its
  world uses;
* **what to ignore** — the sound bank, and `BP/Entities.ebp` when the balance is
  already covered by the bundled tables.

Still unknown: whether a `.map` can be loaded from outside the cartridge's
NitroFS at all, which is the real question for the feature and has not been
looked at. `tools/lb_map.py` and `docs/data/maps.tsv` cover the format; the
loader that consumes it has not been located.
