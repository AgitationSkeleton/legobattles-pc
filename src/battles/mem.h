/* The guest address space, for code that is graded against the ROM.
 *
 * WHY THIS EXISTS. A reimplemented function has to read the same bytes the
 * cartridge's own code reads, or the comparison means nothing. In the recomp
 * port those bytes live behind ndsrecomp's bus; in a native build they live in
 * a buffer this port owns. The function body should not know which.
 *
 * THE LAYOUT IS THE CARTRIDGE'S, ON PURPOSE. Every offset in this
 * decompilation was measured against the ROM's own structures, and keeping
 * main RAM as one flat region at 0x02000000 means a measured offset stays
 * correct in the native build without being re-derived. Rearranging the data
 * is a later decision and a separate risk; it is not needed to run natively.
 *
 * BATTLES_ON_RECOMP is defined by the runner's build. Without it these are
 * ordinary loads out of `battles_ram`, which is what the native port links.
 */

#ifndef BATTLES_MEM_H
#define BATTLES_MEM_H

#include "types.h"

#define BATTLES_MAIN_RAM_BASE 0x02000000u
#define BATTLES_MAIN_RAM_SIZE 0x00400000u

#ifdef BATTLES_ON_RECOMP

/* Implemented in port/runner/src/lb_shadow.cpp over ndsrecomp's bus. Not
 * inline: the bus read has a fast path and a slow path and the runner owns
 * the choice. */
u32 battles_read32(u32 addr);
u16 battles_read16(u32 addr);
u8  battles_read8(u32 addr);

#define B_R32(a) battles_read32((u32)(a))
#define B_R16(a) battles_read16((u32)(a))
#define B_R8(a)  battles_read8((u32)(a))

#else

extern unsigned char *battles_ram;   /* BATTLES_MAIN_RAM_SIZE bytes */

#define B_PTR(a) (battles_ram + ((u32)(a) - BATTLES_MAIN_RAM_BASE))
#define B_R32(a) (*(const u32 *)(const void *)B_PTR(a))
#define B_R16(a) (*(const u16 *)(const void *)B_PTR(a))
#define B_R8(a)  (*(const u8  *)(const void *)B_PTR(a))

#endif

/* A guest pointer. Kept as a u32 rather than a host pointer so that a struct
 * read out of guest memory has the same width it does in the ROM. */
typedef u32 gptr;

#endif /* BATTLES_MEM_H */
