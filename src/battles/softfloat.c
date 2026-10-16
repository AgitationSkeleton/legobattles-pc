/* The single-precision soft-float routines, and the case for deleting them.
 *
 * The ARM7TDMI and the ARM946E-S in the DS have no floating-point unit, so
 * every `float` operation in LEGO Battles is a call into hand-written IEEE-754
 * code. Three of them are here:
 *
 *     func_0211F324   a + b      540 bytes    __addsf3
 *     func_0211F934   a - b      608 bytes    __subsf3
 *     func_0211F748   a * b      480 bytes    __mulsf3
 *
 * IDENTIFIED FROM THE BIT MANIPULATION, not from a symbol. The multiply opens
 *
 *     0211f748  eor  r2, r0, r1               ; sign = sign(a) ^ sign(b)
 *     0211f74c  and  r2, r2, #0x80000000
 *     0211f750  mov  ip, #0xff
 *     0211f754  ands r3, ip, r0, lsr #23      ; exponent of a
 *     0211f758  lsl  r0, r0, #8               ; mantissa to the top
 *     0211f75c  cmpne r3, #0xff               ; Inf/NaN?
 *     0211f760  beq  0211f7c4
 *     0211f764  orr  r0, r0, #0x80000000      ; restore the implicit 1
 *
 * -- a 23-bit mantissa, an 8-bit exponent biased so 0xFF is Inf/NaN, and a
 * sign in bit 31. That is binary32 and nothing else.
 *
 * THE ADD AND THE SUBTRACT ARE ONE ROUTINE WITH TWO DOORS. The add at
 * 0211F324 begins `eors r2, r0, r1 / eormi r1, r1, #0x80000000 / bmi
 * 0211F940` -- differing signs means the magnitudes must be subtracted, so it
 * flips b and jumps into the SUBTRACT's body. The subtract at 0211F934 does
 * the mirror image, jumping to 0211F330 inside the add. Both entry points are
 * dispatched, and neither interior target is, which is one more reason the
 * grading harness observes rather than replaces (docs/333): replacing the add
 * would leave the subtract's jump running the original code.
 *
 * WHY THIS IS WORTH GRADING RATHER THAN TRANSCRIBING. A native port on a
 * machine with an FPU should call `a + b`, not carry 1,628 bytes of software
 * mantissa alignment. The only question is whether the host's binary32 agrees
 * with this code on the values the game actually produces, and that is a
 * measurement, not an argument. The reimplementations below ARE `a + b`,
 * `a - b` and `a * b`; the harness compares them against the ROM over every
 * call a real session makes.
 *
 * WHAT A PASS WOULD AND WOULD NOT SETTLE. Agreement over a replay covers the
 * values that replay produced. It does not cover denormal inputs the route
 * never generated, signalling NaNs, or a rounding mode the game never selects
 * -- and x86 flushes denormals only when told to, while this code handles them
 * in software. A disagreement on those is expected and is information, not a
 * failure of the port: it names exactly which cases need the software path.
 */

#include "mem.h"

/* Bits in, bits out, reinterpreted rather than converted -- the ROM's callers
 * pass raw register words and a C `float` parameter would let the compiler
 * canonicalise a NaN on the way in. */
static float bits_to_f(u32 b)
{
    union { u32 u; float f; } c;
    c.u = b;
    return c.f;
}

static u32 f_to_bits(float f)
{
    union { u32 u; float f; } c;
    c.f = f;
    return c.u;
}

/* THE ROM HAS ONE NaN AND IT IS NOT THE HOST'S.
 *
 * Measured by the synthetic sweep (docs/333), not assumed. Over the full
 * 22x22 grid of interesting bit patterns:
 *
 *     Battles_FMul/sweep   graded 484   disagreed 92
 *     Battles_FAdd/sweep   graded 484   disagreed 86
 *
 * and every sampled disagreement is the same shape:
 *
 *     FMul(00000000, 7F800000)  cartridge 7FFFFFFF   host FFC00000
 *     FMul(00000000, 7FC00000)  cartridge 7FFFFFFF   host 7FC00000
 *     FMul(00000000, 7F800001)  cartridge 7FFFFFFF   host 7FC00001
 *     FSub(00000000, 7FC00000)  cartridge 7FFFFFFF   host 7FC00000
 *
 * The cartridge returns **0x7FFFFFFF for every NaN result** -- sign clear,
 * exponent all ones, mantissa all ones -- and does not propagate an input
 * NaN's payload or its sign. x86 returns the canonical quiet NaN 0x7FC00000,
 * or the incoming payload, or a negative NaN for 0 * Inf.
 *
 * NONE OF THIS APPEARS IN LIVE PLAY: the same routines graded 6,252 and 6,540
 * real calls with zero disagreements. The route never produces a NaN. That is
 * exactly why the sweep exists -- a substitution justified only by live
 * agreement would ship this difference unnoticed, and a value that is stored
 * and later compared bit-for-bit would diverge the moment one appeared.
 *
 * Canonicalising here costs one compare on a path that is already a function
 * call, and makes the substitution bit-exact rather than nearly so.
 */
#define BATTLES_ROM_NAN 0x7FFFFFFFu

static u32 canon(float r)
{
    u32 bits = f_to_bits(r);
    /* NaN is the only value not equal to itself. Testing the bits instead
     * would need the exponent-and-mantissa dance this function exists to
     * avoid, and `r != r` cannot be optimised away without -ffast-math. */
    if (r != r) {
        return BATTLES_ROM_NAN;
    }
    return bits;
}

/* func_0211F324 */
u32 Battles_FAdd(u32 a, u32 b)
{
    return canon(bits_to_f(a) + bits_to_f(b));
}

/* func_0211F934 */
u32 Battles_FSub(u32 a, u32 b)
{
    return canon(bits_to_f(a) - bits_to_f(b));
}

/* func_0211F748 */
u32 Battles_FMul(u32 a, u32 b)
{
    return canon(bits_to_f(a) * bits_to_f(b));
}
