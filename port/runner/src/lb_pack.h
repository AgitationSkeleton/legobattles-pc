// Content packs (SG-0114, docs/490): one .lbpack file holding custom units,
// mod maps, the Balance values and a preset of gameplay settings.
//
// lb_pack.cpp is the container and the files (the pack's own format, the unit
// and map folders, installing them); lb_overlay.cpp is the page (Balance >
// Content packs) and the two parts that live in settings.json: the Balance
// values and the settings preset.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// One file in a pack: a relative path ('/' separated) and its bytes.
struct LbPackEntry {
    std::string path;
    std::vector<uint8_t> data;
};

// The limits a pack is held to, on export and on import.
constexpr size_t kLbPackMaxEntries = 2048;
constexpr size_t kLbPackMaxEntry = 16u << 20;    // one file
constexpr size_t kLbPackMaxTotal = 128u << 20;   // the whole .lbpack
constexpr size_t kLbPackMaxPath = 200;

// A path a pack may hold: relative, '/' separated, no "." or ".." part, no
// drive, no backslash, no characters Windows refuses, no device names.
bool lb_pack_path_ok(const std::string& p);

// The container: "LBPK", version, count, then (path, length, crc32, bytes).
bool lb_pack_write(const std::string& file, const std::vector<LbPackEntry>& entries, std::string* why);
bool lb_pack_read(const std::string& file, std::vector<LbPackEntry>* out, std::string* why);

// What can go into a pack from this machine.
struct LbPackUnitFolder {
    std::string folder;    // mods/units/<folder>
    std::string name;      // unit.json's "name" ("" when none)
    bool enabled = false;  // named in mods/units/enabled.txt
};
std::vector<LbPackUnitFolder> lb_pack_list_units();

struct LbPackMapFolder {
    std::string rel;    // under mods/maps, '/' separated ("Level Editor/My map")
    std::string leaf;   // the folder's own name
};
std::vector<LbPackMapFolder> lb_pack_list_maps();

// A unit folder as "units/<folder>.lbub" (the lobby's LBUB bundle), and a map
// folder's files as "maps/<leaf>/<file>". False with `why` when it cannot go.
bool lb_pack_add_unit(const std::string& folder, std::vector<LbPackEntry>* out, std::string* why);
bool lb_pack_add_map(const LbPackMapFolder& m, const std::string& leaf, std::vector<LbPackEntry>* out,
                     std::string* why);

// A read pack, by kind.
struct LbPackMap {
    std::string leaf;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;   // file name, bytes
};
struct LbPackContents {
    bool has_manifest = false, has_balance = false, has_settings = false;
    std::string manifest, balance, settings;   // pack.json, balance.json, settings.json
    std::vector<std::pair<std::string, std::vector<uint8_t>>> units;   // folder name, LBUB bundle
    std::vector<std::string> unit_names;                              // unit.json "name", per unit
    std::vector<LbPackMap> maps;
    std::vector<std::string> ignored;   // entries no part of the port reads
};
bool lb_pack_split(const std::vector<LbPackEntry>& entries, LbPackContents* out, std::string* why);

// Installing. 1 written (as `folder`), 2 already there, 0 failed (`why`).
// A unit goes to mods/units/<folder> (renamed on a clash) and is added to
// enabled.txt; a map to mods/maps/Packs/<leaf> (renamed on a clash).
int lb_pack_install_unit(const std::string& want, const std::vector<uint8_t>& bundle, std::string* folder,
                         std::string* why);
int lb_pack_install_map(const LbPackMap& m, std::string* folder, std::string* why);
