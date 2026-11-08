// The level editor's map model: the cartridge's .map read and written byte
// for byte, and everything a map folder needs besides (the tree mask, the
// area plane, the minimaps, the COMP container). No emulator or UI here, so a
// test program can round-trip every stock map (tools/levelmap_test.cpp).
//
// The format is docs/15's, as tools/lb_map.py validates it on all 122 maps:
//
//   'MAP'
//   'TERR' w h u6 u7 char[32] tileset
//          A[w*h] terrain class  (0 grass, 2 stone, 3 water, 5 mountain)
//          B[w*h] tree-edge sprite data
//          C[w*h] area id        (8-connected fill of A; grass+stone one family)
//          u16 n, D[n]           tree mask, run lengths, off first
//          E[w*h] u16 metatile   (< 440 the world's table, >= 440 the map's own)
//   'RRET'
//   'EVNT' { 'L' id npts (x y)*n flag [x0 y0 x1 y1] n*9 entities n*8 pickups n*3 walls } '!'
//   'TRIG' { 'L' id flag [10 bytes] } '!'
//   'MARK' { 'L' group n (x y flag)*n } '!'
//   'MINE' { 'L' n (x y)*n } '!'          four groups
//   'PAM'
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lvl {

// Plane A's classes.
enum Terrain : uint8_t { kGrass = 0, kStone = 2, kWater = 3, kMountain = 5 };

// The EVNT entity's type byte (the map tool's EntityType; docs/12).
enum EntityType : uint8_t {
    kHero = 0, kBuilder = 1, kMelee = 2, kRanged = 3, kMounted = 4, kTransport = 5,
    kSpecial = 6, kBase = 7, kHarvester = 8, kMineB = 9, kFarm = 10, kBarracks = 11,
    kSpecialFactory = 12, kTower = 13, kTower2 = 14, kTower3 = 15, kShipyard = 16,
    kBridge = 17, kGate = 18, kWall = 19, kPickup = 20,
};

struct Entity { uint8_t x, y, team, type, subtype, health, e0, e1, e2; };
struct Pickup { uint8_t x, y, team, type, subtype, hp, e0, e1; };
struct Wall { uint8_t x, y, team; };
struct Point { uint8_t x, y; };

struct Event {
    uint8_t id = 0;
    std::vector<Point> points;          // patrol path
    uint8_t area_flag = 0;
    uint8_t area[4] = {0, 0, 0, 0};     // x0 y0 x1 y1 when area_flag
    std::vector<Entity> ents;
    std::vector<Pickup> picks;
    std::vector<Wall> walls;
};

struct Trigger {
    uint8_t id = 0, flag = 0;
    uint8_t body[10] = {};              // only when flag
};

// x, y, flag are the file's three bytes (parse/write copy exactly those).
// `keep` is the level editor's own and is never written: on a bridge site
// (groups 7/8) it means "keep the terrain", no posts or bank tiles stamped
// (SG-0085, docs/473).
struct Marker { uint8_t x, y, flag; uint8_t keep = 0; };
struct MarkerGroup {
    uint8_t id = 0;                     // 0 trees, 3 build, 6 shipyard, 7/8 bridge
    std::vector<Marker> m;
};

struct Map {
    int w = 0, h = 0;
    uint8_t u6 = 3, u7 = 2;             // 3,2 on every stock map
    std::string tileset;                // KingTileset / PirateTileset / MarsTileset
    std::vector<uint8_t> A, B, C;
    std::vector<uint8_t> D;             // the tree mask as stored (run lengths)
    std::vector<uint16_t> E;
    std::vector<Event> events;
    std::vector<Trigger> triggers;
    std::vector<MarkerGroup> markers;
    std::vector<std::vector<Point>> mines;   // four groups
};

// Parses a decompressed .map. Every byte must belong to the grammar above
// and the file must end at 'PAM'; anything else is refused with a reason.
// Sizes are bounded (w, h 8..255) so a hostile file cannot ask for much.
bool parse(const uint8_t* data, size_t n, Map* out, std::string* why);
std::vector<uint8_t> write(const Map& m);

// A shareable Free Play level: what the master server accepts (its
// src/levels.js validateMap) and what a downloaded one must be before it is
// written to disk. Sizes 32..128, the three tilesets, only the four terrain
// classes, a tree mask that covers the map, tiles within the world's table
// and `detail` detail tiles, plane C consistent with plane A, every object
// on the map with known types and teams, golden bricks as the only pickups,
// four mine groups, and two or more starts of different players.
bool validate(const Map& m, size_t detail, std::string* why);
int start_count(const Map& m);

// The tree mask: one byte per cell (0/1) <-> the stored run lengths.
std::vector<uint8_t> trees_decode(const std::vector<uint8_t>& D, int cells);
std::vector<uint8_t> trees_encode(const std::vector<uint8_t>& mask);

// Plane C from plane A.
std::vector<uint8_t> area_fill(const std::vector<uint8_t>& A, int w, int h);

// The game's COMP container with one stored block (docs/05).
std::vector<uint8_t> comp_wrap(const std::vector<uint8_t>& raw);

// A 128x128 4bpp minimap (MinimapGenerator's rules): `tiled` for the NCGR,
// linear for the NCBR; `trees` draws the tree hatch (the "mini" files) or not
// ("miniNT"). Uncompressed; wrap it with comp_wrap to ship.
std::vector<uint8_t> minimap(const Map& m, const std::vector<uint8_t>& trees,
                             bool with_trees, bool tiled);

}  // namespace lvl
