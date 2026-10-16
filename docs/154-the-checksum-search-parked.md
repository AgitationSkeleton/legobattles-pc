# The checksum search, parked

Three iterations have gone into the save record's four-byte checksum. It is not
found. This records the search space that has been eliminated so the next
attempt starts where this one stopped, and states plainly that the thread is
parked rather than finished.

## Eliminated by measurement

**Algorithms**, tested against the three distinct `(checksum, payload)` pairs
from `docs/151`, over bytes `4..242`:

* CRC-32, its complement, Adler-32, byte sum, `u16` sum, `u32` sum, and CRC-32
  with the field zeroed, in both byte orders (`docs/151`);
* a parameterised sweep: **9 polynomials** -- including CRC-32, CRC-32C,
  CRC-32K, CRC-32Q and the normal and reflected forms -- crossed with **3 init
  values**, **2 xor-outs**, and both input and output reflection, giving 216
  combinations;
* seeded accumulators: byte sum, `u32` sum and `u32` xor, each with five seeds.

**Code paths**, each caught by a watchpoint and each eliminated by reading it:

* `02110884` -- writes the exact checksum value, and is **memcpy** (`docs/153`);
* `0380299C` -- reads exactly 239 payload bytes, and is the **EEPROM read-back
  verify** (`docs/153`);
* `02110880` -- the memcpy's load side; its source run is the **512-byte save
  image** at `02168A40`, not the record.

## What is known and useful regardless

* the live record buffer is at **`02168500`**, checksum at `+0`, name at `+4`;
* the assembled 512-byte save image is at **`02168A40`**;
* the record is a **`Battles::CardPage`**, and the four-byte header is
  structural: `func_0204BC14` returns 4 and the constructor stores `size - 4` as
  the payload length (`docs/152`);
* three distinct `(checksum, payload)` pairs exist to test any future candidate
  instantly.

## Why it is parked rather than pursued

The checksum is a prerequisite for exactly one thing: writing unlock bits into a
save (`docs/151`). That is one of seven `unread` settings, and the three
`blocked` settings are worth more and are blocked on something else entirely
(`docs/149`).

Three iterations for one prerequisite of one setting is past the point where
this is the best use of the next iteration. The eliminations above are the
durable result; the next attempt should start by finding the record buffer's
*producer* -- the code that fills `02168500` before the memcpy -- rather than by
guessing algorithms, since the guessing space is now well covered.
