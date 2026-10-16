# Collectables, and the 289-slot unlock space

`Battles::CollectableItemBlueprint` is the largest of the three blueprint types
— **342 of the 551 records** — and it turns out to hold the game's entire
collectible and unlockable system, not just pickups.

## The category enum

`+0x5C`, exactly where `UnitBlueprint` keeps its own category. Eleven values,
each holding one coherent kind and nothing else:

| id | category | count | examples |
| ---: | --- | ---: | --- |
| 0 | HealthPowerup | 1 | `HealthPowerup` |
| 1 | ManaPowerup | 1 | `ManaPowerup` |
| 2 | Hint | 21 | `Hint_1` … `Hint_21` |
| 3 | MissionObject | 28 | `CW1_1_Bones`, `CW1_3_Scroll`, `MH1_2_CargoPods`, `PP_Crate` |
| 4 | ConceptArt | 66 | `ConceptArt_Character1_C` … |
| 5 | Map | 30 | `Map_01` … `Map_30` |
| 7 | RedBrick | 18 | `RedBrick_FastMining`, `RedBrick_DoubleStuds`, `RedBrick_IncreaseSpellRange` |
| 8 | Ticket | 1 | `Ticket` |
| 9 | Minikit | 90 | `Minikit_K1_1` … |
| 10 | Stud | 1 | `BlueStud` |
| 11 | Minifig | 85 | `Minifig_K_King`, `Minifig_K_Engineer`, … |

Category 6 is unused.

## `+0x6C` is a global unlock slot

The five collectable categories — ConceptArt, Map, RedBrick, Minikit and
Minifig — hold **289 records between them, and their `+0x6C` values are
0 … 288: 289 distinct values, perfectly contiguous, with no gaps and no
collisions.** Every record in the other six categories reads 0.

The space is allocated in blocks:

| slots | category | count |
| --- | --- | ---: |
| 0 – 29 | Map | 30 |
| 30 – 95 | ConceptArt | 66 |
| 96 – 180 | Minifig | 85 |
| 181 – 198 | RedBrick | 18 |
| 199 – 288 | Minikit | 90 |

That is the **save file's unlock bitfield index** — 289 bits, one per unlockable,
which is what `Battles::Save::BitStream` and `Battles::Save::PlayerData`
(`docs/02`) exist to write. A port that wants everything unlocked sets 289 bits
and needs no other knowledge.

A contiguous 0…N-1 covering exactly the union of five disjoint categories is not
something that happens by coincidence; it is what makes this a conclusion rather
than a reading.

## The minikits confirm the campaign table

Ninety minikits, named `Minikit_<side><act>_<n>`, distributed:

| | act 1 | act 2 | act 3 |
| --- | ---: | ---: | ---: |
| King | 5 | 6 | 4 |
| Wizard | 6 | 5 | 4 |
| Pirate | 5 | 5 | 5 |
| Imperial | 5 | 5 | 5 |
| Earth | 6 | 5 | 4 |
| Alien | 5 | 5 | 5 |

Now compare `LOC` 199–216, the achievement strings, read in `docs/07`'s pass:

```
5/5 Minikits - King Act 1        6/6 Minikits - King Act 2        4/4 Minikits - King Act 3
6/6 Minikits - Wizard Act 1      5/5 Minikits - Wizard Act 2      4/4 Minikits - Wizard Act 3
6/6 Minikits - Gemma Act 1       5/5 Minikits - Gemma Act 2       4/4 Minikits - Gemma Act 3
5/5 … Alien Commander …          5/5 …                            5/5 …
5/5 … Captain Brickbeard …       5/5 …                            5/5 …
5/5 … Governor Broadside …       5/5 …                            5/5 …
```

**Every one of the eighteen counts matches**, from two files that share no
structure — a text table and a binary blueprint. It also settles a small naming
question: `LOC` calls the Earth campaign's hero **Gemma**, and her act counts
(6/5/4) are the `E` prefix's, confirming `docs/08`'s reading of `mh` as the
Earth campaign from a third direction.

## A one-line bug worth recording

The first version of the parser wrote `"unlock_slot": u16(0x6C) or None`, and
reported **288** unlockables instead of 289. `Map_01` occupies slot **0**, and
`0 or None` is `None`.

The count was one short of a number that had just been established as exactly
289 and contiguous, so it was visible immediately — but only because the
contiguity check was already there to disagree with it. A field whose valid
range includes zero must be gated on *whether the field applies*, not on its
value. It now checks the category.
