# The packet layout

**Status: the wire protocol is read end to end. Send and receive are both
located, the header is a 1-byte type id followed by a `u16` owner, and the
vtable offsets confirm `docs/87`'s slot assignments independently. Item 5's
protocol work is essentially finished.**

## The packet

```
+0   u8    type id      command+0x0C & 0xFF   (26 known values, docs/88)
+1   u16   owner        command+0x10, little-endian
+3   ...   body         the class's own slot 3 / slot 4
```

No length field. No magic. No alignment or padding. The receiver measures the
packet by how far the read cursor moved.

## Send

`func_020044BC` — **`Command_Send(cmd, primary, threshold, overflow)`**:

```c
g->primary_4  = primary;      /* install the destination buffers */
g->limit_0    = threshold;
g->overflow_8 = overflow;

Command_WriteHeader(cmd);     /* func_02004588 */
cmd->vtable[3](cmd);          /* ldr r1,[r1,#0xc] -> offset 0xC = SLOT 3 */

g->primary_4 = g->limit_0 = g->overflow_8 = 0;   /* tear it down */
```

Those three globals are exactly the ones `Stream_WriteByte` reads (`docs/87`), so
**the two-buffer threshold is installed per send and cleared afterwards** — it is
a framing device, not ambient state. A port that keeps one permanent buffer
changes where the split falls.

`func_02004588` — **`Command_WriteHeader`**:

```c
Stream_WriteByte(cmd, cmd->type_0C & 0xFF);   /* the tag, ONE byte */
Stream_WriteU16(cmd, &cmd->owner_10);         /* func_0200484C: ldrh then write16 */
```

**The tag is masked to 8 bits**, which bounds the id space at 0–255 and is
consistent with the 26 ids running 0 to 33.

## Receive

`func_02004504` — **`Command_Receive(cmd, out)`**:

```c
BeginRead(g_stream);                    /* func_020EDFC8 */
start = g2->cursor_20;
ok = false;
if (Command_ReadHeader(cmd)) {          /* func_020045AC */
    if (cmd->vtable[4](cmd)) {          /* ldr r1,[r1,#0x10] -> offset 0x10 = SLOT 4 */
        RecordConsumed(out, start - g2->cursor_20);   /* func_020EE368 */
        ok = true;
    }
}
EndRead(g_stream);                      /* func_020EE07C */
return ok;
```

`func_020045AC` — **`Command_ReadHeader`** — is the exact mirror of the writer,
and fails the whole receive if the stream is exhausted:

```c
u8 tag;
if (!Stream_ReadByte(cmd, &tag)) return false;
return Stream_ReadU16(cmd, &cmd->owner_10);
```

## Two independent confirmations of `docs/87`

`docs/87` identified slots 3 and 4 as serialise and deserialise by their argument
conventions — by value versus by address with a boolean return. The framing layer
confirms it from a completely different direction, through the raw vtable
offsets:

```
Command_Send     ldr r1, [r1, #0xc]    offset 0x0C / 4 = slot 3
Command_Receive  ldr r1, [r1, #0x10]   offset 0x10 / 4 = slot 4
```

Two readings, two methods, same answer.

## The tag is written but skipped on read

`Command_ReadHeader` reads the tag into a **stack local and discards it**. So the
receiver already knows which class it is building before it gets here — something
above peeks the tag to choose the class, and this call consumes the byte so the
body lines up.

**That peek is not located.** It is the last unread piece of the receive path, and
it is small: whatever picks a class from a byte. A port can already frame, route
and decode using the layout above; what it cannot yet do is *reproduce the
cartridge's own choice* of how the peek is implemented.

## Length is implicit

`RecordConsumed(out, start - cursor)` measures the packet after the fact. There is
no length prefix, so **a truncated packet is detected only by a read failing
mid-body** — which is exactly what `docs/87`'s per-field validation and
`Stream_ReadByte`'s fail-on-empty exist to catch. The two designs fit together:
no length field is safe *because* every read is checked.

For the port this is a hard requirement rather than a detail. Dropping the
per-field checks in favour of a length prefix would be a different protocol.

## Where item 5 stands

**Read and reproducible:** the packet layout, both header directions, the codec
(`docs/87`), the buffer installation, the 26 type ids (`docs/88`), and the
validation contract.

**Still unread:** the tag peek that selects the class; what the sync commands
hash and how often; what a lobby holds; the DS wireless transport that carries
these packets.
