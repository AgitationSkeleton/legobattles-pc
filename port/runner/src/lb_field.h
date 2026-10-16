// lb_field.h - the whole-battlefield renderer.
//
// This file belongs to legobattles-decomp, not to ndsrecomp.
//
// docs/167: the emulated 2D path cannot show more field than the DS already
// does, because BG1/BG2 are a scroll double-buffer and only one screen's worth
// of terrain is ever materialised in VRAM. docs/168: the level grid in main RAM
// describes the whole map, so drawing it host-side has no such limit.
//
// Everything is located at runtime from what the guest is displaying. No
// address is hardcoded: they move between levels, and pretending otherwise
// would be guessing. tools/lb_field.py is the reference
// implementation these algorithms were developed and verified against.

#ifndef LB_FIELD_H
#define LB_FIELD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LbFieldInfo {
    uint32_t map_base;         // BG1's tilemap in VRAM
    uint32_t char_base;        // BG1's tile graphics in VRAM
    uint32_t primary_table;    // metatile table, 12 bytes per entry, ALREADY +2
    uint32_t secondary_table;  // the second table, indexed by (mt - split)
    int      table_count;      // the SPLIT: indices below it use the primary
    uint32_t grid_base;        // legacy flat base; 0 when row_table is used
    int      grid_w, grid_h;   // in metatiles
    int      phase_x, phase_y; // the screen window's alignment within a metatile
    int      cam_cell_x;       // camera's top-left cell in the grid
    int      cam_cell_y;
    // ---- the pointer chain, read out of the cartridge (func_020A3478) -------
    //
    // The level grid is a ROW TABLE, not a flat array: row y lives at
    // *(u32*)(map + 0x34 + y*4) and holds one u16 metatile index per column.
    // Reading it as `grid_base + (y*w + x)*2` only works while the rows happen
    // to be allocated contiguously, which is a property of one level and not of
    // the format.
    uint32_t map_obj;          // *(u32*)0x02156CB0, the map object
    uint32_t row_table;        // map + 0x34; 0 means fall back to grid_base
} LbFieldInfo;

// Locate every structure above. Expensive (it scans main RAM), so call it once
// per level rather than per frame. Returns false when the level is not up yet.
bool lb_field_locate(LbFieldInfo* out);

// Limit lb_field_render and lb_field_apply_shroud to a window in MAP PIXELS.
// A zero-area window means the whole map, which is what the overlay panel
// wants. The host-terrain path sets it to the camera rect plus the bands,
// because nothing outside that is ever sampled -- see the note in
// lb_hostterrain_prepare.
void lb_field_set_window(int x0, int y0, int x1, int y1);
void lb_field_get_window(int* x0, int* y0, int* x1, int* y1);

// Pixel size of the whole map for a located level.
void lb_field_size(const LbFieldInfo* info, int* w, int* h);

// Draw the whole map as 0xFFRRGGBB, row-major, into `dst`, which must hold
// lb_field_size() pixels. Cheap enough to call per frame.
bool lb_field_render(const LbFieldInfo* info, uint32_t* dst);

// Does a previously located level still look like itself?
//
// The tables and the grid MOVE between levels -- docs/168 saw the metatile
// tables at different addresses in consecutive runs -- so a located level that
// has since been torn down would otherwise be drawn from stale addresses.
// Cheap: samples a handful of grid cells for plausible metatile indices.
bool lb_field_still_valid(const LbFieldInfo* info);

// Capture the appearance of every unit currently inside the camera.
//
// The game culls units before submission (docs/174), so a unit off its own
// screen can never be drawn by widening anything. Its pixels are recoverable
// though: whatever the guest paints over the terrain IS the unit, and the
// terrain is reproducible host-side exactly. Differencing the two lifts the unit
// out with a free alpha mask.
//
// Call this EVERY frame -- it is cheap, because it renders terrain only for the
// 40x40 patch under each unit rather than the whole map. Learning only when a
// view happens to be drawn means a unit is almost never on screen at the moment
// of capture.
void lb_field_learn_sprites(const LbFieldInfo* info);

// Composite the guest's LIVE field over the host-drawn map.
//
// This is what makes the view genuinely wider rather than a panned stock view.
// The host renderer supplies terrain for the WHOLE level -- ground the DS never
// draws, because docs/167 showed only one screen's worth exists in VRAM at a
// time -- and the guest's own 256x192 output is placed at the camera position so
// units, buildings and effects are the real thing wherever the DS renders them.
//
// `dst` must already hold lb_field_render()'s output. Returns false if the
// camera is not available.
bool lb_field_composite_guest(const LbFieldInfo* info, uint32_t* dst);

// Read the camera straight out of the game's own state.
//
// docs/174: func_020396E0 is the scroll callback. It reads the camera as two
// signed 16-bit pixel values at +0x5E / +0x60 of its view object and writes only
// the SUB-TILE remainder to BGnHOFS/VOFS -- which is why the tilemap is rewritten
// every 8 px in x and 16 px in y. The object is found by locating the callback
// record that holds the function pointer; the userdata sits in the next word.
//
// This replaces terrain matching, which docs/173 showed can only work over
// explored ground and is defeated by a symmetric map.
//
// Returns false before the level is up.
bool lb_field_camera(int* map_x, int* map_y);

// Update `info`'s camera cell and phase from what the guest is displaying NOW.
//
// lb_field_locate() is far too expensive to run per frame, but its result goes
// stale the moment the player scrolls -- so a camera box drawn from the located
// cell alone is frozen. This re-matches the visible window against the already
// located grid, which is cheap: it searches near the last known cell first and
// only falls back to the whole grid when that fails.
//
// Returns false when the window cannot be placed (a scene change, or the level
// has been torn down), which is the signal to re-locate.
bool lb_field_track_camera(LbFieldInfo* info);

// Enumerate live entities, in MAP pixels ready to draw on the render above.
//
// Positions come from the simulation's own world space (docs/169: entity
// +0x0EC / +0x0F0, 20.12 fixed point, proven world-absolute by scrolling the
// camera and watching them not move). That space is offset from the level
// array's frame by a constant this returns already applied.
//
// Returns false once `index` is past the last live entity.
bool lb_field_entity_at(int index, int* owner, int* category, int* map_x, int* map_y);

// The same entity's blueprint RECORD address, which names it: records sit at
// base + 4 + index * 0x7C, so (record - base - 4) / 0x7C indexes
// out/eur/entities.json for the name and category. Category 0 is "Hero".
bool lb_field_entity_blueprint(int index, uint32_t* record);

// The entity's own address, so a watch can be pointed at it.
bool lb_field_entity_ptr(int index, uint32_t* addr);

// The camera's rectangle in map pixels, so a viewer can show what the game
// window is actually covering.
void lb_field_camera_rect(const LbFieldInfo* info, int* x, int* y);

// Map pixels -> pixels on the cartridge's own minimap, within the 256x192 top
// screen. Measured from the game's rendering rather than assumed; see the
// constants in lb_field.cpp and docs/180.
void lb_field_map_to_minimap(int map_x, int map_y, float* mm_x, float* mm_y);

// 1 if the player has explored this metatile cell, read back from the
// cartridge's own minimap (docs/189). 1 when unknown, so nothing is hidden
// on a frame the minimap could not be classified.
int lb_field_cell_explored(int cx, int cy);

// The shroud is painted into a whole-map buffer ONCE per change, not per
// frame: rebuilding it every frame cost 1.5M pixel writes and a 6 MB
// texture upload, which was the frame rate. Call refresh, compare the
// generation, and only re-apply when it moved.
void lb_field_refresh_shroud(const LbFieldInfo* info);
int  lb_field_shroud_generation(void);
// Diagnostic: print the map chain as read from the calling thread. Used to
// compare the main thread against lb_widemap's gate thread.
void lb_field_gate_probe(void);
// LB_ITCMWATCH=1: check the two ITCM bytes docs/326 tracks, once per frame, and
// dump the bus access ring the first time they change. Costs two byte reads a
// frame; the per-write version suppressed the bug it was hunting.
void lb_itcm_watch(void);
// True only when the held shroud came from the located map record (docs/247)
// rather than the minimap-readback fallback.
int  lb_field_shroud_from_record(void);
void lb_field_apply_shroud(const LbFieldInfo* info, uint32_t* dst);

#ifdef __cplusplus
}
#endif

#endif  // LB_FIELD_H
