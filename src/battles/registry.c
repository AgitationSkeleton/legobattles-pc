/* Battles::RegisteredCreateInfo -- the registry lookup at func_02016A00.
 *
 * THE FIRST FUNCTION OF SUBSTANCE IN THIS DECOMPILATION. Everything matched
 * before it is a two- or three-instruction accessor (docs/332: 785 generated
 * getters plus four hand-written ones, 32 bytes in total). This one has a
 * loop, an early exit and a sentinel, and it is graded by behaviour rather
 * than by byte equality -- see docs/333 for why that is the only metric
 * available and how the A/B is run.
 *
 * WHAT IT IS. A singleton at 0x0215111C holds a singly linked list at +0x40.
 * Each node carries its key at +0x24 and its successor at +0x04. The function
 * returns the first node whose key matches, or 0.
 *
 * WHERE THE NAME COMES FROM. 17 functions load the literal 0x0215111C out of
 * their pools, all of them in the 0x020163D0..0x02016F50 cluster, and the last
 * of them is named by the ARM9's own RTTI: Battles::RegisteredCreateInfo
 * ::virtual_01. So the list is the create-info registry and these are its
 * operations. The class name is ground truth; the method name is not, and this
 * function is therefore named for what it does.
 *
 * READ OUT OF THE IMAGE, not out of Ghidra. `lb_code.py dis 0x02016a00`:
 *
 *     02016a00  mvn   r1, #0              ; r1 = -1
 *     02016a04  cmp   r0, r1
 *     02016a08  moveq r0, #0
 *     02016a0c  bxeq  lr                  ; key == -1 -> 0, without touching RAM
 *     02016a10  ldr   r1, [pc, #0x2c]     ; = 0x0215111C
 *     02016a14  ldr   r2, [r1, #0x40]     ; node = head
 *     02016a18  cmp   r2, #0
 *     02016a1c  beq   0x2016a3c
 *     02016a20  ldr   r1, [r2, #0x24]     ; node->key
 *     02016a24  cmp   r0, r1
 *     02016a28  moveq r0, r2
 *     02016a2c  bxeq  lr                  ; hit -> the node itself
 *     02016a30  ldr   r2, [r2, #4]        ; node = node->next
 *     02016a34  cmp   r2, #0
 *     02016a38  bne   0x2016a20
 *     02016a3c  mov   r0, #0
 *     02016a40  bx    lr
 *     02016a44  .word 0x0215111C          ; the literal, inside the 0x48 body
 *
 * Two details that a transcription of the decompiler output would have lost,
 * and both change behaviour:
 *
 *   - THE -1 GUARD RUNS BEFORE THE HEAD IS LOADED. A key of -1 returns 0 even
 *     when the singleton is not yet constructed. Reordering it after the load
 *     would fault on a null holder during startup.
 *   - THE SENTINEL IS -1, NOT 0. Key 0 is a legitimate key and is looked up
 *     like any other; only 0xFFFFFFFF is refused. `if (!key) return 0` would
 *     be wrong in a way no test that never asks for key 0 would catch.
 *
 * The body is 0x48 bytes and the last word of it is the literal pool, so the
 * instruction count is 0x44 = 68 bytes, which is what the inventory records.
 */

#include "mem.h"

/* The singleton, from the literal pool at 0x02016A44. */
#define REGISTRY_SINGLETON 0x0215111Cu
#define REGISTRY_HEAD      0x40u   /* singleton + 0x40 -> first node */
#define NODE_NEXT          0x04u
#define NODE_KEY           0x24u

/* func_02016A00 */
gptr Battles_RegistryFind(u32 key)
{
    gptr node;

    if (key == 0xFFFFFFFFu) {
        return 0u;
    }

    node = B_R32(REGISTRY_SINGLETON + REGISTRY_HEAD);
    while (node != 0u) {
        if (B_R32(node + NODE_KEY) == key) {
            return node;
        }
        node = B_R32(node + NODE_NEXT);
    }
    return 0u;
}
