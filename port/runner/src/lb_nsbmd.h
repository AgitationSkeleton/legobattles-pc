// A small software renderer for the cartridge's NSBMD (BMD0) models, for the
// Custom units preview of transports and siege units (docs/448).
//
// It draws the first model of MDL0 in its rest pose (the SBC bytecode run as
// the hardware would: NODEDESC, MTX, POSSCALE, NODEMIX), textured from TEX0,
// with a z-buffer, cutout alpha and a mild flat shade. No animation is applied.
// Faces the material culls are still drawn behind the others, so one-sided
// sails and wheels stay visible as the preview turns (the DS would hide them).
// Each call parses the file again; it is cheap (a few thousand triangles).
#pragma once

#include <cstddef>
#include <cstdint>

// bmd/n: a whole uncompressed .nsbmd file. bank: the team colour, the model
// palette's 16-colour bank (docs/412: 0 Blue, 1 Red, 2 Green, 3 Yellow,
// 4 Purple, 5 unused (white in these models), 6 Grey). The game's colour
// numbers are not banks: Building_Draw3D maps colour c through {1,0,2,3,4,6}
// (0 Red -> bank 1). A bank past the palette's end falls back to 0.
// yaw turns the model about its vertical axis; a positive pitch tips its top
// towards the viewer. The model is fitted into w*h at a size that does not
// change with yaw. out_abgr: w*h pixels, 0xAABBGGRR; untouched pixels are 0.
// Returns false (out cleared) when the file does not parse.
bool lb_nsbmd_render(const uint8_t* bmd, size_t n, int bank, float yaw_radians, float pitch_radians, int w,
                     int h, uint32_t* out_abgr);

// SG-0064: as above in the teal team colour; shades are three BGR555 words
// (light, mid, dark). Bank 5 is built from bank 6 with entries 12-14 replaced.
bool lb_nsbmd_render_teal(const uint8_t* bmd, size_t n, const uint16_t shades[3], float yaw_radians,
                          float pitch_radians, int w, int h, uint32_t* out_abgr);
