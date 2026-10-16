# The save record has a checksum, and the name field is confirmed

`docs/150` asked for two saves differing by one known change. Here they are: the
warp route typed `Q W E` for one and `Q W` for the other, dropping a single
letter and nothing else.

## The diff

Record 0 differs at **exactly five offsets**:

```
+000   Qwe=59   Qw=80
+001   Qwe=F5   Qw=C7
+002   Qwe=24   Qw=8E
+003   Qwe=43   Qw=51
+006   Qwe=65   Qw=00      <- the dropped letter
```

`+0x06` is the third character of the name, which is the change that was made.
The other four are the leading word.

## Two things this settles

**`+0x00 .. +0x03` is a checksum, not an id.** One content byte changed and all
four leading bytes changed with it. An id would be stable, or would change
independently of content; a checksum changes exactly like this. `docs/150`
listed both possibilities and could not choose between them -- this chooses.

**The name field is confirmed and bounded.** `+0x04` onward, null-terminated,
and *only* the altered character moved. Nothing else in the record is derived
from the name.

And a third thing, for free: `Qwe/rec1` and `Qw/rec1` carry the **same**
checksum `F8180082`, so record 1 is byte-identical across both saves. It is an
untouched second slot, not a mirror of record 0 -- which refines `docs/100`'s
note that record 1 is "a byte-for-byte copy of record 0 in a fresh save".

## The consequence for the Unlock family

**Any write into a save record must recompute this checksum**, or the game will
presumably reject or repair the record. That is a prerequisite nobody had
identified, and it would have turned a working unlock patch into a corrupted
save.

It also means the Unlock family cannot be done as a naive data patch after all:
it needs the checksum algorithm first.

## Which is not guessed

Tested against all four records as known pairs, over bytes `4..242`: CRC-32, its
complement, Adler-32, a byte sum, a `u32` sum, a `u16` sum, and CRC-32 with the
checksum field zeroed -- in both byte orders. **None matches.**

So the algorithm is custom, or covers a different range, or folds in something
outside the record. Guessing further would be expensive and unreliable; the
answer is in the code. `docs/100` already located the save container builder at
`func_0204B0A4` and the magic writer at `func_0204B1D4`, and the checksum
routine is the natural neighbour of both.

**That is the next read**, and it is a narrow one.
