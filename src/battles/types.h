/* Shared types for the LEGO Battles decompilation.
 *
 * Only structures whose layout has been RECOVERED FROM THE ROM appear here,
 * and each field cites the document that established it. A field nobody has
 * read is named `unk_NN` and left as raw bytes -- guessing a type here would
 * silently change the code the compiler emits, which is the one thing a
 * matching decompilation cannot tolerate.
 */

#ifndef BATTLES_TYPES_H
#define BATTLES_TYPES_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;

/* Battles::CollectableItemBlueprint -- 0x70 bytes. See docs/11, docs/22, docs/27. */
typedef struct CollectableItemBlueprint {
    u8  unk_00[0x5C];
    u8  category;        /* +0x5C  docs/22: eleven values                    */
    u8  unk_5D[3];
    u32 ref;             /* +0x60  docs/27: polymorphic, meaning per category */
    u8  unk_64[8];
    u16 unlock_slot;     /* +0x6C  docs/22: 0..288 over five categories      */
    u8  unk_6E[2];
} CollectableItemBlueprint;

/* Battles::Sim::CollectableItem -- the entity. Only +0x08 is established. */
typedef struct CollectableItem {
    u8 unk_00[8];
    CollectableItemBlueprint *blueprint;   /* +0x08  docs/25 */
} CollectableItem;

/* Battles::Sim::MagicInfluencedUnitStats -- see docs/29.
 *
 * The buffable subset of a unit's numbers. Only four values are copied out of
 * the blueprint into this block; everything else the simulation reads straight
 * from the blueprint.
 */
typedef struct MagicInfluencedUnitStats {
    u8   unk_00[4];
    void *entity;             /* +0x04 */
    u32  armour_like;         /* +0x08  seeded from the team side, 20.12      */
    u16  melee_damage;        /* +0x0C  from UnitBlueprint+0x68               */
    u16  unk_0E;
    u32  multiplier;          /* +0x10  20.12, defaults to 1.0 (0x1000)       */
    u8   attack_interval;     /* +0x14  from UnitBlueprint+0x6D, ticks        */
    u8   unk_15;
    u16  resource_yield;      /* +0x16  from UnitBlueprint+0x6C               */
    u16  second_multiplier;   /* +0x18  seeded 1.0 in Recompute               */
} MagicInfluencedUnitStats;

#endif /* BATTLES_TYPES_H */
