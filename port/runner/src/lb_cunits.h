// Custom units and buildings (docs/448): what lb_mods.cpp's cartridge rebuild
// asks of lb_cunits.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// One new file, in a NEW directory "cu" under an existing one (`parent`, a
// path such as "Sprites" or "UI/FEPlayerCards"), so no stock file id moves.
struct CuFile {
    std::string parent;
    std::string name;
    std::vector<uint8_t> data;
};

// A cartridge file by path, decompressed; false when absent.
using CuReader = std::function<bool(const std::string& path, std::vector<uint8_t>* out)>;

// Reads mods/units (the enabled folders) against the stock blueprint file and
// the cartridge's own assets. Fills the files to add and the display names to
// append to every language file (one string per unit, in order). False when no
// custom unit is enabled.
bool lb_cunits_prepare(const std::vector<uint8_t>& stock_ebp, const CuReader& read,
                       std::vector<CuFile>* files, std::vector<std::string>* names);

// The extended BP/Entities.ebp, COMP-wrapped, with each unit's display-name id
// set to first_name_id + k (or left as its template's when first_name_id < 0).
bool lb_cunits_finish(int first_name_id, std::vector<uint8_t>* ebp_wrapped);

// Balance > Custom units' preview: template sheet `tsheet` of blueprint
// `tmpl` as custom unit `folder` would have it (the folder's sheet and frames
// drawn over the template's), 4bpp indices. Reads the running cartridge.
bool lb_cunits_folder_sheet(const std::string& folder, int tmpl, const std::string& tsheet, int* w, int* h,
                            std::vector<uint8_t>* px);

// Community units (lb_unitlab.cpp's Share online and Community units): a
// folder's bundle, a bundle written to mods/units/<folder> after the same
// checks a lobby's get, and a PNG as RGBA (0xAABBGGRR).
bool lb_cunits_folder_bundle(const std::string& folder, std::vector<uint8_t>* out);
bool lb_cunits_unpack(const std::vector<uint8_t>& bundle, const std::string& folder);
bool lb_cunits_png_rgba(const std::vector<uint8_t>& png, int* w, int* h, std::vector<uint32_t>* px);
// BR-0078: model/texture.png onto a model's bytes, as the importer applies it.
bool lb_cunits_apply_model_texture(std::vector<uint8_t>* nsbmd, const std::vector<uint8_t>& png);
// BR-0086 (docs/487): a folder's sounds/<NAME>.wav as the battle will play it,
// as 16-bit mono at the preview device's 32,000 Hz; "" when it worked, else
// why the build would leave the file out. And the sound index a file name is
// read as (0 SELECT, 1 DEATH, 2 STRIKE, 3 HIT, 4 SPECIAL), or -1.
std::string lb_cunits_preview_wav(const std::vector<uint8_t>& wav, std::vector<int16_t>* out);
int lb_cunits_sound_role(const std::string& stem);

// SG-0061, the lab's Convert tool. An .NCBR (raw or COMP-wrapped, 4bpp, linear
// or tiled) as 4-bit palette indices; false with `why` set otherwise.
bool lb_cunits_ncbr_decode(const std::vector<uint8_t>& file, int* w, int* h, std::vector<uint8_t>* idx,
                           std::string* why);
// Bank `bank` (16 colours) of an .NCLR as 0xAABBGGRR; entry 0 is clear.
bool lb_cunits_nclr_decode(const std::vector<uint8_t>& file, int bank, uint32_t out[16]);
// A PNG as a linear 4bpp .NCBR, sized up to whole 8-pixel tiles with clear
// pixels. A 16-colour indexed PNG keeps its indices (the lab's own exports);
// any other picture takes the nearest of pal[1..15] (0xAABBGGRR). `comp` wraps
// the file the way the cartridge's own are.
bool lb_cunits_png_to_ncbr(const std::vector<uint8_t>& png, const uint32_t pal[16], bool comp,
                           std::vector<uint8_t>* out, std::string* why);
