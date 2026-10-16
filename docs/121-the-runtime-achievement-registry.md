# The runtime achievement registry

`docs/97` enumerated the 25 achievements from the ROM and ended by saying what
it could not give: **where progress is stored**, without which the overlay's
*Unlock Everything* can only honestly be drawn `[unread]`. This finds the
runtime half.

## The ROM table, read in full

`0x0213BE8C`, thresholds at `+0x00` and the per-entry byte at `+0x32`:

```
 0 UnlockHiddenHeroes         0     9 WinMultiplayer         20    18 UnlockAllCodes          0
 1 CollectStuds           50000    10 Play3PlayerMP           1    19 UseRoarSpell          200
 2 KillKingWithKing           1    11 CompleteSide1Campaign   0    20 HealPoints          20000
 3 DefeatEnemyUnits        1000    12 CompleteSide2Campaign   0    21 BuildMountedUnits     250
 4 DefeatSiegeUnits         100    13 CompleteSide3Campaign   0    22 BuildSiegeUnits       100
 5 DestroyBuildings         250    14 CompleteSide4Campaign   0    23 BuildFarms            200
 6 WinSkirmishGames          20    15 CompleteSide5Campaign   0    24 BuildTowers           100
 7 WinResourceRace            5    16 CompleteSide6Campaign   0
 8 WinKillTheKing             5    17 PurchaseAllConceptArt   0
```

The nine zero thresholds are the one-shot achievements -- the six campaign
completions, concept art, all-codes, and hidden heroes -- which are set by an
event rather than counted up to.

## Finding the runtime array

`func_020A68D0` builds 25 records of 40 bytes, so the array can be found by
**signature** rather than by catching the builder: scan main RAM for the u16
`0xC350` (CollectStuds' 50000) with `1` at `+0x28`, `1000` at `+0x50` and `100`
at `+0x78` -- the next three thresholds at the record stride.

Exactly one match in `02000000..02400000`:

    record array base = 02156DBC

which corroborates `docs/97`'s note that the builder's destination global is
`02156D00`.

## Record layout, confirmed against the ROM

```
+0x00  u32   index 0..24
+0x04  u32   index again
+0x08  ptr   name string      (matches the ROM's base+0x4C array exactly)
+0x0C  u32   string id        (0xB65, 0xB68, 0xB6E ...)
+0x18  u32   0x40, constant
+0x1C  u32   1, constant
+0x20  ptr   per-entry state buffer
+0x24  u16   threshold        (matches the ROM's base+0x00 array exactly)
+0x26  u16   the per-entry byte
```

All 25 records verified: `+0x24` reproduces the ROM threshold list above
entry-for-entry, and `+0x08` resolves to the right name for every index.

## The per-entry state buffers

`+0x20` points into `023DE7F0 .. 023DEF70`, descending by `0x50` per record.
Each is a **64-byte payload** -- the `0x50` stride is 64 bytes plus a 16-byte
heap header, visible as the `RF`/`DU` block magic, size `0x40`, and two
free-list pointers at `+0x40` of each dump.

Every one of the four sampled buffers is **entirely zero**, which is what a
profile with no achievements should look like, and is consistent with the fresh
save `docs/100` describes.

## What remains before Unlock can be wired

The progress field's **offset within the 64-byte buffer is not yet known**,
because a buffer of zeros does not say which of its words is the counter. That
is one measurement away and does not need guessing: watch
`023DE7F0..023DEF80` during play and see which offset moves when studs are
collected or a tower is built.

Until then no write is made. Setting a byte because it is plausibly the counter
is guessing, which this project does not do, and the overlay
keeps its `[unread]` badge, which remains the honest label.

What has changed is that the badge is now one measurement from `[wired]` rather
than blocked on an unread save format.
