// xdelta (VCDIFF, RFC 3284) patches of the player's own ROM (docs/474): the
// codec, and what lb_mods.cpp / lb_cunits.cpp offer the export and import.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ---- the codec (lb_xdelta.cpp) ------------------------------------------------
// Decodes `patch` against `source` into `target`. The default code table, any
// number of windows, VCD_SOURCE and VCD_TARGET segments, every address mode,
// xdelta3's application header (returned in `appheader`) and its adler32
// window checksum (verified). Secondary compression and custom code tables are
// refused with `why` saying so.
bool lb_vcdiff_decode(const std::vector<uint8_t>& source, const std::vector<uint8_t>& patch,
                      std::vector<uint8_t>* target, std::vector<uint8_t>* appheader, std::string* why);
// Encodes `target` against `source`: block matching for COPY from the source,
// ADD and RUN for the rest, 8 MB target windows, each with the adler32
// checksum xdelta3 writes. No secondary compression. `progress` (0..1) may be
// called from this thread.
bool lb_vcdiff_encode(const std::vector<uint8_t>& source, const std::vector<uint8_t>& target,
                      const std::vector<uint8_t>& appheader, std::vector<uint8_t>* patch,
                      const std::function<void(double)>& progress = nullptr);

// ---- lb_mods.cpp ----------------------------------------------------------------
// The ROM the player supplied (rom/, as read; never changed after boot).
const std::vector<uint8_t>& lb_mods_original();
// A copy of the cartridge image the game runs now, taken under the lock a
// Reset's rebuild holds; `also` runs under the same lock (to read what that
// build made, its custom units). False when no image was kept.
bool lb_mods_copy_live(std::vector<uint8_t>* out, const std::function<void()>& also = nullptr);

// Real hardware: loaded mod map `map` (0..lb_mods_count()-1) put in stock slot
// `slot` (1..30): Maps/mpNN.map, its minimap files, its detail tiles and its
// name in every language file replaced. Nothing else changes.
struct LbSlotPick { int map; int slot; };
bool lb_mods_build_slots(const std::vector<uint8_t>& original, const std::vector<LbSlotPick>& picks,
                         std::vector<uint8_t>* out, std::string* report);

// A map found in a patched image.
struct LbPatchMap {
    std::string name;     // its name, from the image's language file
    std::string stem;     // the file stem to install it under ("mp07", "map")
    std::string where;    // where it was in the image, for the report
    int slot = 0;         // the stock slot it replaced, or 0
    int players = 0, world = 0;
    std::vector<uint8_t> map, detail, mini[4];
    uint64_t hash = 0;    // docs/413's identity (map, detail, minimaps)
};
// What a patched image changed, against the original.
struct LbPatchScan {
    std::vector<LbPatchMap> maps;
    std::vector<std::string> other;   // changed or new files no importer uses
    std::vector<std::string> notes;   // code, header, banner, companions ...
    std::vector<std::string> unit_files;   // changed Sprites/Models/cards files
    bool ebp_changed = false;         // BP/Entities.ebp differs
    bool lng_changed = false;         // a LOC/*.lng differs
};
bool lb_mods_patch_scan(const std::vector<uint8_t>& original, const std::vector<uint8_t>& image,
                        LbPatchScan* out, std::string* why);
// Writes `m` to mods/maps/Imported/<name>/ as a loose map folder (docs/413).
// 1 written, 2 already in the game or already in that folder, 0 failed.
int lb_mods_install_map(const LbPatchMap& m, bool dry_run, std::string* folder, std::string* why);
// A file of an image by path, the game's COMP wrapper removed when `unwrap`.
// The image must outlive the reader.
std::function<bool(const std::string&, std::vector<uint8_t>*)> lb_mods_image_reader(
    const std::vector<uint8_t>& image, bool unwrap);
// String `id` of the image's LOC/American_English.lng ("" when absent).
std::string lb_mods_image_text(const std::vector<uint8_t>& image, uint32_t id);

// ---- lb_cunits.cpp ----------------------------------------------------------------
// A unit bundle (the lobby's "LBUB" form) installed as mods/units/<folder> and
// added to enabled.txt. 1 written, 2 already present (and enabled), 0 failed.
int lb_cunits_install_bundle(const std::vector<uint8_t>& bundle, const std::string& want, bool dry_run,
                             std::string* folder, std::string* why);
// The port's custom-unit block ("CUN1") in a patched blueprint file: its unit
// count, or 0. Both buffers decompressed.
int lb_cunits_block_count(const std::vector<uint8_t>& stock_ebp, const std::vector<uint8_t>& patched_ebp);
// Stock units a patched image re-skinned (a sprite sheet changed), each made a
// custom unit folder: unit.json (the template, its name, the record's changed
// fields) and sheets/*.NCBR. `records_changed` counts stock unit records that
// differ at all (balance changes included); `notes` says what was left out.
struct LbReskin {
    int index = -1;
    std::string folder, name;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;   // relative path, bytes
};
using LbReader = std::function<bool(const std::string&, std::vector<uint8_t>*)>;
bool lb_cunits_find_reskins(const std::vector<uint8_t>& stock_ebp, const std::vector<uint8_t>& patched_ebp,
                            const LbReader& read_stock, const LbReader& read_patched,
                            const std::function<std::string(uint32_t)>& patched_text,
                            std::vector<LbReskin>* out, int* records_changed, std::vector<std::string>* notes);
// A re-skin written as mods/units/<folder> and enabled; as install_bundle.
int lb_cunits_install_reskin(const LbReskin& r, bool dry_run, std::string* folder, std::string* why);

// ---- the jobs (lb_xdelta.cpp), for the Maps > Patches page ------------------------
// Each runs on its own thread; one at a time.
bool lb_xdelta_busy();
int lb_xdelta_progress();   // percent of the current step
// The last job's report (several lines), whether it failed, and whether it
// installed something a "Load new maps and units now" would bring in.
std::string lb_xdelta_message(bool* bad, bool* installed);
// Export the running cartridge (every enabled mod map and custom unit).
bool lb_xdelta_export_port(const std::string& path);
// Export mod maps in stock slots, for real hardware and other emulators.
bool lb_xdelta_export_slots(const std::string& path, const std::vector<LbSlotPick>& picks);
// Import a patch made for the original ROM: decode, find its maps and units,
// install them. `original` names another ROM to apply it to (a patch made for
// the USA release); empty for rom/.
bool lb_xdelta_import(const std::string& path, const std::string& original = std::string());
// The last import's patch when it did not fit rom/ (empty otherwise), so the
// page can offer to apply it to another ROM.
std::string lb_xdelta_failed_patch();
// The last import's patched ROM, written out whole (for the Balance tab's
// "from a ROM", a mods/ .nds, or another emulator).
bool lb_xdelta_have_patched();
bool lb_xdelta_save_patched(const std::string& path);
// LB_XDELTA_SELFTEST (docs/474): after the cartridge is built.
void lb_xdelta_selftest_if_asked();
