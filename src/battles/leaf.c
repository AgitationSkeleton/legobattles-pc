/* Verified leaves: small, pure, self-contained functions from the tranche
 * docs/332 sized. Each is read out of the image with `lb_code.py dis` and the
 * disassembly is reproduced above it, because the decompiler corpus is a hint
 * and not a source (its own header records Ghidra dropping call arguments at
 * 169 of 170 sites for one checked function).
 *
 * NAMED FOR WHAT THEY COMPUTE, not for what they are probably for. None of
 * these three is a vtable slot, so the ARM9's RTTI gives no class name for
 * them, and inventing a purpose would put a guess in a symbol that later work
 * would read as established. The same rule types.h already applies to fields.
 */

#include "mem.h"

/* ── func_020EF1E0 ─────────────────────────────────────────────────────────
 *
 *     020ef1e0  ldr   r3, [r0, #0x24]      ; n = inline count
 *     020ef1e4  cmp   r1, r3
 *     020ef1e8  addlt r0, r0, #4           ; i < n: &inline[0]
 *     020ef1ec  addlt r0, r0, r1, lsl #1   ;        + i*2
 *     020ef1f0  ldrge r2, [r0, #0x2c]      ; i >= n: the spill block
 *     020ef1f4  subge r0, r1, r3           ;         i - n
 *     020ef1f8  addge r0, r2, r0, lsl #1   ;         + (i-n)*2
 *     020ef1fc  bx    lr
 *
 * A u16 array with a small-buffer optimisation: the first `n` elements sit
 * inline at obj+4, the rest in a heap block whose pointer is at obj+0x2C. It
 * returns the ADDRESS of element `i`, not the element.
 *
 * THE COMPARISON IS SIGNED. `addlt` is LT, not LO, so a negative index takes
 * the inline branch and returns obj+4+i*2 -- an address BEFORE the object.
 * That is the ROM's behaviour and it is reproduced rather than corrected; a
 * bounds check here would be a different function.
 */
gptr Battles_SplitArrayElem(gptr obj, u32 index)
{
    s32 i = (s32)index;
    s32 n = (s32)B_R32(obj + 0x24u);

    if (i < n) {
        return obj + 4u + (u32)(i * 2);
    }
    return B_R32(obj + 0x2Cu) + (u32)((i - n) * 2);
}

/* ── func_0206AC2C ─────────────────────────────────────────────────────────
 *
 *     0206ac2c  ldr  r0, [r0, #0xc]     ; node = head
 *     0206ac30  cmp  r0, #0
 *     0206ac34  beq  0206ac50
 *     0206ac38  ldr  r2, [r0, #0xc]     ; node->key
 *     0206ac3c  cmp  r1, r2
 *     0206ac40  bxeq lr                 ; hit -> the node itself
 *     0206ac44  ldr  r0, [r0, #4]       ; node = node->next
 *     0206ac48  cmp  r0, #0
 *     0206ac4c  bne  0206ac38
 *     0206ac50  mov  r0, #0
 *     0206ac54  bx   lr
 *
 * The same singly-linked search as func_02016A00 over a different object: head
 * at obj+0x0C, key at node+0x0C, successor at node+0x04. Unlike the registry
 * there is NO sentinel test -- every key value is looked up, including -1.
 */
gptr Battles_ListFindByKey(gptr obj, u32 key)
{
    gptr node = B_R32(obj + 0x0Cu);

    while (node != 0u) {
        if (B_R32(node + 0x0Cu) == key) {
            return node;
        }
        node = B_R32(node + 0x04u);
    }
    return 0u;
}

/* ── func_0205E638 ─────────────────────────────────────────────────────────
 *
 *     0205e638  ldr   r1, [r0, #0x160]
 *     0205e63c  ldr   r0, [pc, #0x14]    = 0x0000FFFF
 *     0205e640  ldr   r1, [r1, #8]
 *     0205e644  ldrh  r1, [r1, #0x66]
 *     0205e648  cmp   r1, r0
 *     0205e64c  movne r0, #1
 *     0205e650  moveq r0, #0
 *     0205e654  bx    lr
 *
 * Two pointer hops and one halfword compared against 0xFFFF, returning 1 or 0.
 * 0xFFFF is the "no value" sentinel this binary uses in several places -- the
 * same one func_02016A00 refuses before it touches memory -- so this reads as
 * "is that field set", and the name says no more than the code does.
 *
 * THE LOAD IS A HALFWORD. `ldrh` zero-extends, so the compare is against the
 * low 16 bits only; reading it as a u32 would never equal 0xFFFF for a field
 * whose upper half holds anything, and the function would always return 1.
 */
u32 Battles_IndirectU16IsSet(gptr obj)
{
    gptr p = B_R32(obj + 0x160u);
    p = B_R32(p + 0x08u);
    return B_R16(p + 0x66u) != 0xFFFFu ? 1u : 0u;
}
