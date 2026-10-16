# SetOwner's one dispatching site is the spawn path

`docs/147` left two controls un-rebadged because `Entity::SetOwner` has **one**
dispatching call site rather than zero, and one is not zero. That site is now
identified, and it settles both.

## The site

It lives in the generated function for **`0x0205C604`**, whose ARM is:

```asm
0205C604  push {r3,r4,r5,r6,r7,lr}
0205C624  bl   02082DF4          ; create something; bail if it returns 0
0205C63C  bl   0205C708
0205C644  bl   02087250          ; resolve a handle -> r5, the new entity
0205C650  bl   02087390          ; resolve r6       -> r1, an owner
0205C65C  bl   020598C4          ; Entity::SetOwner(new entity, owner)   <<<
0205C660  ldrb r0, [r6]     ; strb r0, [r5, #0x164]   ; stamp the creator
0205C668  ldrb r0, [r6, #1] ; strb r0, [r5, #0x165]
0205C670  bl   0208330C     ; str  r0, [r5, #0x180]   ; and a timestamp
0205C684  strb r1, [r4]     ; write the new handle back to the caller
```

Create an entity, give it an owner, record who made it and when, return its
handle. **This is the spawn path.** Its only caller is `0206B8CC`.

## Which settles both controls

Hotwire changes an existing entity's owner from `HotWireSpell`'s tick
(`func_02078D5C`), and that function has **zero** dispatching call sites. The
one reachable route into `SetOwner` is spawn, which is not hotwire.

So neither control can act on a hotwire:

* **Hotwire targets: specials only** -- the filter is implemented and correct
  (`docs/135`) and sits inside `set_owner_override`, which a hotwire never
  reaches;
* **Hotwire Kills Transport Occupants** -- the occupant sweep is in the same
  override, with the same problem. Its convert mode was badged **WIRED**.

Both are now **BLOCKED**, with the reason on the panel. Badge census:
**5 wired, 5 read, 7 unread, 3 blocked** -- three controls moved out of
optimistic states this iteration and the one before it.

## What "one site" taught

`docs/147` deliberately refused to badge these two on suspicion, and that was
right: the count alone could not distinguish "reachable on the path that
matters" from "reachable somewhere irrelevant". Reading the site took one
disassembly and turned a guess into a fact.

The general rule for this port, now stated fully: **a non-zero dispatch count is
necessary but not sufficient.** The count says an override can fire; only
reading the call site says whether it fires where the setting needs it.
