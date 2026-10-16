# 247 — The fog record, located instead of correlated

Status: **fixed, 100.0% agreement, on by default.** Closes the thread running
through docs/206, docs/245.

## Why the search could never have worked

Every previous attempt scanned RAM for a buffer whose contents correlate with
the fog the guest draws, and graded candidates against `g_seen` — **the
window-derived approximation being replaced**. A buffer that shares the proxy's
blind spots scores well, so four candidates in a row were adopted and dropped.
The best of them managed 88.6 / 94.6 / 94.3% agreement across the three
metatile phases.

Worse, the sampler read `base + my*stride + mx` as a **byte**. The real record
is one **bit** per cell, so even a search that landed on the right address
would have read it wrong.

## The chain, out of the decomp

`func_020EEBF4` is 32 bytes and is the only function that reads the mask:

```c
return (*(byte *)(*(int *)(*(int *)(param_1 + 0x14) + param_3 * 4) +
                  (param_2 >> 3)) & 1 << (param_2 & 7)) != 0;
```

A descriptor with a **row table at +0x14**, indexed `row[y][x>>3] & (1<<(x&7))`.

Its caller `func_02036D1C` tests a rectangle's four corners:

```c
uVar5 = *(undefined4 *)(*DAT_02036eb8 + 0x10);
... FUN_020eebf4(uVar5, x, y) on each corner ...
if (!bypass && all four read 0) return 0;      // nothing visible here
```

`DAT_02036eb8` is `0x021569E4`, read out of the static ARM9 image. The sibling
literal `DAT_02036ebc` is `0x02156CB0` — the map pointer this port already
uses — which is the independent check that this function is the map-space
visibility test and not some other rectangle.

**Polarity comes from the decomp, not from assumption**: the caller reports
"not visible" when the corners read 0, so a set bit means revealed.

    holder = u32[0x021569E4]
    desc   = u32[holder + 0x10]
    rowtab = u32[desc + 0x14]

Measured live:

| offset | value | |
|---|---|---|
| +0x04 | 8 | stride, `(64+7)/8` |
| +0x08 | 64 | width — the **map's** metatiles, not the viewport's 32 |
| +0x0C | 64 | height — not the viewport's 24 |
| +0x10 | data | +0x14 row table |

64x64 matches `map+0x14`/`+0x15`. 413 of 4096 bits set at a freeplay start.

Both the holder and the descriptor **move between battles** — `0226991C` in one
run, `022698E0` in the next — so the chain is followed per battle and
validated against the map's own dimensions and the stride the setter computes,
rather than cached.

### The two decoys

The viewport-sized bitmasks at `0x02151F28` and at `view+0x164` both read
**0 of 3072 bits**, before and after a 20 s linger. They are allocated, the
view-width patch reallocates them correctly, and nothing populates them. Both
had been mistaken for the fog record; docs/163 guessed "fog of war, shroud or a
dirty-tile map" and the last of those is what they behave like.

## Result

    [lb_findfog] LOCATED 64x64 bits, rows at 022866F8, stride 8
    [lb_phase] agreement by cam_tx%3:  0: 100.0% of 9372
                                       1: 100.0% of 148632
                                       2: 100.0% of 253836

100.0% on all three phases over more than 400,000 samples, against
88.6 / 94.6 / 94.3% for the best correlated candidate. The located record is
still graded by that same every-frame comparison and dropped on the same
streak — locating it removes the search, not the check.

Photographed with `LB_ASPECTBOX=1`: revealed terrain now extends above, below
and left of the DS aspect box with a **ragged** boundary following the real
explored region. Previously its top and bottom edges were perfectly straight
and lay exactly on the box, with solid cloud beyond — the fog view square around the
original aspect, and the spotty fog creeping onto the traversed area, that the
wrong correlated buffers produced.

`LB_FOGSRC` therefore defaults to the map record again, with
`LB_FOGSRC=window` restoring the old cache for comparison, and `LB_FOGLOC=0`
forcing the correlation search so the two can be A/B'd on one route.
