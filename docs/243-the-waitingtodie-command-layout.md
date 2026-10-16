# 243 — The WaitingToDie command, field by field

> **Corrected by docs/392.** Transport Occupants' "Destroy them" does not use
> this command. The kill the game itself uses on a dying entity's occupants is
> `func_0205E78C(entity, 0)`, an immediate call inside the simulation:
> `Sim::DieEntityCommand::v08` makes it on every handle in the entity's list
> at +0x100 (count +0x104). The same slot of `Sim::WaitingToDieEntityCommand`
> (v08, 0x02075380) is an empty function, so waiting to die is not dying.
> Destroy now makes that one call per occupant through the docs/383 hop. The
> reading of the command below stands; the premise that issuing it is how to
> kill does not.

docs/241 concluded that the five-call guest sequence behind `hotwire_transport`
mode 1 cannot complete from an override, and listed as the preferred way forward:
"reduce it to one guest call — build the command object in guest memory directly
… and call only the submit". It then described the layout as "largely known",
which is a fair summary of a guess. A structure in guest memory is not built
from a summary, so this reads it out of the image.

Every offset below comes from `tools/lb_code.py dis`, and each is quoted.

## The allocation — `func_02005B0C`

```
  0x02005b0c  push   {r3, r4, r5, lr}
  0x02005b18  ldr    r1, [pc, #0x20]     = 0x0213b0b4
  0x02005b1c  ldr    r2, [pc, #0x20]     = 0x0000020f
  0x02005b20  mov    r0, #0x2c
  0x02005b24  bl     #0x204c6e4          -> func_0204c6e4
  0x02005b28  cmp    r0, #0
  0x02005b2c  popeq  {r3, r4, r5, pc}
  0x02005b38  bl     #0x20079a8          -> func_020079a8
```

**The object is 0x2C bytes.** `func_0204C6E4(size, 0x0213B0B4, 0x20F)` is an
allocator carrying a file/line pair — a debug `operator new`. A null return is
handled, so the game itself treats allocation failure as survivable. Then
`func_020079A8(obj, sink, vec)` constructs it.

## The constructor — `func_020079A8`

```
  0x020079b4  mov    r1, #0x21
  0x020079bc  bl     #0x2004350          -> func_02004350
  0x020079c0  ldr    r2, [pc, #0x14]     = 0x0213b63c
  0x020079c8  add    r0, r5, #0x14
  0x020079cc  str    r2, [r5]
  0x020079d0  bl     #0x20ec6b4          -> func_020ec6b4
```

Three facts, all explicit:

- the tag is **0x21** (33), passed to the base constructor — this corroborates
  `WaitingToDieCommand` tag 0x21 rather than resting on the earlier note;
- the derived **vtable `0x0213B63C` is stored at `+0x00`**, which matches
  `docs/data/commands.tsv` exactly (`Battles::WaitingToDieCommand`, 5 slots,
  `networked=yes`);
- the entity vector is constructed **at `+0x14`**, from the caller's vector.

## The base — `func_02004350(obj, tag, sink)`

```
  0x02004354  ldr    r3, [pc, #0x34]     = 0x0213b090
  0x0200435c  str    r3, [r4]
  0x02004360  ldr    r0, [pc, #0x2c]     = 0x0213b0a0
  0x02004364  mov    r3, #0
  0x02004368  stm    r4, {r0, r3}
  0x0200436c  mov    r0, #2
  0x02004370  str    r0, [r4, #8]
  0x02004378  str    r1, [r4, #0xc]
  0x0200437c  bl     #0x20871f8          -> func_020871f8
  0x02004380  ldrh   r1, [r0]
  0x02004388  strh   r1, [r4, #0x10]
```

| offset | width | value |
|---|---|---|
| `+0x00` | word | `0x0213B090`, immediately replaced via `stm` by `0x0213B0A0`, then by the derived `0x0213B63C` |
| `+0x04` | word | `0` |
| `+0x08` | word | `2` |
| `+0x0C` | word | **the tag** (`0x21` here) |
| `+0x10` | **halfword** | `*(u16*)func_020871F8(sink)` |

**`+0x10` is written with `strh`, not `str`.** The existing note in this project
reads "player named by low nibble of `cmd+0x10`", which is consistent, but the
*width* is the part that has burned this project before — three wrong answers
came from matching an offset while getting the access width wrong. It is a
halfword. `func_020871F8` maps the sink to something whose first halfword is that
id; it is not yet read, and nothing below depends on knowing more than that.

## The vector — `func_020EC6B4(dst, src)`

```
  0x020ec6b8  ldr    r2, [pc, #0x30]     = 0x0214f8ac
  0x020ec6c0  str    r2, [r4]
  0x020ec6c4  mov    r2, #0
  0x020ec6c8  str    r2, [r4, #4]
  0x020ec6cc  str    r2, [r4, #8]
  0x020ec6d0  str    r2, [r4, #0xc]
  0x020ec6d4  ldr    r3, [r1, #0x10]
  0x020ec6dc  str    r3, [r4, #0x10]
  0x020ec6e0  strb   r2, [r4, #0x14]     ; r2 == 1 by now (mov r2,#1 at 0x20ec6d8)
  0x020ec6e4  bl     #0x20ec770          -> func_020ec770
```

Vector-relative (add `0x14` for command-relative):

| offset | width | value |
|---|---|---|
| `+0x00` | word | vtable `0x0214F8AC` |
| `+0x04` | word | `0` |
| `+0x08` | word | `0` |
| `+0x0C` | word | `0` |
| `+0x10` | word | copied from `src+0x10` — a stride/element-size carried over |
| `+0x14` | **byte** | `1` |

Note again a width that a guess would get wrong: `+0x14` is a `strb`.

`0x14 + 0x18 = 0x2C`, which is exactly the allocation size. **The vector occupies
the whole tail of the command and the layout closes.** That the two independently
disassembled sizes agree is the strongest single check available here.

## The element copy — `func_020EC770(dst, src)`

This is the read docs/241 was missing, and it names the three words outright.

```
  0x020ec77c  bl     #0x20ec7dc          -> func_020ec7dc
  0x020ec780  ldr    r1, [r4, #8]
  0x020ec788  str    r1, [r5, #8]        ; dst.count = src.count
  0x020ec78c  bl     #0x20eca5c          -> func_020eca5c   (reserve)
  0x020ec790  ldmib  r4, {r0, r2}        ; r0 = src[+4], r2 = src[+8]
  0x020ec794  ldr    r1, [r5, #4]        ; r1 = dst[+4]
  0x020ec798  lsl    r2, r2, #1          ; bytes = count * 2
  0x020ec79c  bl     #0x2110758          -> func_02110758   (block copy)
```

| vector offset | meaning |
|---|---|
| `+0x04` | **data pointer** (filled by the reserve at `func_020ECA5C`) |
| `+0x08` | **element count** |

And `lsl r2, r2, #1` says **each element is two bytes**. That is a satisfying
cross-check rather than a new assumption: the kill recipe packs its handle from
the entity's `+0xDC`/`+0xDD` — two bytes — so a `u16` element is exactly what the
handle array should hold.

So docs/241's proposed "container / data / count" triple was right in substance
for `+0x04`/`+0x08`. It is now measured rather than proposed.

One thing deliberately **not** claimed: the argument order of
`func_02110758(r0, r1, r2)`. `r0` is the *source* buffer and `r1` the
*destination*, which is the reverse of `memcpy(dst, src, n)`. It may be a
`(src, dst, n)` helper, or the roles may be otherwise. Nothing in this layout
depends on it, so it is left unread rather than assumed.

## The reserve — `func_020ECA5C(vec, needed)`, and the rest of the vector

The copy above calls a reserve before copying elements. Reading it names every
remaining field, so the vector is now fully accounted for rather than partly.

```
  0x020eca64  ldr    r0, [r4, #0xc]      ; capacity
  0x020eca6c  cmp    r8, r0, lsr #1
  0x020eca70  popls  {...}               ; enough room -> return
  0x020eca74  ldrb   r0, [r4, #0x14]     ; the BYTE at +0x14
  0x020eca7c  popeq  {...}               ; zero -> return WITHOUT growing
  0x020eca80  ldr    r6, [r4, #0x10]     ; element descriptor
  0x020eca8c  bl     #0x211fdb4
  0x020ecaa0  bl     #0x20eca50          ; growth multiplier
  0x020ecac8  ldr    r0, [r4, #4]        ; current buffer
  0x020ecacc  ldr    r2, [pc, #0x14]     = 0x0214f8d0   (file)
  0x020ecad4  mov    r3, #0xf5           (line 245)
  0x020ecad8  bl     #0x204caac          -> realloc
  0x020ecadc  str    r0, [r4, #4]        ; new buffer
  0x020ecae0  str    r5, [r4, #0xc]      ; new capacity
```

| vector offset | width | meaning |
|---|---|---|
| `+0x00` | word | vtable `0x0214F8AC` |
| `+0x04` | word | data pointer |
| `+0x08` | word | element count |
| `+0x0C` | word | **capacity** |
| `+0x10` | word | element descriptor / stride |
| `+0x14` | **byte** | **may-grow flag** — zero means reserve returns without reallocating |

Two things worth noting. The byte at `+0x14` is not a boolean decoration: it is
an explicit **"you may not reallocate this buffer"** switch that the game's own
reserve honours by returning early. And growth is a realloc through
`func_0204CAAC(ptr, size, file, line)`, the same debug-allocator family as the
`func_0204C6E4` that allocates the command itself.

That flag is the interesting part for a hand-built command. A vector pointed at a
buffer this port owns, with count 1, adequate capacity and `+0x14` set to **0**,
is one the game will never try to reallocate — the mechanism for that is already
in the shipped code and does not have to be invented.

### The flag gates freeing too — it is an OWNERSHIP flag

The obvious risk with pointing the vector at a port-owned buffer is teardown: if
the destructor frees whatever `+0x04` points at, the trick is a crash. So
`func_020EC728` was read. It is a named symbol — **`Battles::ArrayEntityRef::
virtual_00`** — which also names the class this vector is. It restores the vtable
and delegates to `func_020EC7E8`:

```
  0x020ec7f0  ldrb   r0, [r4, #0x14]     ; the same byte
  0x020ec7f4  cmp    r0, #0
  0x020ec7f8  beq    #0x20ec804          ; ZERO -> skip the free entirely
  0x020ec7fc  ldr    r0, [r4, #4]
  0x020ec800  bl     #0x204ca50          ; free(data)
  0x020ec804  mov    r0, #0
  0x020ec808  str    r0, [r4, #4]        ; then clear ptr/capacity/count
  0x020ec80c  str    r0, [r4, #0xc]
  0x020ec810  str    r0, [r4, #8]
```

**`+0x14` gates growth AND freeing.** It is not a "may grow" flag, it is an
**owns-the-buffer** flag, and the shipped code honours it on both paths. A vector
with `+0x14 = 0` is a non-owning view: the game will neither reallocate nor free
the buffer behind it. That is precisely the mechanism a hand-built command needs,
and it already exists rather than having to be contrived.

### What the redesign now costs

With the vector no longer needing any guest call — its six fields can be written
directly over a port-owned two-byte handle buffer — the chain reduces from the
five calls docs/241 measured to **three**:

1. `func_0204C6E4(0x2C, file, line)` — allocate the command. This one cannot be
   skipped: the command is handed to the game's list and the game destroys it
   afterwards, so it must come from the game's own allocator. A static port
   buffer would be freed by the game and is not an option.
2. `func_020871F8(slot)` — the unique serial for `+0x10`. Its collision loop
   (above) rules out imitating it with a plain increment.
3. `func_020EB910(cmd)` — the submit.

Everything between those is plain memory writes at known offsets and widths.

Three calls instead of five is not automatically a fix — docs/241's finding was
that *any* chain can lose the guest return to a slice yield at a backward branch,
and three is not zero. But it makes docs/241's second option, the resumable state
machine, markedly more tractable: three recorded steps rather than five, with no
intermediate object lifetimes to unwind, because the vector is no longer
constructed by the guest at all.

This is where the redesign stands: **grounded end to end, with no unread
dependency**, and reduced to a shape worth implementing and testing rather than
one blocked on unknowns.

## Implemented

`issue_waiting_to_die` in `lb_patches.cpp` now builds the vector itself. The last
value it needed came from reading the init it replaces, `func_020EC5A8`:

```
  0x020ec5b0  str    r3, [r0]        = 0x0214f8ac
  0x020ec5b4  str    r2, [r0, #4]    ; 0
  0x020ec5b8  str    r2, [r0, #8]    ; 0
  0x020ec5bc  str    r2, [r0, #0xc]  ; 0
  0x020ec5c0  mov    r2, #0x10
  0x020ec5c4  str    r2, [r0, #0x10] ; 0x10 -- a growth granularity, not a size
  0x020ec5c8  mov    r2, #1
  0x020ec5cc  strb   r2, [r0, #0x14] ; owns = 1
  0x020ec5d0  strb   r1, [r0, #0x15] ; a SECOND byte, from init's own argument
```

`+0x15` had not shown up before; it is the `1` the old call site passed as
`func_020EC5A8(&vec, 1)`. The port writes it too rather than leaving it whatever
the stack held.

The written vector is therefore: vtable `0x0214F8AC`, data pointing at the port's
own two-byte handle, count `1`, capacity `4`, `+0x10 = 0x10`, `+0x14 = 0`,
`+0x15 = 1`. Capacity 4 against a needed of 1 matters: reserve returns early when
`needed <= capacity>>1`, so even if something does reserve against this vector it
takes the no-growth path.

**The chain is now three guest calls — `ctx`, `construct`, `submit` — down from
six.** `kArrayInit`, `kArrayPush` and the trailing `kArrayFree` are all gone;
the free in particular is not merely unnecessary but was a sixth opportunity to
unwind. The three constants are kept in the source, marked unused-not-dead, since
they name the shipped recipe and the state-machine option would use them again.

**Not yet claimed: that this makes mode 1 work.** Halving the chain improves the
odds and removes two object lifetimes, but docs/241's finding was that *any*
chain can lose its return at a backward branch, and three is not zero. The panel
still says mode 1 behaves as Off, and that text stays until a run shows otherwise.

## Tested. It does not work, and the reason is sharper than "fewer calls is better"

Run `killtest1`, with `LB_KILLTEST=1` on the shipped Free Play route:

```
[lb_killtest] issuing WaitingToDie for 022E7914 (cat 0), pool=20 commands=29
[lb_killtest] armed; waiting for a guest call context (set_owner_override)
[lb_call]     UNWOUND at construct (02005B0C)
[lb_killtest] attempt 1 unwound; re-arming
[dispatch-miss] cpu=arm9 pc=0x02005B0C arm (lr=0x02005B0C)
```

The chain now gets **two calls further than before** — `ctx` completes, and the
unwind lands on `construct` rather than on the old `init`. So the reduction did
what it was built to do. It still does not finish, and the harness then died with
`debug server refused framebuffer: frontend busy or frozen`.

### Why shortening the chain has a floor

The dispatch-miss line invited a wrong conclusion — that `func_02005B0C` is not
lifted. It is. `port/config/lb_arm9.toml` declares it (`addr = 0x02005B0C`,
`size = 0x3C`, `mode = "arm"`, matching the disassembly exactly), the generated
dispatch table maps it at `lb_arm9_dispatch.c:74927` to
`lb_arm9_afunc_02005B0C`, and its 36-byte `NdsStaticValidation` record is
**byte-identical** to what the runtime dumped from live RAM
(`38 40 2D E9` = `E92D4038` = `push {r3,r4,r5,lr}`, and so on). Nothing about the
lifting or the validation is wrong.

Reading `runtime_dispatch` in `runtime_arm.cpp:1033` gives the real shape:

```c
extern "C" void runtime_dispatch(uint32_t target_pc) {
    for (;;) {
        ...
        // Slice-preemption point.
        if (runtime_slice_yield()) {
            g_cpu.R[15] = pc;
            return;                       // <-- BEFORE any lookup
        }
        ...
        if (guard_scope.call(lookup_static_cached(c, pc, thumb))) { ... }
```

**The yield is checked at the top of every dispatch, before the target is even
looked up.** docs/241 described the preemption as happening at backward branches;
that is true but incomplete. `runtime_dispatch` is *itself* a preemption point,
and its own comment says so — it exists to catch loops whose back-edge is an
indirect transfer.

The consequence is structural: **every guest call is a yield opportunity,
independent of what it calls or how long it runs.** Reducing six calls to three
reduces the number of chances from six to three; it cannot reduce it to zero, and
no further shortening will, because the last call is still a dispatch. This is why
docs/241's option 1 ("reduce it to one guest call") was never going to be
sufficient on its own, and the reading above is what shows that rather than
another failed run showing it.

The trailing dispatch-miss and the freeze are downstream of the yield, not a
separate fault: the yield leaves `R[15]` pointing at the constructor with a return
marker this port pushed, and the resume does not reconstitute that context.

### What is kept, and what is not claimed

The change stays. It is strictly better structurally — three yield opportunities
instead of six, no spurious free, and a vector whose every field and width is
measured — and it is a prerequisite for the state machine rather than an
alternative to it. Both entry points remain **opt-in dev flags**: the kill path
needs `LB_KILLPATH` (`lb_patches.cpp:474`) and the self-test needs `LB_KILLTEST`
(`lb_patches.cpp:2814`), so nothing a default user does reaches this code, and
the freeze above is confined to the opt-in path.

What is **not** claimed: that mode 1 works, or that it is closer to working in any
way a user would notice. The panel still says it behaves as Off, which remains
true. docs/241's option 2 — a resumable state machine that records which of the
three steps completed and continues on the next override entry — is now the only
option left standing, and the reason is a measurement rather than a preference.

Incidentally `func_0211FDB4`, called at `0x020ECA8C`, is one of the three
addresses in `dispatch_misses.log` — see the section at the end.

## The id at `+0x10` — `func_020871F8`

```
  0x020871fc  ldr    r5, [pc, #0x34]     = 0x02155418
  0x02087204  and    r6, r0, #0xf        ; low nibble of the argument
  0x02087208  ldrh   r0, [r5, #2]        ; the u16 at 0x0215541A
  0x0208720c  add    r0, r0, #0x10
  0x02087210  and    r0, r0, r4          ; r4 = 0xFFF0
  0x02087214  orr    r0, r0, r6
  0x02087218  strh   r0, [r5, #2]
  0x0208721c  bl     #0x20871ec
  0x02087228  cmp    r1, r0
  0x0208722c  beq    #0x2087208          ; collision -> bump again
  0x02087230  ldr    r0, [pc, #8]        = 0x0215541a
```

`+0x10` is a **unique command serial**: the top twelve bits are a counter at
`0x0215541A` bumped by 0x10, and the low nibble is the player slot taken from the
argument. The existing project note — "player named by low nibble of `cmd+0x10`"
— is correct, and this explains the other twelve bits it never accounted for.

The loop matters for the redesign. `func_020871EC` is consulted and the counter
is bumped **again on collision**, so this is not a free-running counter that can
be imitated by a single add: the game actively avoids reusing a live id. Copying
another command's id would therefore be wrong by construction.

## Where the redesign stands

Everything needed to lay out the object by hand is now measured:

- size `0x2C`, null allocation survivable;
- `+0x00` vtable `0x0213B63C`, `+0x04` `0`, `+0x08` `2`, `+0x0C` tag `0x21`;
- `+0x10` **halfword** unique serial, low nibble = player slot;
- vector at `+0x14`: vtable `0x0214F8AC`, `+0x04` data, `+0x08` count,
  `+0x14` **byte** `1`, elements two bytes wide;
- `0x14 + 0x18 = 0x2C`, so the layout closes against the allocation size.

Two field widths here would have been wrong under the obvious assumption —
`+0x10` is `strh` and vector `+0x14` is `strb`. This project has already lost
three answers to exactly that mistake, which is the reason this document quotes
the store instruction for every field rather than only the offset.

What remains is **allocation and id, not layout**. A hand-built command still
needs a buffer the game will not reclaim, and a serial from `func_020871F8`
(whose collision loop argues against imitating it). Whether that can be reduced
to the single guest call docs/241 wanted, or wants the resumable state machine it
listed second, is a design question this document does not settle.

Mode 1 therefore stays inert by default with the panel saying so. That is
unchanged — but it is now blocked on one decision rather than on missing
knowledge.

## A separate finding: two of these routines have no lifted entry point

`dispatch_misses.log` in the repo root is written by the runner when it reaches
an address the recompiler has no entry point for. It currently holds three
`runtime_confirmed` addresses, and two of them are in **exactly the routines this
document is about**:

```
# cpu=arm9 pc=0x0211FDB4 lr=0x020ECA90
# cpu=arm9 pc=0x020EC8F0 lr=0x020EC8DC
# cpu=arm9 pc=0x020ECA88 lr=0x020EC8F8
```

`0x020EC8DC` is `kArrayPush` — the call the kill recipe makes — and `0x020ECA90`
sits inside the reserve helper `func_020ECA5C` reached from the vector copy.

Checked against the config rather than assumed: `port/config/lb_arm9.toml` holds
**9157** entry points and declares `authoritative_entry_points = true`, so a
missing address is dropped rather than discovered. Of the three:

| address | in lb_arm9.toml |
|---|---|
| `0x0211FDB4` | yes |
| `0x020EC8F0` | **no** |
| `0x020ECA88` | **no** |

These are *runtime-confirmed* — observed executing, not inferred — so adding them
is evidence-backed rather than speculative. Whether that changes mode 1's
behaviour is untested and is NOT claimed here; docs/241 attributed the failure to
cooperative slice yields, and a dispatch miss is a different fault that could be
additional rather than alternative.

It is recorded and not acted on because adding entry points requires a **re-lift**
(`port/lift.sh`), not just a runner rebuild, which is a much larger operation than
anything else in this investigation. `dispatch_misses.log` itself is now gitignored —
it is regenerated every run, and the addresses, not the file, are the artefact
worth keeping.
