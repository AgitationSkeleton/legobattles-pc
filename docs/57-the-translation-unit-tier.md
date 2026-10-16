# The translation unit tier

**Status: annotation moves from 42.6% to 53.3% — 995 new attributions from a
single rule, whose precision is measured at 96.7% on a 1,375-case hold-out
rather than asserted.** 67 of 67 checks pass. Two of them re-run that hold-out,
so the number in this document cannot drift away from the number the tool has.

`docs/56` closed the data segment: 99.4% of files, and the 24 that remain need a
Mobiclip decoder rather than a reading. The next segment by closeness is
annotation, which had sat at 42.6% since `docs/43`.

## The rule

A compiler emits one translation unit's functions as a contiguous run. So a
function with no evidence of its own still sits **between** two functions whose
class is known:

```
0x02029070   func_02029070          ?
0x0202910C   ~ParticleDataEffect    Battles::ParticleDataEffect
0x0202914C   func_0202914c          ?      <- bracketed
0x0202918C   func_0202918c          ?      <- bracketed
...
0x0202994C   ~ParticleDataEffect    Battles::ParticleDataEffect
```

If the two nearest attributed functions agree, everything between them is one
file's worth of code. That is tier L.

It is an **association, not a name** — the same category as tiers A and C. It
says which file a function lives in. `docs/30` has separated names from
associations from the beginning, and this tier is why that separation was worth
building: it adds 995 attributions and **zero** names.

## Why it is measured and not asserted

This is the weakest claim in the annotation stack and the largest tier by a wide
margin, which is exactly the combination that produces a quietly wrong number.
`docs/36` and `docs/39` both had to retract a coverage figure that was computed,
plausible and wrong.

So the rule is tested against the evidence it is derived from. Hide each anchor
in turn, apply the rule to the hole, and see whether it recovers the class that
was there:

```
2,454 anchors -- functions the corpus already owns a class for
1,375 have two agreeing neighbours once hidden
1,330 recovered correctly                          96.7% exact class
   45 wrong
```

## Where the 45 misses land, which matters more than the rate

```
27  name a class that shares a name stem with the true one
15  name a class in the same inheritance family, per the RTTI
 3  name an unrelated class                     0.22% of all cases
```

The 27:

```
FireBallAnimation      -> FireBallEffect
CrabSwarmAnimation     -> CrabSwarmEffect
LightningBoltAnimation -> LightningBoltEffect
MonkeySwarmAnimation   -> MonkeySwarmEffect
RoarAnimation          -> RoarEffect
UpgradeAnimation       -> UpgradeEffect
CustomizeArmy          -> CustomizeArmyVramTransferCallback
StoreMiniFigs          -> StoreMiniFigsVramTransferCallback
```

Every spell in this game is two classes — the effect and its animation — and
they share a source file. Several screens are a class plus its own
`VramTransferCallback`. The 15 inheritance cases are the same story with a
different tell: `CampaignMissionBase` against
`CampaignMission1_CriminalMindsAct2`, `EntityCommandBase` against
`AssistStructureEntityCommand`.

**A rule that says "wrong file" and a rule that says "right file, wrong one of
the two classes in it" are not the same rule.** This is the second one. The
tier's actual claim — *this function is in the translation unit of class X* — is
wrong in 3 of 1,375 cases that the evidence can detect.

Those three are listed rather than hidden:

```
0x0200F234  Battles::Vector3u8   -> Battles::ArrowVolleyEffect
0x020528F8  Sim::Unit            -> Sim::ConstructProgressAction
0x02057414  Sim::Continent       -> Sim::SidestepAvoidancePlotter
```

All three are a small utility class emitted inside a larger file's run —
`Vector3u8` is three functions of vector arithmetic that landed in the middle of
the spell effects. That is a real thing compilers do and the rule cannot see it.

## A negative result

**Adding tiers V, A and C to the anchor set makes this worse on both axes.**
More anchors sounds like more evidence:

```
anchor set             anchors  cases  exact   unrelated  net new
corpus owners only       2,454  1,375  96.7%    3 (0.22%)   995
owners + tiers V/A/C     3,215  1,956  96.2%   21 (1.07%)   938
```

Precision falls, unrelated misses go up sevenfold, and the yield *drops* —
because the added anchors were functions tier L would otherwise have claimed.
Derived attributions are not the same currency as the corpus's own, and stacking
them compounds their errors. **The corpus's class ownership is the anchor set to
use**, and this is recorded so it is not retried.

## A tier that could NOT be validated, and why that is the finding

The obvious follow-on was tier C round two: tier C attributes a function to a
class when **every** caller is a method of that class, and it only counts
callers whose class the corpus owns. Now that tiers V, A, C and L supply classes
for 2,460 more functions, running it again would attribute **279** more.

Before adopting them, the same hold-out was applied to tier C. It came back
**0.0% exact**, on 18 cases.

That is not a verdict on tier C. It is a verdict on the test:

```
corpus anchors with any direct caller:  51 of 2,454   (2%)
```

**98% of the labelled functions are virtual methods and are never called
directly** -- they are reached through a vtable, which is exactly why the RTTI
knew their class in the first place. The labelled set and the population tier C
targets (non-virtual helpers) barely intersect, so the 18 cases that do overlap
are not a sample of anything.

And they are all one thing. Thirteen of the eighteen are a **base-class method
called by a derived class's methods**:

```
SidestepAvoidancePlotter        <- callers all in AstarPlotter
WaitForObstaclePlotter          <- callers all in SidestepAvoidancePlotter
SegmentedVectorPlotter          <- callers all in WaitForObstaclePlotter
MoveProjectileBaseEntityCommand <- callers all in MoveProjectileSpellEntityCommand
Sim::Entity                     <- callers all in Sim::Bridge
```

"Every caller is a method of X" is true, and the callee belongs to X's *base*.
The rule identifies the caller's class correctly and says nothing about the
callee's.

**So the 279 were not adopted.** A tier that cannot be measured is not the same
as a tier that measures badly, and neither is a reason to ship it. Tier L could
be validated because its population and the anchor population are the same kind
of thing -- functions laid out in a file. Tier C's cannot be, and the honest
report is that its 313 existing attributions rest on the argument in `docs/40`
rather than on a measurement.

Two other candidates were measured and set aside in the same way:

* **Function-pointer tables.** 368 runs of four or more consecutive function
  pointers, 3,958 entries, 2,647 distinct functions -- but only **213** are not
  already annotated, because 354 of the 368 runs are vtables the RTTI already
  covers. Not worth a tier.
* **Overlay functions.** 152 of the corpus's functions live in overlays and none
  carry a symbol. Each overlay is a mission (`docs/06`), so "belongs to overlay
  N" is a real association -- but it is 152 functions and a different kind of
  claim, so it is left for a pass that can do it properly.

## What it found, checked against a pass that did it by hand

`docs/56` spent twenty minutes reading the particle loader and identified
`Battles::ParticleDataEffect` from a vptr at one allocation site. Tier L, which
knows nothing about particles, put **19 functions of that module** in
`ParticleDataEffect`'s translation unit — including `func_0202914C` and
`func_02029DAC`, the two `docs/56` read by hand.

The largest gains:

| class | functions |
| --- | ---: |
| `User::WorldViewScreen` | 50 |
| `Sim::FogCircle` | 39 |
| `Sim::Entity` | 24 |
| `Sim::CommandBase` | 24 |
| `User::WorldViewTopScreen` | 22 |
| `Battles::SpellIconsMiningEffect` | 21 |
| `Battles::ParticleDataEffect` | 19 |

`Sim::FogCircle` gaining 39 functions is the fog-of-war implementation arriving
in one piece. `WorldViewScreen` at 50 is the main gameplay screen.

## The annotation stack now

```
2,501  listed        26.9%   of which 2,454 free from the shipped RTTI
  135  tier S        named from a sole string reference
  235  tier F        C++ static-object initialisers
  334  tier T        tail-call thunks
  244  tier V        constructors, by the vptr they assign
  204  tier A        constructs a class, inlined at several sites
  313  tier C        every caller is a method of one class
  995  tier L        the translation unit it sits in
-----
4,961  of 9,314      53.3%

948 are NAMES.  1,512 are ASSOCIATIONS.
```

The name/association split is the honest headline. **Just under a thousand
functions have something like a name; five times that many only have a
neighbourhood.** Reporting 53.3% without that split would be the exact failure
`docs/30` was written to prevent.

## Tips and pitfalls

* **Test a heuristic against the evidence it came from.** The anchors are a
  free labelled set; a rule that cannot recover them is not worth running on
  the unlabelled ones.
* **Classify the misses, do not just count them.** 96.7% and 96.7% are the same
  number whether the misses are one class away or in a different subsystem, and
  those are completely different tools.
* **More anchors is not more evidence** when the new anchors are themselves
  derived.
* **Check that the hold-out population is the population the rule targets.**
  Tier C's test returned 0% not because the rule is wrong but because 98% of the
  labelled functions are virtual and never called directly. A validation set has
  to be a sample of the thing being predicted.
* A tier that produces no names is still worth having, provided the metric it
  feeds keeps names and associations apart.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| data decoded, by byte | **71.4%** | 86.6 MB of 121.2 MB |
| **ARM9 annotated** | **53.3%** | 2,501 listed + 2,460 derived, hold-out measured |
| decompilation | 0.157% | 377 functions byte-verified |

Annotation is now the second-least-complete segment after decompilation, and
4,353 functions still carry nothing at all.
