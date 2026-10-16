/* Battles::Sim::CollectableItem accessors.
 *
 * The block at 0x02000C6C..0x02000D30, identified in docs/27. Each reaches
 * the entity's blueprint through `Entity+0x08` and reads one field, so the
 * generated code is a load pair and a return.
 */

#include "types.h"

/* func_02000C6C -- the unlock slot, docs/22 */
u16 CollectableItem_GetUnlockSlot(CollectableItem *self)
{
    return self->blueprint->unlock_slot;
}

/* func_02000D00 -- the polymorphic reference field, docs/27 */
u32 CollectableItem_GetRef(CollectableItem *self)
{
    return self->blueprint->ref;
}
