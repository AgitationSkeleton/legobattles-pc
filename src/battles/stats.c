/* Battles::Sim::MagicInfluencedUnitStats accessors.
 *
 * The four-getter block at 0x0205CBF4..0x0205CC0C, identified in docs/29.
 * Each is two instructions, so there is only one sensible encoding and the
 * compiler's choices do not enter into it -- which is exactly why these are
 * the right functions to start a matching decompilation with.
 */

#include "types.h"

/* func_0205CBF4 -- returns the address of the multiplier field, not its value. */
u32 *MagicInfluencedUnitStats_GetMultiplierPtr(MagicInfluencedUnitStats *self)
{
    return &self->multiplier;
}

/* func_0205CBFC */
u8 MagicInfluencedUnitStats_GetAttackInterval(MagicInfluencedUnitStats *self)
{
    return self->attack_interval;
}

/* func_0205CC04 */
u16 MagicInfluencedUnitStats_GetResourceYield(MagicInfluencedUnitStats *self)
{
    return self->resource_yield;
}

/* func_0205CC0C */
u16 MagicInfluencedUnitStats_GetSecondMultiplier(MagicInfluencedUnitStats *self)
{
    return self->second_multiplier;
}
