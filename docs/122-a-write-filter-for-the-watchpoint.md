# A write filter for the watchpoint, and what it showed

## The instrument problem, fixed

The watch ring holds 512 events. A range that is **read** every frame and
written rarely reports the read traffic and nothing else: the writes are evicted
before they can be looked at, and the result looks like *"no writes happen
here"* -- a conclusion, not an absence of data.

That silent failure has now cost this thread three times, and `docs/117`
retracted two documents built on it.

`LB_WATCH` gained a companion:

    LB_WATCH_RW=w     record writes only
    LB_WATCH_RW=r     record reads only

implemented in `watch_push`, so the uninteresting half is never recorded rather
than being recorded and then evicted.

Measured on the same range, same activity, before and after:

| | saturated windows | writes seen |
| --- | --- | --- |
| unfiltered | **20 / 20** | 40 (biased -- only the tail of each window) |
| `LB_WATCH_RW=w` | **0 / 18** | **6534** |

Two orders of magnitude more write data, and every window now complete.

## What it showed about the achievement buffers

Watching `023DE7F0..023DEF80` -- the 25 per-achievement state buffers found in
`docs/121` -- during play, writes only:

```
+42  16-bit  x3024        \
+4C  32-bit  x1296         |  the 16-byte heap header that follows
+48  32-bit   x900         |  each 64-byte payload: block magic,
+40  16-bit   x432         |  size, and free-list pointers
+44  32-bit   x432        /
+00   8-bit   x450        <- the only payload write
```

The only write into the payload is at **`+0x00`**, one byte, from a single PC:

```
pc=020F2280   addr=023DEF70 .. 023DEB60   record +00   value=00   once per window, every record
```

It writes **zero to offset 0 of all 25 buffers**, repeatedly. That is a periodic
**clear**, not a counter being incremented.

## Conclusion: these buffers are not the progress store

Achievement progress does not live in the per-entry buffers reached from the
record's `+0x20` pointer, at least not in any offset that changes during play.
Their payloads stay zero and their only payload write is a reset.

So `docs/121`'s hope -- that the progress offset was "one measurement away" --
is **wrong**, and this is that measurement saying so. The buffers are a
transient per-entry scratch or presentation slot, not persistence.

*Unlock Everything* stays `[unread]` in the overlay. No write has been made and
none is justified.

## What is left

Progress must be stored somewhere else, and the two named classes `docs/97`
identified remain unread: `SaveGameEvent::SaveableGameParametersType` and
`User::ProfileCard`. The save container's 242-byte records (`docs/100`) are
still the obvious home, and their contents are still unread.

The productive next step is to diff a save record across an achievement-worthy
action rather than to keep searching RAM: the container format is known, the
port controls the save path, and a 242-byte record is small enough to diff
exhaustively.
