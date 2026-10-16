# 280 — Interior addresses are not entry points, and adding three clipped three functions

This retracts the entry-point half of commit `356cf6f7` ("lift three confirmed
misses"). The three addresses it added to `port/config/lb_arm9.toml` are not
function starts, and declaring them truncated the functions that contain them.

## What was added, and what those addresses actually are

| added entry | container | container size | what the instruction is |
|---|---|---|---|
| `0x02087360` | `0x02087350` `EntityAtTile` | `0x40` | `mov r1, r4` |
| `0x020EC8F0` | `0x020EC8DC` `kArrayPush` | `0x48` | `str r1, [r4, #8]` |
| `0x020ECA88` | `0x020ECA5C` | `0x90` | `mov r1, r6` |

None is preceded by a terminator; none is a branch target. `lb_code.py xref`
on `0x02087360` reports **0 literal-pool references and 0 direct branches**.
`0x020EC8DC` opens `push {r0,r1,r2,r3}` / `push {r4,lr}` and closes
`pop {r4,lr}` / `add sp,sp,#0x10` / `bx lr`, so `0x020EC8F0` is plainly inside
one function body, twenty bytes in.

## The damage, measured not inferred

The emitted validation records in `port/generated/lb_arm9_dispatch.c`:

```
{0x02087350u, 16u, ...}      declared 0x40, emitted 0x10
{0x020EC8DCu, 20u, ...}      declared 0x48, emitted 0x14
{0x020ECA5Cu, 44u, ...}      declared 0x90, emitted 0x2C
```

Each emitted length is exactly `split_addr - container_addr`. Declaring an
entry point inside a function clips the container at that address. For
comparison an untouched neighbour, `0x020872DC`, emits its full `44u` = `0x2C`.

So each container's `NdsStaticValidation` now covers only its first few
instructions. The gate in `lookup_in()` checks that live bytes still equal
lifted bytes over that record, so three functions lost most of their coverage.

## Why the premise was wrong

`runtime_dispatch_miss()` is not only called for unknown functions. It is the
`default:` arm of the **resume switch** that every lifted function carries
(`recompiler/src/main.cpp`, `emit_resume_switch` and the `emit_entry_switch`
block in `emit_function_body`). Both loops emit

```c
case 0x<interior_pc>u: goto L_<interior_pc>;
```

for every instruction address from `fn.addr + step` to `fn.end_addr`, above the
comment "a real computed branch into the function arrives at an interior PC and
jumps directly to that instruction."

**Therefore an interior address never needs its own entry point.** It is already
reachable, by construction, through its container's switch. A dispatch miss
reporting an interior PC means the container's `end_addr` did not reach it —
the fault is the container's extent, not a missing function.

docs/243 recorded the three addresses from `dispatch_misses.log` and checked
them against the config, finding two absent. That check was sound; the
inference drawn from it in `356cf6f7` — absent therefore must be added — was
not. An interior address is *correctly* absent from an entry-point list.

The log lines themselves show it:

```
# cpu=arm9 pc=0x0211FDB4 lr=0x020ECA90     <- function start, correct return
# cpu=arm9 pc=0x020EC8F0 lr=0x020EC8DC     <- interior pc, lr = its own container's start
# cpu=arm9 pc=0x020ECA88 lr=0x020EC8F8     <- interior pc, lr = return into kArrayPush
```

Only the first has the shape of a call. The other two have a valid LR with a
PC in the middle of a body, which is what resuming inside a function looks
like, not what calling one looks like.

## What is NOT claimed

* **That the split is currently breaking anything.** It is not, on the evidence
  available: a recorded full combat route
  (`demos/user-combat-2026-09-14.lbdemo`, 266 s) replays with **0 dispatch
  misses** on the current build. The lifted code is all still present — the
  tail simply became a second function reached by fallthrough, which the
  emitter supports (`literal_fallthrough` is a counted, normal path).
* **That removing the three entries is safe without a re-lift to prove it.**
  The original misses were real observations. If they return after removal, the
  correct fix is the container's `end_addr`, not an interior entry point.

The defect that is established is the validation coverage: 16, 20 and 44 bytes
where 64, 72 and 144 were declared.

## Consequence for docs/278

docs/278 built its direction on `func_0203ABC8` maintaining the camera record,
having found two `strh ...,[rN,#0x5E]` stores in one function and read that as a
two-armed clamp. Both halves of that are wrong and are retracted in that
document.
