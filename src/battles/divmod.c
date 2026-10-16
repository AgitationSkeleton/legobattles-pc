/* func_0211FDB4 -- the signed 32-bit divide-and-remainder the whole game uses.
 *
 * The ARM7TDMI has no divide instruction, so every `/` and `%` on a signed int
 * in this binary is a call here. 53 call sites, and a native port replaces the
 * whole thing with the host's own division -- but only once its edge cases are
 * known, because two of them are not what C does.
 *
 * SIGNATURE, from the code rather than from a convention: R0 = numerator,
 * R1 = denominator, and it returns BOTH -- quotient in R0, remainder in R1.
 * Grading only R0 would pass a reimplementation whose remainder was wrong at
 * every single call.
 *
 * THE ALGORITHM, read with lb_code.py dis:
 *
 *     0211fdb4  eor   ip, r0, r1
 *     0211fdb8  and   ip, ip, #0x80000000   ; bit31 <- sign of the quotient
 *     0211fdbc  cmp   r0, #0
 *     0211fdc0  rsblt r0, r0, #0            ; r0 <- |a|
 *     0211fdc4  addlt ip, ip, #1            ; bit0  <- sign of the remainder
 *     0211fdc8  cmp   r1, #0
 *     0211fdcc  rsblt r1, r1, #0            ; r1 <- |b|
 *     0211fdd0  beq   0211ffac              ; b == 0 -> straight to the fixup
 *     0211fdd4  cmp   r0, r1
 *     0211fdd8  movlo r1, r0                ; |a| < |b| (UNSIGNED): r = a
 *     0211fddc  movlo r0, #0                ;                       q = 0
 *     0211fde0  blo   0211ffac
 *     ...                                   ; restoring division, q in r0
 *     0211ffa8  mov   r1, r3                ; remainder
 *     0211ffac  ands  r3, ip, #0x80000000
 *     0211ffb0  rsbne r0, r0, #0            ; apply the quotient's sign
 *     0211ffb4  ands  r3, ip, #1
 *     0211ffb8  rsbne r1, r1, #0            ; apply the remainder's sign
 *     0211ffbc  bx    lr
 *
 * TWO EDGE CASES THAT ARE NOT C, and both are why this is written with
 * unsigned arithmetic instead of the host's `/` and `%`:
 *
 *   - DIVISION BY ZERO DOES NOT TRAP. The `beq` at 0211fdd0 reads the flags
 *     from `cmp r1,#0` two instructions earlier (`rsblt` has no S), so b == 0
 *     jumps to the fixup with r0 still holding |a| and r1 holding 0. The sign
 *     bits then restore a's sign, so the result is **q = a, r = 0**. In C that
 *     is undefined behaviour and on x86 it is a fault, so a port that writes
 *     `a / b` changes a defined behaviour into a crash.
 *
 *   - INT_MIN IS NOT NEGATED. `rsblt r0, r0, #0` on 0x80000000 overflows back
 *     to 0x80000000, and everything after it is unsigned, so |INT_MIN| is
 *     treated as 2147483648. `-2147483648 / -1` therefore yields 1 with the
 *     sign bits clearing, not the overflow C leaves undefined.
 *
 * Both are reproduced here by doing the magnitude work in u32 and applying the
 * signs exactly where the ROM does.
 */

#include "mem.h"

/* func_0211FDB4. `a` and `b` are the raw register words; the caller's ints are
 * reinterpreted rather than converted, which is what the ROM sees. */
void Battles_DivMod(u32 a, u32 b, u32 *q_out, u32 *r_out)
{
    u32 sign = (a ^ b) & 0x80000000u;   /* ip */
    u32 q, r;

    if ((s32)a < 0) {
        a = 0u - a;
        sign += 1u;                     /* bit 0: the remainder is negative */
    }
    if ((s32)b < 0) {
        b = 0u - b;
    }

    if (b == 0u) {
        /* The fixup is reached with r0 = |a| and r1 = |b| = 0. */
        q = a;
        r = 0u;
    } else if (a < b) {                 /* UNSIGNED, as `movlo` is */
        q = 0u;
        r = a;
    } else {
        q = a / b;                      /* u32: the restoring division */
        r = a % b;
    }

    if (sign & 0x80000000u) q = 0u - q;
    if (sign & 1u)          r = 0u - r;

    *q_out = q;
    *r_out = r;
}
