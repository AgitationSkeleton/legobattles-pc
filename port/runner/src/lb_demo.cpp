// lb_demo.cpp - record the player's input, and play it back.
//
// Part of legobattles-decomp. See port/runner/README.md.
//
// WHY THIS EXISTS
//
// Scripted probes spent a long time failing to click on a unit; a recording of
// a player's real inputs replaces them.
//
// The reason is worth stating plainly rather than buried. Every automated probe this project has written for the touch screen
// has had to GUESS something -- where a sprite's clickable pixels are relative
// to the position the port reports, how long a drag has to be held, what
// deselects, whether the thing it just selected is the thing it aimed at -- and
// each guess has produced a confident wrong answer at least once (docs/214,
// docs/217, docs/218). A player guesses none of it.
//
// So the player plays, this records exactly what the guest received, and the
// port replays it as many times as an investigation needs. The recording is the
// ground truth that the probes were trying and failing to synthesise.
//
// WHAT IS RECORDED, AND WHY AT THIS LEVEL
//
// The two calls the frontend makes into the guest, and nothing else:
//
//     nds_set_touch(x, y, down)      the DS touch panel
//     nds_set_key_mask(mask)         the buttons
//     lb_touch_set_extended(x)       this port's own out-of-band coordinate
//
// Recording window pixels instead would tie a demo to the window size it was
// made at, and recording SDL events would tie it to the layout, the HUD anchor
// and the ImGui panel positions -- all of which this project changes weekly. At
// the guest boundary a demo means "the console received this", which stays true
// however the port draws.
//
// Frames, not wall-clock. The counter advances once per vblank, so a replay
// lands each input on the same frame the player did regardless of how fast the
// host runs. That is also what makes a replay a fair test of a change: if the
// picture differs between two builds replaying one demo, the change did it.
//
// THE FORMAT is one line per CHANGE, not per frame -- a minute of play is a few
// hundred lines and it can be read and edited by hand:
//
//     lbdemo 1 cadence=<20|30> hudhit=<0|1> camedge=<0|1> snd=<0|1> depth=<0|1>
//     <frame> t <down> <x> <y> <extended-x>
//     <frame> k <mask>
//     <frame> c <20|30>          the game pacing changed (docs/351)
//
// Anything the line does not carry is unchanged from the frame before, which is
// exactly how the hardware behaves between reads. A header without `cadence=`
// is a recording made before the steady-pacing default, and replays at 20. (A
// build older than 'c' events would read one as a key mask; they are written
// only when the pacing setting is changed during a recording.)
//
// `hudhit=1` (docs/371): the touches are the TRUE coordinates, because the
// cartridge's widget hit test was corrected for the moved HUD while this was
// recorded. Without it, a click on a moved HUD button was stored already
// rewritten to the button's native point by the frontend. Replaying such a
// demo with the correction on would move that point away again and miss the
// button, so it replays with the correction off.
//
// `camedge=1` (docs/373): the camera's range stopped where the picture stops.
// A recording without it was made with the stock range, and its taps were
// aimed at wherever that camera was, so it replays with the stock range.
// `camgrid=1` (docs/393): and those bounds sat on the cartridge's 8-px camera
// grid; without it a camedge recording replays the unrounded bounds.
//
// `snd=1` (docs/376): the cartridge's audibility window for unit and building
// sounds was widened to the picture. Starting more sounds is more guest work
// per pass, and the pass length is what the game's pacing reads (docs/351),
// so a recording without it replays with the stock window to stay exact.
//
// `depth=1` (docs/377): sprite depth was 11 + feet row across the vertical
// bands instead of the cartridge's byte. The four functions it patches run
// in Tier 3 while it is on, which changes the counts, so a recording without
// it replays with the stock words.
//
// `exty=1` (docs/383): touch lines carry a seventh field, the out-of-band Y
// (INT32_MIN for none). Before it only the X travelled, so a click above or
// below the DS's own 192 rows -- a unit up there, or a HUD button anchored to
// the window's top or bottom -- replayed at a clamped row 0 or 191. A touch is
// also written when only its out-of-band coordinate changed: a drag inside a
// band holds the clamped panel value at the edge while the true one moves.
//
// `hudmemo=1` (docs/383): a press on a moved HUD button that this frame's scan
// did not show (the frames after a camera step draw no HUD) was answered from
// the widget's memo or the last scan with HUD, not as stock. `fp2d=1`: the
// placement footprint was harvested with a site-relative shift in both axes.
// `fpline=1` (docs/392): a wall or bridge line's site is the painter's own
// cursor cell, not the building-ghost record that a line leaves at 0,0.
// All three change what the cartridge does with the same inputs, so a
// recording without them replays with the old rules.
//
// `quick=1` (docs/383): the recording may hold `q` lines, `FRAME q ACTION`: a
// battle shortcut (Deselect, a brick) that ran the cartridge's own function
// for that button in that guest frame. See lb_demo_note_quick.
//
// `enemy8=1` (docs/384): Free Play's opponent selector cycles through eight
// choices (the six factions, Custom, Random), not six, so the same presses
// reach a different choice; an older recording cycles mod 6. Each Custom or
// Random opponent the recording fielded is an `a` line, `FRAME a v0 .. v9`,
// the army record's ten halfwords, taken in order on replay -- a Random draw
// and the Team Builder's enemy team are not part of the recording otherwise.
//
// `cage=N` and `ownhero=N` (docs/385) carry the RECORDER's setting, 0 or 1:
// the monkey cage fix (it changes which tier runs Unit_BuildBillboard, and so
// the counts) and Prioritize Own Hero Spells (it changes which selected hero
// the HUD drives). A replay takes them from the header, whatever the
// replaying machine's own settings say; absent, both are 0 (stock).
// `oppname=N` (docs/385): a Custom or Random opponent is named after its
// hero, not its base faction -- 1 the hero's full name (the first docs/385
// build), 2 with the four long ones shortened (Brickbeard, Broadside, Space
// Police, Space Criminal); without it the faction's name, as recorded.
// `tileset=N palette=N pickseed=S` (docs/387): the recorder's Force Tileset
// and Level Palettes settings, and the seed their Random draws come from (a
// draw is per level, counted from boot). Absent: Off, Per-Mod, 0.
// `music=N sets=1` (docs/388): the recorder's Music mode, and `FRAME s K V`
// lines -- setting K (0 Force Tileset, 1 Level Palettes, 2 Music, 3 the cage
// fix, 4 Prioritize Own Hero Spells) changed to V on that frame. The header
// is written before the settings file is read, so it holds the DEFAULTS:
// a recorded session can say tileset=0 while its log forces Random
// on the first battle. The first frames therefore carry an `s` line for
// every setting that differs, and a setting changed mid-session gets one
// too. Absent: the replaying machine's Music mode, as before.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>   /* legobattles-decomp: one recording name per launch */
#include <array>
#include <string>
#include <vector>

#include "lb_overlay.h"

// The runner's own two, which are C++ (io.h declares them without extern "C"),
// so they must be declared the same way here or the link fails on a mangled
// name that nothing defines.
void nds_set_touch(uint16_t x, uint16_t y, bool down);
void nds_set_key_mask(uint32_t mask);

extern "C" {
// The guest clock (io.cpp, added by apply_hooks.py; docs/396).
void lb_rtc_get(unsigned char out[7]);
void lb_rtc_set(const unsigned char in[7]);
int lb_rtc_is_host(void);
void lb_touch_set_extended(int x);
void lb_touch_set_extended_y(int y);   /* docs/383 */
void lb_touch_clear_extended(void);
}

namespace {

enum class Mode { Idle, Record, Play };

struct Event {
    uint32_t frame;
    char kind;          // 't' touch, 'k' keys
    int32_t a, b, c, d; // touch: down, x, y, ext   keys: mask
    int32_t e = INT32_MIN;   // touch: extended y (docs/383), INT32_MIN none
};

Mode g_mode = Mode::Idle;
std::vector<Event> g_events;
size_t g_play_at = 0;
uint32_t g_frame = 0;
std::string g_path;
std::string g_status = "idle";

// The live state, so only CHANGES are written.
//
// g_last_ext's "none" is INT32_MIN, and it has to be, because that is the value
// lb_patches uses to mean "this port has nothing to say about the x, hand the
// game the native one". It is NOT a spare number picked here.
//
// The first version stored "none" as 0 and wrote 0 into the file, on the
// reasoning that an unset coordinate may as well be zero. 0 is a legal
// out-of-band x, so a replay then called lb_touch_set_extended(0) on every
// event and published x = 0 to the touch slot for every tap in the demo --
// sending every one of them to the far-left column. Measured: a replay of the
// menu route selected nothing and ended on the name-entry keyboard, having
// tapped an empty profile slot instead of "Max".
int g_last_down = -1, g_last_x = -1, g_last_y = -1;
int g_last_ext = INT32_MIN;
int g_last_ext_y = INT32_MIN;               // docs/383
int g_rec_ext = INT32_MIN, g_rec_ext_y = INT32_MIN;   // as last WRITTEN
uint32_t g_last_keys = 0xFFFFFFFFu;

// Set while lb_demo_tick is injecting, so the interceptors let those calls
// through instead of treating them as live input to be dropped.
bool g_injecting = false;

// THE GAME'S PACING TRAVELS WITH THE RECORDING (docs/351).
//
// Inputs are indexed by VBlank, but the game reads them once per PASS of its
// loop, and the pass cadence is a setting: steady 30 Hz (the default since
// 2026-09-23) or the cartridge's own 20 ms threshold. A recording replayed at
// the other cadence hands each input to a different simulation step -- and a
// one-VBlank tap can fall between two passes -- so it would replay into a
// game its player never played. The header says which cadence the demo was
// made at (`lbdemo 1 cadence=30`), a 'c' event marks a change mid-recording,
// and a file with neither predates the steady default: 20.
int g_demo_cadence = 20;   // playback: the cadence in force at this point
int g_rec_cadence = 0;     // recording: the cadence last written
int g_demo_hudhit = 0;     // playback: the header's hudhit= (docs/371)
int g_demo_camedge = 0;    // playback: the header's camedge= (docs/373)
int g_demo_camgrid = 0;    // playback: the header's camgrid= (docs/393)
int g_demo_snd = 0;        // playback: the header's snd= (docs/376)
int g_demo_depth = 0;      // playback: the header's depth= (docs/377)
int g_demo_exty = 0;       // playback: the header's exty= (docs/383)
int g_demo_hudmemo = 0;    // playback: the header's hudmemo= (docs/383)
int g_demo_fp2d = 0;       // playback: the header's fp2d= (docs/383)
int g_demo_fpline = 0;     // playback: the header's fpline= (docs/392)
int g_demo_enemy8 = 0;     // playback: the header's enemy8= (docs/384)
int g_demo_cage = 0;       // playback: the header's cage= (docs/385)
int g_demo_ownhero = 0;    // playback: the header's ownhero= (docs/385)
int g_demo_oppname = 0;    // playback: the header's oppname= (docs/385)
int g_demo_tileset = 0;    // playback: the header's tileset= (docs/387)
int g_demo_palette = 0;    // playback: the header's palette= (docs/387)
unsigned g_demo_seed = 0;  // playback: the header's pickseed= (docs/387)
int g_demo_music = -1;     // playback: music=, then `s 2` lines (docs/388)
unsigned char g_demo_rtc[7] = {};   // playback: the header's rtc= (docs/396)
bool g_demo_has_rtc = false;
// docs/388: a replay that ran to its end. The level a recording's last
// input starts loads after that input -- a Free Play recording ends on the
// Start press -- so its Force Tileset, Level Palettes and Music stay in
// effect for the rest of the run instead of falling back to this machine's.
bool g_replay_done = false;
// docs/388: recording, the settings as last written.
int g_rec_tileset = 0, g_rec_palette = 0, g_rec_music = 0, g_rec_cage = 0,
    g_rec_ownhero = 0;
// docs/392: kinds 5..7 -- Free Play's opening opponent, Transport Occupants
// and Hotwire targets. Recorded from `sets=2`; a replay of an older
// recording leaves them to this machine (-1), one of sets=2 starts from the
// stock values (Wizard, Off, Vanilla) and follows its `s` lines.
int g_rec_oppdef = 1, g_rec_hwtransport = 0, g_rec_hwmode = 0;
int g_demo_oppdef = -1, g_demo_hwtransport = -1, g_demo_hwmode = -1;
// docs/406: kind 8, EMP disables transports. Every recording made before it
// ran without it, so a replay starts from Off whatever its version.
int g_rec_emp = 0, g_demo_emp = 0;
// docs/406: kind 9, the red-brick cheats as a mask, from `sets=3` (an older
// recording leaves them to this machine, -1, as before); kind 10, the AI
// gets them too, Off in every recording before it.
int g_rec_cheats = 0, g_demo_cheats = -1;
int g_rec_aicheats = 0, g_demo_aicheats = 0;
int g_rec_aihero = 0, g_demo_aihero = 0;   // kind 11, the AI's hero rebuild
int g_rec_trel = 0, g_demo_trel = 0;       // kind 12, docs/408
int g_rec_crea = 0, g_demo_crea = 0;       // kind 13, docs/410
// docs/409: THE FIELD'S SIZE, which zoom and the window decide and which the
// simulation sees (camera bounds, where a moved HUD button is touched). A
// session that zoomed could not be replayed: four replays of one such
// recording diverged in the first battle. `f W H` lines record each size the frontend
// picks; a replay of one sizes its field the same, whatever its window.
// Recordings without them keep the window's own.
int g_rec_fw = 0, g_rec_fh = 0, g_demo_fw = 0, g_demo_fh = 0;
// docs/384: the opponent armies a recording fielded, in order, and the next
// one a replay takes.
std::vector<std::array<unsigned char, 20>> g_armies;
size_t g_army_at = 0;
// The frame of the last `a` line. The army is asked for by the Start handler
// a few frames after the Start press, which is often the recording's last
// timed event, so a replay stays in Play until the frame AFTER this one
// (run rep384a: finished at 4677, asked at 4680, fielded the wrong army).
uint32_t g_army_last_frame = 0;

void reset_state() {
    g_last_down = g_last_x = g_last_y = -1;
    g_last_ext = INT32_MIN;
    g_last_ext_y = INT32_MIN;
    g_rec_ext = g_rec_ext_y = INT32_MIN;
    g_last_keys = 0xFFFFFFFFu;
}

bool load(const char* path) {
    g_events.clear();
    FILE* fh = std::fopen(path, "rb");
    if (!fh) return false;
    // docs/404: the header line is past 230 characters; a 256-byte buffer
    // would have split a longer one into "events".
    char line[1024];
    bool header = false;
    g_demo_cadence = 20;          // unless the header says otherwise
    g_demo_hudhit = 0;            // likewise: older recordings are unmapped
    g_demo_camedge = 0;           // and made with the stock camera range
    g_demo_camgrid = 0;           // and, if widened, unrounded bounds
    g_demo_snd = 0;               // and the stock audibility window
    g_demo_depth = 0;             // and the stock sprite depth
    g_demo_exty = 0;              // and carry no out-of-band Y
    g_demo_hudmemo = 0;           // and the old HUD-hit rules
    g_demo_fp2d = 0;              // and the one-axis footprint harvest
    g_demo_fpline = 0;            // and a line's site at the ghost's 0,0
    g_demo_enemy8 = 0;            // and the six-way opponent selector
    g_demo_cage = 0;              // and the stock cage choice
    g_demo_ownhero = 0;           // and the stock hero HUD pick
    g_demo_oppname = 0;           // and the base faction's name
    g_demo_tileset = 0;           // and each map's own tileset
    g_demo_palette = 0;           // and its own palette
    g_demo_seed = 0u;
    g_demo_music = -1;            // and this machine's Music mode
    g_demo_oppdef = g_demo_hwtransport = g_demo_hwmode = -1;   // docs/392
    g_demo_emp = 0;               // docs/406: and no EMP on transports
    g_demo_cheats = -1;           // and this machine's cheats
    g_demo_aicheats = 0;          // and none for the AI
    g_demo_aihero = 0;            // and the stock hero rebuild
    g_demo_trel = 0;              // and the stock transport ownership
    g_demo_crea = 0;              // and no creatures in armies
    g_demo_fw = g_demo_fh = 0;    // docs/409: and the window's own field
    g_demo_has_rtc = false;       // and the clock the replaying run has
    g_armies.clear();
    g_army_at = 0;
    g_army_last_frame = 0;
    while (std::fgets(line, sizeof line, fh)) {
        if (!header) {
            header = true;
            if (std::strncmp(line, "lbdemo", 6) == 0) {
                if (const char* c = std::strstr(line, "cadence=")) {
                    const int v = std::atoi(c + 8);
                    if (v == 20 || v == 30) g_demo_cadence = v;
                }
                if (const char* h = std::strstr(line, "hudhit="))
                    g_demo_hudhit = std::atoi(h + 7) == 1 ? 1 : 0;
                if (const char* e = std::strstr(line, "camedge="))
                    g_demo_camedge = std::atoi(e + 8) == 1 ? 1 : 0;
                if (const char* g = std::strstr(line, "camgrid="))
                    g_demo_camgrid = std::atoi(g + 8) == 1 ? 1 : 0;
                if (const char* s = std::strstr(line, "snd="))
                    g_demo_snd = std::atoi(s + 4) == 1 ? 1 : 0;
                if (const char* d = std::strstr(line, "depth="))
                    g_demo_depth = std::atoi(d + 6) == 1 ? 1 : 0;
                if (const char* y = std::strstr(line, "exty="))
                    g_demo_exty = std::atoi(y + 5) == 1 ? 1 : 0;
                if (const char* m = std::strstr(line, "hudmemo="))
                    g_demo_hudmemo = std::atoi(m + 8) == 1 ? 1 : 0;
                if (const char* p = std::strstr(line, "fp2d="))
                    g_demo_fp2d = std::atoi(p + 5) == 1 ? 1 : 0;
                if (const char* f = std::strstr(line, "fpline="))
                    g_demo_fpline = std::atoi(f + 7) == 1 ? 1 : 0;
                if (const char* q = std::strstr(line, "enemy8="))
                    g_demo_enemy8 = std::atoi(q + 7) == 1 ? 1 : 0;
                if (const char* g = std::strstr(line, "cage="))
                    g_demo_cage = std::atoi(g + 5) == 1 ? 1 : 0;
                if (const char* o = std::strstr(line, "ownhero="))
                    g_demo_ownhero = std::atoi(o + 8) == 1 ? 1 : 0;
                if (const char* n = std::strstr(line, "oppname="))
                    g_demo_oppname = std::atoi(n + 8) == 2   ? 2
                                     : std::atoi(n + 8) == 1 ? 1 : 0;
                if (const char* t = std::strstr(line, "tileset=")) {
                    const int v = std::atoi(t + 8);
                    g_demo_tileset = (v >= 0 && v <= 4) ? v : 0;
                }
                if (const char* p = std::strstr(line, "palette=")) {
                    const int v = std::atoi(p + 8);
                    g_demo_palette = (v >= 0 && v <= 3) ? v : 0;
                }
                if (const char* s = std::strstr(line, "pickseed="))
                    g_demo_seed = (unsigned)std::strtoul(s + 9, nullptr, 10);
                if (const char* m = std::strstr(line, "music=")) {
                    const int v = std::atoi(m + 6);
                    g_demo_music = (v >= 0 && v <= 9) ? v : -1;
                }
                if (const char* r = std::strstr(line, "rtc=")) {   // docs/396
                    unsigned v[7];
                    if (std::sscanf(r + 4, "%2x%2x%2x%2x%2x%2x%2x", &v[0], &v[1],
                                    &v[2], &v[3], &v[4], &v[5], &v[6]) == 7) {
                        for (int k = 0; k < 7; ++k) g_demo_rtc[k] = (unsigned char)v[k];
                        g_demo_has_rtc = true;
                    }
                }
                if (const char* s2 = std::strstr(line, "sets="))   // docs/392
                    if (std::atoi(s2 + 5) >= 2) {
                        g_demo_oppdef = 1;
                        g_demo_hwtransport = 0;
                        g_demo_hwmode = 0;
                        if (std::atoi(s2 + 5) >= 3) g_demo_cheats = 0;   // docs/406
                    }
                continue;
            }
        }
        unsigned f = 0;
        char kind = 0;
        {   // docs/384: an opponent army, kept apart from the timed events
            unsigned af = 0;
            int v[10];
            if (std::sscanf(line, "%u a %d %d %d %d %d %d %d %d %d %d", &af,
                            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                            &v[7], &v[8], &v[9]) == 11) {
                std::array<unsigned char, 20> rec{};
                for (int i = 0; i < 10; ++i) {
                    rec[(size_t)(2 * i)] = (unsigned char)(v[i] & 0xFF);
                    rec[(size_t)(2 * i + 1)] = (unsigned char)((v[i] >> 8) & 0xFF);
                }
                g_armies.push_back(rec);
                if (af > g_army_last_frame) g_army_last_frame = af;
                continue;
            }
        }
        int a = 0, b = 0, c = 0, dd = 0, ee = INT32_MIN;
        const int got = std::sscanf(line, "%u %c %d %d %d %d %d", &f, &kind,
                                    &a, &b, &c, &dd, &ee);
        if (got >= 3) {
            Event ev{f, kind, a, b, c, dd};
            if (kind == 't' && g_demo_exty && got >= 7) ev.e = ee;
            g_events.push_back(ev);
        }
    }
    std::fclose(fh);
    return !g_events.empty();
}

// THE RECORDING IS WRITTEN AS IT HAPPENS, not at the end.
//
// The player who makes a demo will finish by closing the window, and a session
// that ends by closing the window never reaches a tidy shutdown. A recorder
// that buffers everything and saves on stop would hand back an empty file
// exactly when it mattered, and losing a player's recording to a missing
// fclose is not acceptable.
//
// One line per event, flushed immediately. A kill -9 costs at most the event
// still in flight, and the file left behind is a valid demo either way because
// the format has no trailer.
FILE* g_rec = nullptr;

void emit(const Event& e) {
    if (!g_rec) return;
    if (e.kind == 't')
        std::fprintf(g_rec, "%u t %d %d %d %d %d\n", e.frame, e.a, e.b, e.c,
                     e.d, e.e);
    else if (e.kind == 'c')
        std::fprintf(g_rec, "%u c %d\n", e.frame, e.a);
    else if (e.kind == 'q')
        std::fprintf(g_rec, "%u q %d\n", e.frame, e.a);
    else if (e.kind == 's')   /* docs/388: a setting changed */
        std::fprintf(g_rec, "%u s %d %d\n", e.frame, e.a, e.b);
    else if (e.kind == 'b')   /* docs/404: a Balance tab value */
        std::fprintf(g_rec, "%u b %d %d %d\n", e.frame, e.a, e.b, e.c);
    else if (e.kind == 'f')   /* docs/409: the field's size */
        std::fprintf(g_rec, "%u f %d %d\n", e.frame, e.a, e.b);
    else
        std::fprintf(g_rec, "%u k %d\n", e.frame, e.a);
    std::fflush(g_rec);
}

// docs/404: the Balance tab's values as last WRITTEN to the recording, one
// slot per (record, field); a `b` line for each that differs from the
// player's set now (-1: the value was cleared). `initial` starts from none,
// so a recording opens with the whole set on frame 0.
std::vector<int> g_rec_bal;
unsigned g_rec_bal_gen = 0u;
void rec_balance_diff(bool initial) {
    if (g_mode != Mode::Record) return;
    const unsigned gen = lb_balance_custom_generation();
    if (!initial && gen == g_rec_bal_gen) return;   // nothing changed
    g_rec_bal_gen = gen;
    const int kinds[2] = {LB_BAL_UNITS, LB_BAL_ATTACKS};
    const int recs = lb_balance_record_count(LB_BAL_UNITS) +
                     lb_balance_record_count(LB_BAL_ATTACKS);
    const int stride = 32;   // docs/408: attacks have 17 fields, units 15
    if (initial || (int)g_rec_bal.size() != recs * stride)
        g_rec_bal.assign((size_t)(recs * stride), -1);
    for (const int kind : kinds) {
        const int first = lb_balance_record_first(kind);
        const int n = lb_balance_record_count(kind);
        const int nf = lb_balance_field_count(kind);
        for (int r = first; r < first + n; ++r)
            for (int f = 0; f < nf && f < stride; ++f) {
                const int now = lb_balance_custom_get(r, f);
                int& was = g_rec_bal[(size_t)(r * stride + f)];
                if (now == was) continue;
                was = now;
                const Event e{g_frame, 'b', r, f, now, 0};
                g_events.push_back(e);
                emit(e);
            }
    }
}

}  // namespace

// docs/383: A BATTLE SHORTCUT, TAKEN. The action a shortcut ran (Deselect, a
// brick), noted in the guest frame the cartridge ran it -- which is the frame
// the replay has to run it too. It is not a touch and not a key: the shortcut
// calls the cartridge's own function for the button, so it travels as its own
// event, `q`. An older build reading one would take it for a key mask, so
// recordings carrying them say `quick=1` in their header.
extern "C" void lb_quick_replay(int action);   /* src/lb_patches.cpp */
extern "C" void lb_demo_note_quick(int action) {
    if (g_mode != Mode::Record) return;
    const Event e{g_frame, 'q', action, 0, 0, 0};
    g_events.push_back(e);
    emit(e);
}
// Live shortcuts are ignored while a recording plays, like live touches.
extern "C" int lb_demo_playing(void) { return g_mode == Mode::Play ? 1 : 0; }

// docs/384: the opponent selector's rule (-1 not playing, 0 the old six-way,
// 1 eight-way), and the opponent armies.
extern "C" int lb_demo_play_enemy8(void) {
    return g_mode == Mode::Play ? g_demo_enemy8 : -1;
}
// docs/385: the monkey cage fix runs Unit_BuildBillboard in Tier 3, which
// changes the counts, so a replay takes the recording's cage= -- the
// recorder's setting -- and a recording made before it keeps the stock word.
// -1: not playing, so the live setting decides.
extern "C" int lb_demo_play_cage(void) {
    return g_mode == Mode::Play ? g_demo_cage : -1;
}
// docs/385: "Prioritize Own Hero Spells" picks which selected hero drives the
// HUD, and so whose spell a press casts; a replay takes the recorder's choice.
extern "C" int lb_demo_play_ownhero(void) {
    return g_mode == Mode::Play ? g_demo_ownhero : -1;
}
// docs/385: a Custom or Random opponent is named after its hero (1 in full, 2
// shortened); a recording made before that keeps its base faction's name
// (and the counts that go with it).
extern "C" int lb_demo_play_oppname(void) {
    return g_mode == Mode::Play ? g_demo_oppname : -1;
}
// docs/387: Force Tileset and Level Palettes as recorded (-1: not playing,
// so the live settings decide), and the seed of their Random draws -- the
// recording's while one plays, else this run's, drawn once at start-up.
extern "C" int lb_demo_play_tileset(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_tileset : -1;
}
extern "C" int lb_demo_play_palette(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_palette : -1;
}
// docs/388: the Music mode as recorded; -1 when not playing, or playing a
// recording made before music= was written.
extern "C" int lb_demo_play_music(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_music : -1;
}
// docs/392: Free Play's opening opponent, Transport Occupants and Hotwire
// targets as recorded; -1 when not playing or for a recording before sets=2.
extern "C" int lb_demo_play_default_opponent(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_oppdef : -1;
}
extern "C" int lb_demo_play_hotwire_transport(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_hwtransport : -1;
}
extern "C" int lb_demo_play_hotwire_mode(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_hwmode : -1;
}
extern "C" int lb_demo_play_emp_transports(void) {   // docs/406
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_emp : -1;
}
extern "C" int lb_demo_play_cheats(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_cheats : -1;
}
extern "C" int lb_demo_play_ai_cheats(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_aicheats : -1;
}
extern "C" int lb_demo_play_ai_hero_rebuild(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_aihero : -1;
}
// docs/409: called by the frontend with the field size it is about to use.
// Recording, a change is a `f` line; replaying one that has them, the
// recorded size replaces it.
extern "C" void lb_demo_field(int* w, int* h) {
    if (!w || !h) return;
    if (g_mode == Mode::Record) {
        if (*w == g_rec_fw && *h == g_rec_fh) return;
        g_rec_fw = *w;
        g_rec_fh = *h;
        const Event e{g_frame, 'f', *w, *h, 0, 0};
        g_events.push_back(e);
        emit(e);
        return;
    }
    if ((g_mode == Mode::Play || g_replay_done) && g_demo_fw > 0) {
        *w = g_demo_fw;
        *h = g_demo_fh;
    }
}
extern "C" int lb_demo_play_creatures(void) {   // docs/410
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_crea : -1;
}
extern "C" int lb_demo_play_transport_release(void) {
    return (g_mode == Mode::Play || g_replay_done) ? g_demo_trel : -1;
}
extern "C" unsigned lb_demo_pick_seed(void) {
    static const unsigned live = [] {
        const unsigned t = (unsigned)std::time(nullptr);
        return (t ^ (t >> 16) ^ 0x5EEDu) | 1u;
    }();
    return g_mode == Mode::Play ? g_demo_seed : live;
}
extern "C" void lb_demo_note_army(const unsigned char rec[20]) {
    if (g_mode != Mode::Record || !g_rec) return;
    std::fprintf(g_rec, "%u a", g_frame);
    for (int i = 0; i < 10; ++i)
        std::fprintf(g_rec, " %d", rec[2 * i] | (rec[2 * i + 1] << 8));
    std::fprintf(g_rec, "\n");
    std::fflush(g_rec);
}
extern "C" int lb_demo_take_army(unsigned char rec[20]) {
    if (g_mode != Mode::Play || g_army_at >= g_armies.size()) return 0;
    std::memcpy(rec, g_armies[g_army_at++].data(), 20);
    return 1;
}

extern "C" void lb_demo_stop(void);
extern "C" void lb_demo_start_record(const char* path) {
    lb_demo_stop();
    g_events.clear();
    reset_state();
    g_path = path ? path : "demo.lbdemo";
    g_rec = std::fopen(g_path.c_str(), "wb");
    if (!g_rec) {
        g_mode = Mode::Idle;
        g_status = "cannot write " + g_path;
        std::fprintf(stderr, "[lb_demo] cannot write %s\n", g_path.c_str());
        std::fflush(stderr);
        return;
    }
    g_rec_cadence = lb_cadence_effective();
    g_rec_tileset = lb_setting_force_tileset();
    g_rec_palette = lb_setting_level_palette();
    g_rec_music = lb_setting_music_mode();
    g_rec_cage = lb_cage_fix_live() ? 1 : 0;
    g_rec_ownhero = lb_setting_own_hero_spells() ? 1 : 0;
    // docs/392: kinds 5..7 start from the stock values the replay assumes,
    // so the first frames write an `s` line for each that differs.
    g_rec_oppdef = 1;
    g_rec_hwtransport = 0;
    g_rec_hwmode = 0;
    g_rec_emp = 0;                // docs/406
    g_rec_cheats = 0;
    g_rec_aicheats = 0;
    g_rec_aihero = 0;
    g_rec_trel = 0;
    g_rec_crea = 0;
    g_rec_fw = g_rec_fh = 0;      // docs/409: the first frame writes the size
    // docs/396: a session on the PC's clock records the guest clock as it
    // stands now, so a replay runs on the same date (the 12/25 unlock).
    char rtc[24] = "";
    if (lb_rtc_is_host()) {
        unsigned char r[7];
        lb_rtc_get(r);
        std::snprintf(rtc, sizeof rtc, " rtc=%02X%02X%02X%02X%02X%02X%02X",
                      r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
    }
    std::fprintf(g_rec, "lbdemo 1 cadence=%d hudhit=%d camedge=%d camgrid=1 snd=%d "
                 "depth=%d exty=1 hudmemo=1 fp2d=1 fpline=1 quick=1 enemy8=1 cage=%d "
                 "ownhero=%d oppname=2 tileset=%d palette=%d pickseed=%u "
                 "music=%d sets=3 balc=1%s\n",
                 g_rec_cadence, lb_hud_hit_live() ? 1 : 0,
                 lb_camera_edge_live() ? 1 : 0,
                 lb_snd_cull_live() ? 1 : 0,
                 lb_depth_unwrap_live() ? 1 : 0,
                 g_rec_cage, g_rec_ownhero,
                 g_rec_tileset, g_rec_palette, lb_demo_pick_seed(),
                 g_rec_music, rtc);
    std::fflush(g_rec);
    g_mode = Mode::Record;
    g_frame = 0;
    // docs/404: the Balance tab's values in force, as frame-0 `b` lines.
    rec_balance_diff(true);
    g_status = "recording to " + g_path;
    std::fprintf(stderr, "[lb_demo] recording to %s\n", g_path.c_str());
    std::fflush(stderr);
}

extern "C" void lb_demo_start_play(const char* path) {
    g_path = path ? path : "demo.lbdemo";
    if (!load(g_path.c_str())) {
        g_mode = Mode::Idle;
        g_status = "cannot read " + g_path;
        std::fprintf(stderr, "[lb_demo] cannot read %s\n", g_path.c_str());
        return;
    }
    reset_state();
    g_play_at = 0;
    g_frame = 0;
    g_replay_done = false;
    g_mode = Mode::Play;
    // docs/404: a replay runs on its own Balance tab values -- its `b` lines,
    // or none for a recording made before there were any.
    lb_balance_custom_play_begin("");
    // docs/396: the clock the recording ran on, set on the frame recording
    // began (both happen in lb_demo_apply_env, on the first VBlank).
    if (g_demo_has_rtc) {
        lb_rtc_set(g_demo_rtc);
        std::fprintf(stderr, "[lb_demo] guest clock set to the recording's: "
                     "20%02X-%02X-%02X %02X:%02X:%02X\n", g_demo_rtc[0],
                     g_demo_rtc[1], g_demo_rtc[2], g_demo_rtc[4] & 0x3F,
                     g_demo_rtc[5], g_demo_rtc[6]);
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "playing %u event(s)",
                  unsigned(g_events.size()));
    g_status = buf;
    std::fprintf(stderr, "[lb_demo] playing %s, %u event(s), last at frame %u\n",
                 g_path.c_str(), unsigned(g_events.size()),
                 unsigned(g_events.back().frame));
    std::fflush(stderr);
}

extern "C" void lb_demo_stop(void) {
    g_replay_done = false;   // docs/388: a stopped replay hands them back
    if (g_mode == Mode::Record) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "wrote %u event(s) to %s",
                      unsigned(g_events.size()), g_path.c_str());
        g_status = buf;
        std::fprintf(stderr, "[lb_demo] %s\n", buf);
        std::fflush(stderr);
    } else if (g_mode == Mode::Play) {
        g_status = "stopped";
    }
    if (g_rec) {
        std::fclose(g_rec);
        g_rec = nullptr;
    }
    g_mode = Mode::Idle;
}

extern "C" int lb_demo_state(void) {
    return g_mode == Mode::Record ? 1 : (g_mode == Mode::Play ? 2 : 0);
}

extern "C" const char* lb_demo_status(void) { return g_status.c_str(); }
extern "C" unsigned lb_demo_frame(void) { return g_frame; }
extern "C" unsigned lb_demo_event_count(void) {
    return unsigned(g_events.size());
}
extern "C" int lb_demo_play_cadence(void) {
    return g_mode == Mode::Play ? g_demo_cadence : 0;
}
extern "C" int lb_demo_play_hudhit(void) {
    return g_mode == Mode::Play ? g_demo_hudhit : -1;
}
extern "C" int lb_demo_play_camgrid(void) {   /* docs/393 */
    return g_mode == Mode::Play ? g_demo_camgrid : -1;
}
extern "C" int lb_demo_play_camedge(void) {
    return g_mode == Mode::Play ? g_demo_camedge : -1;
}
extern "C" int lb_demo_play_snd(void) {
    return g_mode == Mode::Play ? g_demo_snd : -1;
}
extern "C" int lb_demo_play_depth(void) {
    return g_mode == Mode::Play ? g_demo_depth : -1;
}
extern "C" int lb_demo_play_hudmemo(void) {   /* docs/383 */
    return g_mode == Mode::Play ? g_demo_hudmemo : -1;
}
extern "C" int lb_demo_play_fp2d(void) {      /* docs/383 */
    return g_mode == Mode::Play ? g_demo_fp2d : -1;
}
extern "C" int lb_demo_play_fpline(void) {    /* docs/392 */
    return g_mode == Mode::Play ? g_demo_fpline : -1;
}

// Live touch, on its way to the guest. Returns 0 to DROP the call.
extern "C" int lb_demo_intercept_touch(uint16_t* x, uint16_t* y, int* down) {
    if (g_mode == Mode::Play) return g_injecting ? 1 : 0;
    if (g_mode != Mode::Record) return 1;
    const int dn = *down ? 1 : 0;
    if (dn != g_last_down || (dn && ((int)*x != g_last_x ||
                                     (int)*y != g_last_y ||
                                     g_last_ext != g_rec_ext ||
                                     g_last_ext_y != g_rec_ext_y))) {
        g_last_down = dn;
        g_last_x = (int)*x;
        g_last_y = (int)*y;
        g_rec_ext = g_last_ext;
        g_rec_ext_y = g_last_ext_y;
        Event e{g_frame, 't', dn, g_last_x, g_last_y, g_last_ext};
        e.e = g_last_ext_y;
        g_events.push_back(e);
        emit(e);
    }
    return 1;
}

extern "C" int lb_demo_intercept_keys(uint32_t* mask) {
    if (g_mode == Mode::Play) return g_injecting ? 1 : 0;
    if (g_mode != Mode::Record) return 1;
    if (*mask != g_last_keys) {
        g_last_keys = *mask;
        const Event e{g_frame, 'k', (int32_t)*mask, 0, 0, 0};
        g_events.push_back(e);
        emit(e);
    }
    return 1;
}

// This port's own out-of-band x, published beside the clamped hardware value.
// Recorded on the NEXT touch event rather than as an event of its own: it is
// only ever meaningful together with a touch, and a demo that carried it
// separately could replay the two out of order.
extern "C" void lb_demo_note_extended(int x) {
    if (g_mode == Mode::Record) g_last_ext = x;
}

// ...and its vertical twin (docs/383), recorded the same way.
extern "C" void lb_demo_note_extended_y(int y) {
    if (g_mode == Mode::Record) g_last_ext_y = y;
}

// Once per frame, from lb_patches_on_vblank.
extern "C" void lb_demo_tick(void) {
    if (g_mode == Mode::Idle) return;
    ++g_frame;
    if (g_mode == Mode::Record) {
        // The pacing setting changed mid-recording: say so on this frame, so
        // the replay switches where the player did.
        const int c = lb_cadence_effective();
        if (c != g_rec_cadence) {
            g_rec_cadence = c;
            const Event e{g_frame, 'c', c, 0, 0, 0};
            g_events.push_back(e);
            emit(e);
        }
        // docs/388: and the settings a replay takes from the recording.
        const int now[14] = {lb_setting_force_tileset(),
                            lb_setting_level_palette(),
                            lb_setting_music_mode(),
                            lb_cage_fix_live() ? 1 : 0,
                            lb_setting_own_hero_spells() ? 1 : 0,
                            lb_setting_default_opponent(),     // docs/392
                            lb_setting_hotwire_transport(),
                            lb_setting_hotwire_mode(),
                            lb_emp_transports_live(),          // docs/406
                            lb_cheat_mask_live(),
                            lb_ai_cheats_live(),
                            lb_ai_hero_rebuild_live(),
                            lb_transport_release_live(),       // docs/408
                            lb_creatures_live()};              // docs/410
        int* const was[14] = {&g_rec_tileset, &g_rec_palette, &g_rec_music,
                              &g_rec_cage, &g_rec_ownhero, &g_rec_oppdef,
                              &g_rec_hwtransport, &g_rec_hwmode, &g_rec_emp,
                              &g_rec_cheats, &g_rec_aicheats, &g_rec_aihero,
                              &g_rec_trel, &g_rec_crea};
        for (int k = 0; k < 14; ++k) {
            if (now[k] == *was[k]) continue;
            *was[k] = now[k];
            const Event e{g_frame, 's', k, now[k], 0, 0};
            g_events.push_back(e);
            emit(e);
        }
        rec_balance_diff(false);   // docs/404
        return;
    }
    if (g_mode != Mode::Play) return;

    g_injecting = true;
    while (g_play_at < g_events.size() &&
           g_events[g_play_at].frame <= g_frame) {
        const Event& e = g_events[g_play_at++];
        if (e.kind == 't') {
            // INT32_MIN is "no out-of-band coordinate", and must reach the
            // patch as a CLEAR rather than as a set of some number -- see
            // g_last_ext.
            if (e.d == INT32_MIN) lb_touch_clear_extended();
            else lb_touch_set_extended(e.d);
            if (e.e != INT32_MIN) lb_touch_set_extended_y(e.e);   /* docs/383 */
            nds_set_touch((uint16_t)e.b, (uint16_t)e.c, e.a != 0);
            // docs/383: A RELEASE KEEPS ITS OUT-OF-BAND COORDINATE, as it does
            // live. The cartridge acts on the release edge and reads the
            // coordinate at its own poll, after this returns; clearing it here
            // handed it the clamped panel value instead. A button pressed in a
            // band released at native 0,0, off the button, so the held re-test
            // dropped it and the replay went its own way (run rep383c: the red
            // brick released at 4,0 and its list never opened). Live, the next
            // delivery without one clears it (the one-delivery rule), which
            // is what happens here too. Recordings without `exty=1` keep the
            // old clear, so they replay exactly as before.
            if (!e.a && !g_demo_exty) lb_touch_clear_extended();
        } else if (e.kind == 'c') {
            if (e.a == 20 || e.a == 30) g_demo_cadence = e.a;
        } else if (e.kind == 'q') {
            lb_quick_replay(e.a);   /* docs/383: a battle shortcut, this frame */
        } else if (e.kind == 's') {   /* docs/388: a setting, this frame */
            if (e.a == 0 && e.b >= 0 && e.b <= 4) g_demo_tileset = e.b;
            else if (e.a == 1 && e.b >= 0 && e.b <= 3) g_demo_palette = e.b;
            else if (e.a == 2 && e.b >= 0 && e.b <= 9) g_demo_music = e.b;
            else if (e.a == 3) g_demo_cage = e.b ? 1 : 0;
            else if (e.a == 4) g_demo_ownhero = e.b ? 1 : 0;
            else if (e.a == 5 && e.b >= 0 && e.b <= 7) g_demo_oppdef = e.b;
            else if (e.a == 6 && e.b >= 0 && e.b <= 2) g_demo_hwtransport = e.b;
            else if (e.a == 7 && e.b >= 0 && e.b <= 2) g_demo_hwmode = e.b;
            else if (e.a == 8) g_demo_emp = e.b ? 1 : 0;   // docs/406
            else if (e.a == 9 && e.b >= 0) g_demo_cheats = e.b & 0x3FFFF;
            else if (e.a == 10) g_demo_aicheats = e.b ? 1 : 0;
            else if (e.a == 11) g_demo_aihero = e.b ? 1 : 0;
            else if (e.a == 12) g_demo_trel = e.b ? 1 : 0;   // docs/408
            else if (e.a == 13) g_demo_crea = e.b ? 1 : 0;   // docs/410
        } else if (e.kind == 'b') {   /* docs/404: a Balance tab value */
            lb_balance_custom_play_set(e.a, e.b, e.c);
        } else if (e.kind == 'f') {   /* docs/409: the field's size */
            if (e.a >= 256 && e.b >= 192) { g_demo_fw = e.a; g_demo_fh = e.b; }
        } else {
            nds_set_key_mask((uint32_t)e.a);
        }
    }
    g_injecting = false;

    if (g_play_at >= g_events.size() && g_mode == Mode::Play &&
        g_frame > g_army_last_frame) {   // docs/384: and its last army taken
        g_mode = Mode::Idle;
        g_replay_done = true;            // docs/388: its settings stay
        g_status = "finished";
        std::fprintf(stderr, "[lb_demo] finished at frame %u\n",
                     unsigned(g_frame));
        std::fflush(stderr);
    }
}

// docs/396: a game reset starts the guest's frames over, so a recording
// cannot run on across it -- its frame numbers would no longer mean the same
// frames. A replay stops; a recording closes and a new one begins, from the
// title screen, exactly as a fresh launch would make it.
extern "C" void lb_demo_on_reset(void) {
    const Mode was = g_mode;
    lb_demo_stop();
    lb_balance_custom_play_end();   // docs/404: back to the player's values
    if (was == Mode::Record && !std::getenv("LB_DEMO_NOREC")) {
        char name[64];
        std::snprintf(name, sizeof name, "lb-session-%llu.lbdemo",
                      (unsigned long long)std::time(nullptr));
        lb_demo_start_record(name);
    }
}

// LB_DEMO_REC=path / LB_DEMO_PLAY=path, so a demo can be made and replayed
// without touching the UI -- which matters because a demo of a UI interaction
// should not have to begin with a UI interaction.
extern "C" void lb_demo_apply_env(void) {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* p = std::getenv("LB_DEMO_REC")) lb_demo_start_record(p);
    else if (const char* q = std::getenv("LB_DEMO_PLAY")) lb_demo_start_play(q);
    else if (!std::getenv("LB_DEMO_NOREC")) {
        // RECORD BY DEFAULT, because the alternative is re-deriving the
        // player's route with guessed coordinates and fixed sleeps.
        //
        // Testing with simulated timed inputs that re-create a player's
        // navigation into Free Play is inefficient, and the fix is already
        // here -- this file replays at the guest boundary, frame-indexed, so a
        // route the player walks once can be replayed forever at whatever
        // speed the host manages. What was missing is that recording it took a
        // deliberate act, so it never happened during ordinary play.
        //
        // Input only: touch coordinates and the button mask, the same two
        // calls the frontend already makes into the guest. It costs one line
        // per CHANGE, so a whole session is a few hundred lines.
        //
        // A NAME PER LAUNCH, because a fixed one destroys the recording that
        // mattered. The first version wrote "lb-last-session.lbdemo" and
        // truncated it on every start: a player played a long route, the game
        // was relaunched, and a 12 KB recording became 1 KB before it could be
        // read. A demo that only survives until the next launch is not a
        // record of anything.
        //
        // LB_DEMO_NOREC=1 turns it off.
        char name[64];
        std::snprintf(name, sizeof name, "lb-session-%llu.lbdemo",
                      (unsigned long long)std::time(nullptr));
        lb_demo_start_record(name);
    }
}
