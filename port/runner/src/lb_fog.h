// lb_fog.h - the map-space visibility record, read the way the game reads it.
//
// This file belongs to legobattles-decomp, not to ndsrecomp.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT
//
// The same pointer chain is already followed inside lb_widemap.cpp, to fog the
// synthesised band columns. docs/325 phase 4 deletes that file: its purpose --
// inventing content for columns either side of the DS aspect -- goes away once
// the field is drawn from map-space data at a host-chosen scale. Exporting the
// reader from there would make phase 4 an untangling job instead of a delete,
// so the chain lives here and lb_widemap keeps its own copy until it goes.
//
// THE CHAIN, out of the decomp (docs/247). func_020EEBF4 is the only function
// that reads the mask, and it is 32 bytes:
//
//     return (*(byte *)(*(int *)(*(int *)(param_1 + 0x14) + param_3 * 4) +
//                       (param_2 >> 3)) & 1 << (param_2 & 7)) != 0;
//
// A descriptor with a row table at +0x14, indexed row[y][x>>3] & (1<<(x&7)) --
// one BIT per metatile, which is why every earlier search that read it as a
// byte failed even when it had the right address.
//
// POLARITY IS FROM THE DECOMP, NOT ASSUMED: the caller func_02036D1C reports
// "not visible" when a rectangle's four corners all read 0, so a SET bit means
// revealed.
//
// WHY IT IS TRUSTED: the record is graded every frame against the tile the
// guest itself drew, and reads 100.0% agreement on all three metatile phases
// over 400,000+ samples, against 88.6/94.6/94.3% for the best buffer the old
// correlation search ever found. Fog appears exactly where this bit is clear.
#ifndef LB_FOG_H
#define LB_FOG_H

#include <cstdint>

// Follow the chain and validate it against the map this battle is on. Both the
// holder and the descriptor MOVE between battles (0226991C in one run, 022698E0
// in the next), so this is called per battle and never cached across a session.
//
// Returns false and changes nothing if the chain does not validate, which is
// the state between battles and during a load.
bool lb_fog_locate(int map_w, int map_h);

// True once a validated record is held.
bool lb_fog_ready(void);

// One metatile. Out of range is "not revealed", the conservative answer: it can
// over-fog, but it can never reveal ground the player has not seen.
bool lb_fog_revealed(int mx, int my);

// Drop the held record. Called when the map changes underneath us.
void lb_fog_drop(void);

#endif  // LB_FOG_H
