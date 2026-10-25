// lb_overlay.cpp - the LEGO Battles ImGui layer.
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file; see
// port/runner/README.md.
//
// HONESTY RULE: every change to the code that supports the recomp port is
// based on a strict understanding of the decomp, never guessed or pretended.
//
// Every control below is real UI with real persisted state, but most of them
// do not yet reach the simulation, because the ROM work that would tell us
// WHERE to reach has not been done for them. Rather than ship switches that
// look functional and do nothing, each one carries an explicit state:
//
//   WIRED      the control changes the running game
//   READ       the ROM location is known (docs/ cites it) but not hooked up
//   UNSEEN     the whole chain is present and traced -- widget, accessor and
//              consumer -- but it has never been WATCHED taking effect,
//              because the harness cannot stage the situation it needs. It is
//              untested, not known-broken. See the note above the enum.
//   BLOCKED    the ROM IS read and the change is understood, but no mechanism
//              this port has can apply it. docs/143 counted which addresses the
//              recompiler dispatches to: gameplay functions have zero, so an
//              override on them never runs. A control in this state is honest
//              about being inert -- it is not a to-do, it is a dependency on
//              recompiler coverage.
//
// The badge is drawn next to the control. When something becomes WIRED the
// badge goes away. Nothing here fabricates an effect.
//
// UNREAD -- "the ROM has not been read for this yet" -- USED TO BE A FIFTH
// STATE AND IS GONE. Its last two users were the Unlock Santa and Unlock All
// Maps checkboxes, and docs/239 closed them by reading the cheat registry
// rather than by wiring them: 79 of the 155 records share the single string id
// 0x0af1, "Unnamed Cheat Code", and there is no Santa record and no map record
// to set at all. Both controls were REMOVED (see tab_cheats) instead of being
// re-badged, which left the state with no members, no badge renderer reachable
// for it, and nothing in the port naming it. A state no control can be in is
// not a state, so it is deleted rather than kept as an empty slot a future
// sweep has to re-investigate.

#include "lb_overlay.h"
#include "lb_field.h"
#include "io.h"          /* legobattles-decomp: save manager tab */

// The adaptive surface's own geometry, for the corrected minimap box in
// draw_top_screen. Declared here rather than by including gpu3d.h because
// these five are all this file wants, and the band accessors are the same
// ones lb_hud.cpp declares this way.
//
// THE TWO GROUPS HAVE DIFFERENT LINKAGE and it is not cosmetic: gpu3d.h
// carries no extern "C" wrapper, so the nds_gpu3d_* pair are C++ symbols,
// while the lb_* three are defined extern "C" in lb_widemap.cpp. Declaring
// either group the other way links against a name that does not exist.
uint16_t nds_gpu3d_output_width();
uint16_t nds_gpu3d_output_height();
extern "C" {
int lb_band_left(int output_width);        /* src/lb_widemap.cpp */
int lb_band_top(int output_height);        /* src/lb_widemap.cpp */
int lb_widemap_cam_px(int* x, int* y);     /* src/lb_widemap.cpp */
}
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cfloat>   // FLT_MIN, used for a stretch-to-fit button
#include <cmath>    // std::floor/std::ceil: the Top Screen panel's 4:3 snap
#include <chrono>   /* legobattles-decomp: lb_field_bench_tick */
#include <ctime>    /* docs/388: the save cache's timestamps */
#include <atomic>   /* docs/396: the Reset request */
#include <mutex>    /* docs/413: the notice, set from the emulation thread */
#include <thread>   /* docs/430: the update check's own thread */
void lb_mapbrowser_draw(SDL_Renderer* renderer);   /* docs/431: lb_mapfetch.cpp */

// lb_https.cpp (WinHTTP); defined outside any namespace.
bool lb_https_request(const std::string& host, int port, const std::string& method,
                      const std::string& path, const std::string& body,
                      std::string* reply, std::string* error);
extern "C" int lb_demo_playing(void);   /* lb_demo.cpp */
extern "C" void lb_quick_request(int action);   /* lb_patches.cpp */
extern "C" unsigned lb_cancel_slots(void);      /* lb_patches.cpp, SG-0006 */

namespace {

// UNSEEN exists because READ was being used for two different things.
//
// READ means "the ROM location is known but nothing is hooked up". That is a
// gap in the PORT. attack_boost_ranged is not that: its whole chain is present
// and traced -- widget, accessor, and a consumer at lb_patches.cpp that adds
// the delta -- and the only thing missing is having WATCHED it fire. Twelve
// minutes of passive free play produced zero ranged attacks at all (a counter
// in projectile_damage_override proves that, rather than the boost silently
// failing), so the harness cannot exercise it; it needs a battle driven to real
// combat with a boosted ranged unit shooting.
//
// Badging that READ told the player "this control does not affect the game
// yet", which is a stronger claim than the evidence supports and the opposite
// of what the code shows.
// READ and BLOCKED currently have no members either, but unlike UNREAD they
// are still REACHABLE states: a control whose ROM location is cited but not
// hooked up, or one understood and not applicable, can legitimately appear
// again. UNREAD could not -- see the note at the top of the file.
enum Status { WIRED, READ, UNSEEN, BLOCKED };

// The cartridge's own allowance bytes (docs/58, docs/68), shown beside each
// box so a player can see what they are changing from.
constexpr int kShippedCap[10] = {1, 20, 4, 7, 14, 29, 20, 8, 0, 5};

bool g_ready = false;
// Closed on launch. The settings panel is a menu, not part of the game, and it
// covers the field it is meant to configure; F1 or Esc opens it. The Top Screen
// panel is separate (g_top_visible) and stays up, because in Enhanced that
// panel IS the top screen rather than an overlay over it.
bool g_visible = false;
bool g_logged_draw = false;

// ImGui does not rescale itself when the window does. Without this the panel is
// unreadable at 3x and enormous at 1x, because the font and every padding are
// in pixels. Applied on change rather than every frame: rebuilding the style is
// not free and the value only moves when a slider does.
float g_ui_scale = 1.0f;
bool  g_ui_scale_dirty = true;

// ---------------------------------------------------------------------------
// Settings state. Enhanced rendering is the default and the multiplayer name
// defaults to "Player".
// ---------------------------------------------------------------------------
struct Settings {
    // Rendering
    bool enhanced = true;          // "Default enhanced"
    // Move the in-field HUD out to the widened screen's own edges. A corner
    // button belongs in the corner; widening the picture moves the corner.
    // docs/212 classified which 3D quads are HUD by panning the camera.
    // Optional HUD magnification. The in-field buttons are sized for a
    // 256-px screen and look tiny on a wide one.
    float hud_scale = 1.0f;
    // SG-0011: DS Vanilla's two screens, each as a share of the stacked
    // layout (1.0 each is the DS's own, equal sizes).
    float top_scale = 1.0f, bottom_scale = 1.0f;
    // Guest pixels per window pixel. The surface is sized window/scale, so
    // a bigger window spends the extra on FIELD OF VIEW at a constant
    // magnification instead of enlarging the same picture.
    //
    // 2.0, NOT the 4.0 this shipped with, and the change is a bug fix rather
    // than a taste. The sizing is capped from below so the DS's own 256x192
    // always fits, which means the setting is only HONOURED in windows of at
    // least 256*scale x 192*scale. At 4.0 that is 1024x768, so every window
    // up to and including a 1024x768 one showed exactly the DS's field --
    // measured 1.06x, 1.05x, 1.04x across 640x480, 800x600 and 1024x768,
    // with magnification climbing 2.25x -> 3.74x. A window growing 60% and
    // buying nothing but bigger pixels is precisely the fault of the game
    // being simply blown up to the new window size. At 2.0 the cap binds
    // only below 512x384, which no real window is, so the field grows with
    // the window everywhere it can.
    float pixel_scale = 2.0f;
    // THE GAME'S OWN PACING (docs/351). The cartridge re-waits for a VBlank
    // only when a pass took under 20 ms, and under this port's ARM9 timing a
    // pass takes ~14 ms, so the game steps 1, 2, 1, 2 refreshes apart, which
    // reads as moving, then being pulled back. On: a two-frame threshold,
    // passes 2 VBlanks apart (30.4 steps/s measured, 80% even). It is the
    // default despite the cost: the game runs ~19% fewer simulation steps a
    // second than the port did, which is probably the DS's own pace. Off: the
    // cartridge's 20 ms, exactly as emulated.
    bool steady_cadence = true;
    // docs/387: the terrain's palette (0 Per-Mod, 1 Vanilla, 2 FactionWars,
    // 3 Random) and a forced tileset (0 Off, 1 Castle, 2 Pirates, 3 Mars,
    // 4 Random).
    int level_palette = 0;
    int mod_palette = 0;   // docs/410: 0 the same, else Level Palettes' mode + 1
    bool smooth_models = false;   // docs/411: 3D models without edge marking, AA on
    int res3d = 0;                // docs/434: 3D resolution, 0 Native, 1 2x, 2 3x, 3 4x
    // docs/415: online chat. Toasts for received messages, the swear filter
    // (chat and players' names), and the key that opens the chat box.
    bool chat_toasts = true;
    bool swear_filter = true;
    int chat_key = SDL_SCANCODE_T;
    int swap_key = 0;             // SG-0012: swap the two screens; 0 = unbound
    int force_tileset = 0;
    // docs/383: Space and right-click deselect,
    // Mouse 5 opens the blue brick menu and Mouse 4 the red/yellow/white one.
    // Each runs the cartridge's own callback for that button (lb_patches.cpp,
    // view_update_override), not a tap. On by default; false leaves those
    // inputs alone.
    bool quick_keys = true;
    // docs/398: while the blue brick's list is the open one, 1-9 pick from it
    // instead of the second brick's list. On by default.
    bool quick_keys_blue = true;
    // SG-0006: click a unit in a building's queue, on the Top Screen panel, to
    // cancel it. The game's own cancel command; safe online. On by default:
    // it only reaches a button the game already has.
    bool cancel_queue_click = true;
    // docs/384, the Team Builder and Ban Units tabs. A team is the army
    // screen's nine slots as blueprint indices (lb_overlay.h, LbTeam); both
    // start as Free Play's default, King's. Presets are "Name=p0,...,p8;..."
    // and bans a comma list of blueprint indices.
    int team_working[LB_TEAM_SLOTS] = {0, 2, 3, 4, 5, 6, 7, 8, 9};
    int enemy_team[LB_TEAM_SLOTS] = {0, 2, 3, 4, 5, 6, 7, 8, 9};
    char team_presets[2048] = "";
    char banned_units[512] = "";
    // docs/429: map bans ("s5,h<hash>" tokens), the units banned on some maps
    // only ("bp:tok+tok;..."), the ban preset in force and its banned maps,
    // the Random map entries and Random team (tourney). All off/empty = vanilla.
    char banned_maps[512] = "";
    char unit_map_bans[768] = "";
    char ban_preset[96] = "";
    char ban_preset_maps[512] = "";
    bool random_maps = false;
    bool tourney_team = false;
    // docs/388: the last five armies read from the save, newest first, as
    // "YYYY-MM-DD HH:MM=p0,...,p8;...". Kept by save_cache_tick, never by a
    // control: a way back after Set Team (or the game) replaces the army.
    char team_save_cache[640] = "";
    // How the guest surface meets a window of a different shape.
    // false = FILL: scale until the window is covered and crop the excess.
    // true  = FIT : show every rendered column and bar what is left over.
    //
    // STALE AS WRITTEN, and corrected here rather than left to mislead. It
    // said "the port renders 448 columns whatever the window is, so on a 16:9
    // window FILL discards about a hundred of them". That has not been true
    // since docs/215: the surface is sized to the window's own aspect, so
    // fitting and filling produce the same rectangle and there is nothing left
    // to choose between. lb_setting_fit_all() and LB_FIT are removed outright:
    // the accessor had no caller, so the knob it advertised did nothing.
    // `zoom` and `wide_fill` USED TO LIVE HERE and are gone, for the same
    // reason lb_setting_fit_all() was removed: a setting no control can reach
    // is not a setting.
    //
    // Both shaped draw_wide_field, which is a DIAGNOSTIC -- it returns at once
    // unless g_wide_field, and that is only set by LB_WIDE_FIELD. Neither had a
    // widget anywhere in the panel and neither had an accessor, so the only way
    // to give either a non-default value was hand-editing settings.json. They
    // were persisted and loaded, which made them look like options in the file
    // while being unreachable from the UI.
    //
    // The diagnostic keeps both knobs as env overrides beside the switch that
    // turns it on: LB_WIDE_SPAN (DS screens wide, default 3) and LB_WIDE_FIT=1
    // (fit and letterbox instead of cover and crop). See draw_wide_field.

    // Rebalance / mods
    // 0 vanilla, 1 Rebalance, 2 FactionWars, 3 Funlite. The three are
    // third-party romhacks; the port reads the player's own copy rather than
    // shipping their content.
    int  balance_patch = 0;

    // Enhancement toggles (HOST-DECIDED, lobby-carried)
    // One setting, not two checkboxes. "Patch the glitch" and "hotwire
    // anything" are mutually exclusive by definition -- one narrows the target
    // filter and the other removes it -- so a dropdown states that instead of
    // letting both be ticked and needing a warning about which wins.
    int  hotwire_mode = 0;         // 0 vanilla, 1 specials only, 2 anything
    int  hotwire_transport = 0;    // 0 vanilla, 1 kill occupants, 2 convert them
    // docs/406: the Earth/Alien attack spell's EMP disables transports too
    // (lb_patches.cpp, kEmpTransports). Online, the host's.
    bool emp_transports = false;
    // docs/406: the AI rebuilds its hero after every death, not once
    // (lb_patches.cpp, kAiHeroRebuild). Offline only.
    bool ai_hero_rebuild = false;
    // docs/408: a hotwired transport stops taking its old owner's orders
    // (lb_patches.cpp, release_stolen_transport).
    bool transport_release = false;
    // docs/410: the Wolf, Robot and Shark in armies (lb_teams.cpp, cand_on;
    // their prices, lb_patches.cpp kCreaturePrices). Online, the host's.
    bool creatures = false;
    int  attack_boost_ranged = 0;  // 0 vanilla, 1 ranged units benefit too
    bool fix_hotwired_healing = false;
    // docs/385: the monkey cage (cosmetic, this machine only), on by default.
    bool fix_monkey_cage = true;
    // docs/385: with several heroes selected, the HUD's spells are those of
    // the hero from this player's own army, not the first one selected
    // (lb_patches.cpp, hero_hud_override). On by default.
    bool own_hero_spells = true;
    // docs/396: WHICH builder the others match -- 0 off, 1..6 King, Wizard,
    // Pirate, Imperial, Earth, Alien. It was a checkbox for the Imperial one.
    int same_builders = 0;
    // Per build-limit class, defaulted to the SHIPPED bytes rather than to 0.
    //
    // 0 still means "keep the cartridge's byte", and apply_caps() still
    // restores the shipped value when a box is cleared -- the behaviour is
    // identical either way, because the default now IS the shipped value. What
    // changes is what the panel shows on a fresh install: ten zeroes said
    // nothing about what the game actually allows, so the defaults are the
    // real shipped numbers. kShippedCap is the same table the "(shipped %d)"
    // label beside each box reads, so the two cannot drift apart.
    //
    // Classes 5 and 9 keep their entries here even though the panel no longer
    // shows them: the array is indexed by class and lb_setting_cap() is called
    // with a class number. See the note beside kCapName.
    //
    // ELEMENT-BY-ELEMENT FROM kShippedCap, not a second copy of the same ten
    // numbers. A duplicated literal is a literal that drifts, and the panel
    // prints kShippedCap right beside each box -- so a drift would show as a
    // box whose value and whose "(shipped N)" label disagree on a fresh
    // install, which reads as a bug in the game rather than in this line.
    int  cap[10] = {kShippedCap[0], kShippedCap[1], kShippedCap[2],
                    kShippedCap[3], kShippedCap[4], kShippedCap[5],
                    kShippedCap[6], kShippedCap[7], kShippedCap[8],
                    kShippedCap[9]};

    // Cheats.
    //
    // The cartridge's own 18 cheats (docs/171). Each has a dedicated byte in
    // the global flags block, so these are the game's switches rather than
    // anything this port invents. LOBBY-CARRIED: every one of them changes the
    // simulation, so in a multiplayer game the host decides and a joining
    // client must conform or the lockstep desyncs.
    bool cheat[18] = {false};
    // docs/406: the cheats' owner tests pass for every owner, so the AI
    // gets them too (lb_patches.cpp, kAiCheats). Offline only.
    bool ai_cheats = false;
    // docs/433: with ai_cheats, the cheats go to the AI only (not the player).
    bool ai_cheats_only = false;
    // SG-0017: the computer's heroes cast targeted ("tap") spells too
    // (lb_patches.cpp, apply_ai_tap_spells). Offline only.
    bool ai_tap_spells = false;
    // SG-0002: the computer plays the player's side in Free Play, so two
    // computer players fight (lb_patches.cpp, apply_ai_spectate). Offline only.
    bool ai_spectate = false;

    bool unlock_units = false;
    bool unlock_extras = false;   // the cheat codes (red bricks)
    // docs/407: the rest of what a profile earns (lb_patches.cpp,
    // apply_unlocks): the Free Play maps, the concept art, every campaign.
    bool unlock_maps = false;
    bool unlock_art = false;
    bool unlock_campaigns = false;

    // Music
    int  music_mode = 0;           // 0 vanilla, 1 random, 2 faction, 3 map,
                                   // 4..9 always one side (docs/387)
    // docs/427: the notification sound, local only (never the emulated DS).
    int  notify_sound = 1;         // 0 off, 1 Group set (default) .. 8 Magic
    int  notify_volume = 60;       // percent

    // Cosmetic, CLIENT-LOCAL (docs/356): what this machine's text calls the
    // Space Criminal Leader -- 0 the cartridge's own words, 1 Kranxx,
    // 2 Plisken. Never lobby-carried: only screens read the table it edits.
    int  criminal_leader_name = 0;
    // docs/392, the same kind: what this machine's text calls the Pirates'
    // Plantation -- 0 the cartridge's word, 1 Hide-Away, 2 Storehouse.
    int  plantation_name = 0;
    // docs/425, the same kind: 0 the cartridge's word, 1 the new name.
    int  rename_claw_tank = 0;    // Crystal Reaper
    int  rename_gemma = 0;        // Doctor Kelvin
    int  rename_broadside = 0;    // Governor Tyranny
    int  rename_assault_ship = 0; // Dragon Cruiser
    int  rename_attack_ship = 0; // Crystal Extractor
    int  rename_earth_transport = 0; // Recon Dropship
    int  rename_alien_transport = 0; // Hyper Carrier
    int  rename_starfighter = 0; // Switch Fighter
    int  rename_starship1 = 0;  // Rocket
    // SG-0021: the Pirate prototypes' names.
    int  rename_pirate_buildings = 0; // Shanty Shack -> Tavern, Plantation -> Shanty Shack
    int  rename_bounty = 0;           // Brickbeard's Bounty -> Flying Dutchman
    // docs/392: the choice Free Play's opponent screen opens on -- 0..5 the
    // six factions in the game's order (King, Wizard, Pirate, Imperial,
    // Earth, Alien), 6 Custom, 7 Random. 1, Wizard, is the cartridge's own.
    int  default_opponent = 1;

    // Multiplayer
    // docs/394: the public master server, a Cloudflare Worker
    // (services/master-server-cloudflare), on plain HTTP port 80. The local
    // Python one (services/master-server) still works: 127.0.0.1, 28555.
    char master_host[128] = "lbserver.viosarcade.xyz";
    // docs/419: list on and browse both known servers; and HTTPS first.
    // docs/424: Both is the default.
    bool master_both = true;
    // docs/424: set once a person picks a server in the menu; until then a
    // file on the default server is moved to Both (it was the old default).
    bool master_choice = false;
    bool master_https = true;
    bool match_picture = true;     // docs/428
    bool scores_opt_out = false;   // SG-0018: keep out of the score tables
    // Record every session to lb-session-*.lbdemo. Off by default;
    // lb_demo reads it from the file at the first vblank, before the
    // settings load (lb_settings_file_bool).
    bool record_sessions = false;
    bool discord_status = true;    // docs/430: Discord Rich Presence
    bool discord_hero = true;      // docs/430: ... with the hero as the small image
    bool update_check = true;      // docs/430: look for a newer release at startup
    bool auto_update = false;      // docs/435: and install it by itself
    // docs/420: a lobby's mod map this machine lacks is fetched from the archive.
    bool fetch_maps = true;
    bool fast_link = true;      // docs/423: the host-level WH
    int  master_port = 80;
    char player_name[32] = "Player";
    // The multiplayer player name is derived from the save file's name. On,
    // the name advertised is the loaded
    // profile's own -- the cartridge names its local player from that profile
    // (FUN_0204F674 -> 0x02154E2D, copied into the lobby record by
    // FUN_020F5EC8), so it is the name the other players already see in the
    // game's lobby. player_name above is only used when this is off, or
    // before any profile is loaded.
    bool name_from_save = true;
    bool skip_intro = false;      // SG-0016: fast-forward the boot logos to the title
    // The game's own Host screen advertises the lobby,
    // and its own Join screen reaches the master server's lobbies, with no
    // button in this panel. lb_net_auto_tick does both.
    bool auto_advertise = true;
    bool auto_browse = true;
    // docs/405: ask the router to forward the port (UPnP) when hosting.
    bool upnp = true;
    // DELIBERATELY NOT PERSISTED, and this is the only field of this struct
    // that is not. Saying so here so a future sweep does not read the gap as
    // an oversight and "fix" it.
    //
    // It is not a name the player chooses, the way master_host and
    // player_name are. It is a SESSION TOKEN THE SERVER ISSUES: the master
    // server mints four random Crockford-Base32 characters per advertised
    // lobby (services/master-server/master_server.py:59-60 CROCKFORD /
    // CODE_LENGTH, :122-132 new_code), and drops the lobby when nobody has
    // heartbeated it for ninety seconds (:55 TTL_SECONDS, :166-177 expire).
    // The only thing this field is read by is "Join by code", which does a
    // GET /lobby/<code> for a host address (lb_net.cpp:405).
    //
    // So a code restored at the next launch is dead by construction: the
    // lobby it named stopped existing ninety seconds after its host quit.
    // Worse than useless, in fact -- new_code only avoids codes some CURRENT
    // lobby holds, so once expiry frees those four characters the server can
    // and will hand them to a DIFFERENT host. A remembered code would then
    // silently point a player at a stranger's game, which is a wrong answer
    // rather than a stale one.
    //
    // Ephemeral is the correct behaviour, not a missing feature.
    char room_code[8] = "";
};
Settings g_set;

// The name this machine advertises: the loaded profile's when asked for and
// when there is one, otherwise the typed one.
const char* mp_player_name() {
    static char name[32];
    if (g_set.name_from_save && lb_net_profile_name(name, (int)sizeof name) > 0)
        return name;
    return g_set.player_name;
}

// Defined with the wide-field drawing further down; declared here because the
// Display tab needs to say whether Zoom currently has anything to act on.
extern bool g_wide_field;
// The shared dump frame counter, so the panel's whole-map dump and the
// compositor's own dumps number the same frame identically. See lb_widemap.cpp.
extern "C" unsigned lb_dump_frame_get(void);

// src/lb_filedlg.cpp. Its own TU because <windows.h> cannot be included here.
// saving==0 opens an existing file, !=0 chooses a destination. Returns 1 and
// overwrites buf on success; leaves buf alone on cancel.
extern "C" int lb_pick_save_file(char* buf, int cap, int saving);
extern "C" int lb_pick_json_file(char* buf, int cap, int saving);   // docs/404

// ---- persistence ----------------------------------------------------------
//
// Settings and the ImGui layout live BESIDE THE EXECUTABLE, not in the working
// directory: the launcher may be started from anywhere, and a settings file
// that appears wherever the shortcut happened to point is not persistence.
// SDL_GetBasePath() is the executable's directory.
//
// The format is flat JSON written by hand. Pulling in a JSON library for two
// dozen scalars would be a dependency for its own sake, and the reader below
// only has to understand what the writer emits.
namespace {

std::string settings_dir() {
    static std::string dir;
    if (dir.empty()) {
        char* base = SDL_GetBasePath();
        dir = base ? base : "";
        if (base) SDL_free(base);
    }
    return dir;
}

std::string settings_path(const char* leaf) { return settings_dir() + leaf; }

// Scan a flat JSON blob for "key" and return the text just past its colon.
const char* json_find(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t at = s.find(pat);
    if (at == std::string::npos) return nullptr;
    at = s.find(':', at + pat.size());
    if (at == std::string::npos) return nullptr;
    return s.c_str() + at + 1;
}

void json_get_bool(const std::string& s, const char* key, bool* out) {
    if (const char* p = json_find(s, key)) {
        while (*p == ' ') ++p;
        *out = (*p == 't' || *p == '1');
    }
}
void json_get_int(const std::string& s, const char* key, int* out) {
    if (const char* p = json_find(s, key)) *out = std::atoi(p);
}
void json_get_float(const std::string& s, const char* key, float* out) {
    if (const char* p = json_find(s, key)) *out = (float)std::atof(p);
}
void json_get_str(const std::string& s, const char* key, char* out, size_t cap) {
    const char* p = json_find(s, key);
    if (!p) return;
    while (*p && *p != '"') ++p;
    if (!*p) return;
    ++p;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) ++p;      // the writer escapes " and backslash
        out[n++] = *p++;
    }
    out[n] = 0;
}
void json_get_bools(const std::string& s, const char* key, bool* out, int n) {
    const char* p = json_find(s, key);
    if (!p) return;
    while (*p && *p != '[') ++p;
    if (!*p) return;
    ++p;
    for (int i = 0; i < n && *p; ++i) {
        while (*p == ' ' || *p == ',') ++p;
        if (*p == ']' || !*p) break;
        out[i] = (*p == 't' || *p == '1');
        while (*p && *p != ',' && *p != ']') ++p;
    }
}
void json_get_ints(const std::string& s, const char* key, int* out, int n) {
    const char* p = json_find(s, key);
    if (!p) return;
    while (*p && *p != '[') ++p;
    if (!*p) return;
    ++p;
    for (int i = 0; i < n && *p; ++i) {
        while (*p == ' ' || *p == ',') ++p;
        if (*p == ']' || !*p) break;
        out[i] = std::atoi(p);
        while (*p && *p != ',' && *p != ']') ++p;
    }
}

void json_put_str(std::string& o, const char* key, const char* v, bool comma) {
    o += "  \"";
    o += key;
    o += "\": \"";
    for (const char* p = v; *p; ++p) {
        if (*p == '"' || *p == '\\') o += '\\';
        o += *p;
    }
    o += comma ? "\",\n" : "\"\n";
}

}  // namespace

extern "C" void lb_settings_save(void) {
    char buf[128];
    std::string o = "{\n";
    o += "  \"_comment\": \"LEGO Battles port settings. Written beside the "
         "executable.\",\n";
    std::snprintf(buf, sizeof buf, "  \"enhanced\": %s,\n",
                  g_set.enhanced ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"ui_scale\": %.4f,\n", g_ui_scale); o += buf;
    // THE TWO SCALE SETTINGS WERE NOT PERSISTED, and both are on the Display
    // tab beside settings that were -- so a session that changed them and a
    // session that did not wrote the same file, and the panel came back with
    // the sliders reset while everything around them had been remembered.
    //
    // Magnification is the worse of the two to lose, because it is the control
    // that decides whether a bigger window buys field of view (docs/229): a
    // player who turns it down to see more of the battlefield gets 2.0 back
    // only until the next launch. "Wired" has to mean the setting survives the
    // session, not merely that moving the slider does something while it is
    // open.
    std::snprintf(buf, sizeof buf, "  \"pixel_scale\": %.4f,\n",
                  g_set.pixel_scale); o += buf;
    std::snprintf(buf, sizeof buf, "  \"hud_scale\": %.4f,\n",
                  g_set.hud_scale); o += buf;
    std::snprintf(buf, sizeof buf, "  \"top_scale\": %.4f,\n  \"bottom_scale\": %.4f,\n",
                  g_set.top_scale, g_set.bottom_scale); o += buf;
    std::snprintf(buf, sizeof buf, "  \"steady_cadence\": %s,\n",
                  g_set.steady_cadence ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"level_palette\": %d,\n",
                  g_set.level_palette); o += buf;
    std::snprintf(buf, sizeof buf, "  \"mod_palette\": %d,\n",
                  g_set.mod_palette); o += buf;   // docs/410
    std::snprintf(buf, sizeof buf, "  \"smooth_models\": %s,\n",
                  g_set.smooth_models ? "true" : "false"); o += buf;   // docs/411
    std::snprintf(buf, sizeof buf, "  \"res3d\": %d,\n",
                  g_set.res3d); o += buf;   // docs/434
    std::snprintf(buf, sizeof buf, "  \"chat_toasts\": %s,\n",
                  g_set.chat_toasts ? "true" : "false"); o += buf;   // docs/415
    std::snprintf(buf, sizeof buf, "  \"swear_filter\": %s,\n",
                  g_set.swear_filter ? "true" : "false"); o += buf;   // docs/415
    std::snprintf(buf, sizeof buf, "  \"chat_key\": %d,\n",
                  g_set.chat_key); o += buf;   // docs/415
    std::snprintf(buf, sizeof buf, "  \"swap_key\": %d,\n",
                  g_set.swap_key); o += buf;   // SG-0012
    std::snprintf(buf, sizeof buf, "  \"force_tileset\": %d,\n",
                  g_set.force_tileset); o += buf;
    std::snprintf(buf, sizeof buf, "  \"quick_keys\": %s,\n",
                  g_set.quick_keys ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"quick_keys_blue\": %s,\n",
                  g_set.quick_keys_blue ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"cancel_queue_click\": %s,\n",
                  g_set.cancel_queue_click ? "true" : "false"); o += buf;   // SG-0006
    // docs/384
    for (int which = 0; which < 2; ++which) {
        const int* t = which ? g_set.enemy_team : g_set.team_working;
        o += which ? "  \"enemy_team\": [" : "  \"team_working\": [";
        for (int i = 0; i < LB_TEAM_SLOTS; ++i) {
            std::snprintf(buf, sizeof buf, "%s%d", i ? ", " : "", t[i]);
            o += buf;
        }
        o += "],\n";
    }
    json_put_str(o, "team_presets", g_set.team_presets, true);
    json_put_str(o, "team_save_cache", g_set.team_save_cache, true);
    json_put_str(o, "banned_units", g_set.banned_units, true);
    json_put_str(o, "banned_maps", g_set.banned_maps, true);          // docs/429
    json_put_str(o, "unit_map_bans", g_set.unit_map_bans, true);
    json_put_str(o, "ban_preset", g_set.ban_preset, true);
    json_put_str(o, "ban_preset_maps", g_set.ban_preset_maps, true);
    std::snprintf(buf, sizeof buf, "  \"random_maps\": %s,\n",
                  g_set.random_maps ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"tourney_team\": %s,\n",
                  g_set.tourney_team ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"balance_patch\": %d,\n",
                  g_set.balance_patch); o += buf;
    std::snprintf(buf, sizeof buf, "  \"hotwire_mode\": %d,\n",
                  g_set.hotwire_mode); o += buf;
    std::snprintf(buf, sizeof buf, "  \"hotwire_transport\": %d,\n",
                  g_set.hotwire_transport); o += buf;
    std::snprintf(buf, sizeof buf, "  \"emp_transports\": %s,\n",
                  g_set.emp_transports ? "true" : "false"); o += buf;   // docs/406
    std::snprintf(buf, sizeof buf, "  \"ai_hero_rebuild\": %s,\n",
                  g_set.ai_hero_rebuild ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"transport_release\": %s,\n",
                  g_set.transport_release ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"creatures\": %s,\n",
                  g_set.creatures ? "true" : "false"); o += buf;   // docs/410
    std::snprintf(buf, sizeof buf, "  \"attack_boost_ranged\": %d,\n",
                  g_set.attack_boost_ranged); o += buf;
    std::snprintf(buf, sizeof buf, "  \"fix_hotwired_healing\": %s,\n",
                  g_set.fix_hotwired_healing ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"fix_monkey_cage\": %s,\n",
                  g_set.fix_monkey_cage ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"own_hero_spells\": %s,\n",
                  g_set.own_hero_spells ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"same_builders_from\": %d,\n",
                  g_set.same_builders); o += buf;   // docs/396
    o += "  \"cap\": [";
    for (int i = 0; i < 10; ++i) {
        std::snprintf(buf, sizeof buf, "%s%d", i ? ", " : "", g_set.cap[i]);
        o += buf;
    }
    o += "],\n  \"cheat\": [";
    for (int i = 0; i < 18; ++i) {
        o += i ? ", " : "";
        o += g_set.cheat[i] ? "true" : "false";
    }
    o += "],\n";
    std::snprintf(buf, sizeof buf, "  \"ai_cheats\": %s,\n",
                  g_set.ai_cheats ? "true" : "false"); o += buf;   // docs/406
    std::snprintf(buf, sizeof buf, "  \"ai_cheats_only\": %s,\n",
                  g_set.ai_cheats_only ? "true" : "false"); o += buf;   // docs/433
    std::snprintf(buf, sizeof buf, "  \"ai_tap_spells\": %s,\n",
                  g_set.ai_tap_spells ? "true" : "false"); o += buf;   // SG-0017
    std::snprintf(buf, sizeof buf, "  \"ai_spectate\": %s,\n",
                  g_set.ai_spectate ? "true" : "false"); o += buf;   // SG-0002
    std::snprintf(buf, sizeof buf, "  \"unlock_units\": %s,\n",
                  g_set.unlock_units ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"unlock_extras\": %s,\n",
                  g_set.unlock_extras ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"unlock_maps\": %s,\n",
                  g_set.unlock_maps ? "true" : "false"); o += buf;   // docs/407
    std::snprintf(buf, sizeof buf, "  \"unlock_art\": %s,\n",
                  g_set.unlock_art ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"unlock_campaigns\": %s,\n",
                  g_set.unlock_campaigns ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"music_mode\": %d,\n",
                  g_set.music_mode); o += buf;
    std::snprintf(buf, sizeof buf, "  \"notify_sound\": %d,\n",
                  g_set.notify_sound); o += buf;   // docs/427
    std::snprintf(buf, sizeof buf, "  \"notify_volume\": %d,\n",
                  g_set.notify_volume); o += buf;
    std::snprintf(buf, sizeof buf, "  \"criminal_leader_name\": %d,\n",
                  g_set.criminal_leader_name); o += buf;
    std::snprintf(buf, sizeof buf, "  \"plantation_name\": %d,\n",
                  g_set.plantation_name); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_claw_tank\": %d,\n",
                  g_set.rename_claw_tank); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_gemma\": %d,\n",
                  g_set.rename_gemma); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_broadside\": %d,\n",
                  g_set.rename_broadside); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_assault_ship\": %d,\n",
                  g_set.rename_assault_ship); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_attack_ship\": %d,\n",
                  g_set.rename_attack_ship); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_earth_transport\": %d,\n",
                  g_set.rename_earth_transport); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_alien_transport\": %d,\n",
                  g_set.rename_alien_transport); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_starfighter\": %d,\n",
                  g_set.rename_starfighter); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_starship1\": %d,\n",
                  g_set.rename_starship1); o += buf;
    std::snprintf(buf, sizeof buf, "  \"rename_pirate_buildings\": %d,\n",
                  g_set.rename_pirate_buildings); o += buf;   // SG-0021
    std::snprintf(buf, sizeof buf, "  \"rename_bounty\": %d,\n",
                  g_set.rename_bounty); o += buf;
    std::snprintf(buf, sizeof buf, "  \"default_opponent\": %d,\n",
                  g_set.default_opponent); o += buf;
    std::snprintf(buf, sizeof buf, "  \"skip_intro\": %s,\n",
                  g_set.skip_intro ? "true" : "false"); o += buf;   // SG-0016
    std::snprintf(buf, sizeof buf, "  \"name_from_save\": %s,\n",
                  g_set.name_from_save ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"auto_advertise\": %s,\n",
                  g_set.auto_advertise ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"auto_browse\": %s,\n",
                  g_set.auto_browse ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"upnp\": %s,\n",
                  g_set.upnp ? "true" : "false"); o += buf;
    json_put_str(o, "master_host", g_set.master_host, true);
    std::snprintf(buf, sizeof buf, "  \"master_both\": %s,\n",
                  g_set.master_both ? "true" : "false"); o += buf;   // docs/419
    std::snprintf(buf, sizeof buf, "  \"master_choice\": %s,\n",
                  g_set.master_choice ? "true" : "false"); o += buf;   // docs/424
    std::snprintf(buf, sizeof buf, "  \"master_https\": %s,\n",
                  g_set.master_https ? "true" : "false"); o += buf;   // docs/419
    std::snprintf(buf, sizeof buf, "  \"match_picture\": %s,\n",
                  g_set.match_picture ? "true" : "false"); o += buf;   // docs/428
    std::snprintf(buf, sizeof buf, "  \"scores_opt_out\": %s,\n",
                  g_set.scores_opt_out ? "true" : "false"); o += buf;   // SG-0018
    std::snprintf(buf, sizeof buf, "  \"record_sessions\": %s,\n",
                  g_set.record_sessions ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"discord_status\": %s,\n",
                  g_set.discord_status ? "true" : "false"); o += buf;   // docs/430
    std::snprintf(buf, sizeof buf, "  \"discord_hero\": %s,\n",
                  g_set.discord_hero ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"update_check\": %s,\n",
                  g_set.update_check ? "true" : "false"); o += buf;
    std::snprintf(buf, sizeof buf, "  \"auto_update\": %s,\n",
                  g_set.auto_update ? "true" : "false"); o += buf;   // docs/435
    std::snprintf(buf, sizeof buf, "  \"fetch_maps\": %s,\n",
                  g_set.fetch_maps ? "true" : "false"); o += buf;   // docs/420
    std::snprintf(buf, sizeof buf, "  \"fast_link\": %s,\n",
                  g_set.fast_link ? "true" : "false"); o += buf;    // docs/423
    std::snprintf(buf, sizeof buf, "  \"master_port\": %d,\n",
                  g_set.master_port); o += buf;
    // docs/404: the Balance tab's values, "record:field=value,...".
    {
        static char bal[32768];
        lb_balance_custom_text(bal, (int)sizeof bal);
        json_put_str(o, "balance_custom", bal, true);
    }
    json_put_str(o, "player_name", g_set.player_name, false);
    o += "}\n";

    const std::string path = settings_path("settings.json");
    // Write to a temporary and rename, so an interrupted write cannot leave a
    // truncated settings file where a valid one used to be.
    const std::string tmp = path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        std::fwrite(o.data(), 1, o.size(), f);
        std::fclose(f);
        std::remove(path.c_str());
        std::rename(tmp.c_str(), path.c_str());
    }
}

// One on/off setting read straight from settings.json, for a caller that runs
// before lb_settings_load (the session recorder starts at the first vblank).
// `dflt` when the file or the key is missing.
extern "C" int lb_settings_file_bool(const char* key, int dflt) {
    FILE* f = std::fopen(settings_path("settings.json").c_str(), "rb");
    if (!f) return dflt;
    std::string s;
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) s.append(chunk, n);
    std::fclose(f);
    bool v = dflt != 0;
    json_get_bool(s, key, &v);
    return v ? 1 : 0;
}

extern "C" void lb_settings_load(void) {
    const std::string path = settings_path("settings.json");
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::string s;
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) s.append(chunk, n);
    std::fclose(f);

    json_get_bool(s, "enhanced", &g_set.enhanced);
    json_get_float(s, "ui_scale", &g_ui_scale);
    g_ui_scale_dirty = true;
    // Matching the save side; see the note there. A missing key leaves the
    // default, so a settings file written before these were persisted still
    // loads and simply keeps 2.0 and 1.0.
    json_get_float(s, "pixel_scale", &g_set.pixel_scale);
    json_get_float(s, "hud_scale", &g_set.hud_scale);
    json_get_float(s, "top_scale", &g_set.top_scale);          // SG-0011
    json_get_float(s, "bottom_scale", &g_set.bottom_scale);
    // A file written before this key existed keeps the default (steady).
    json_get_bool(s, "steady_cadence", &g_set.steady_cadence);
    json_get_int(s, "level_palette", &g_set.level_palette);   // docs/387
    json_get_int(s, "force_tileset", &g_set.force_tileset);   // docs/387
    if (g_set.level_palette < 0 || g_set.level_palette > 3) g_set.level_palette = 0;
    json_get_int(s, "mod_palette", &g_set.mod_palette);   // docs/410
    json_get_bool(s, "smooth_models", &g_set.smooth_models);   // docs/411
    json_get_int(s, "res3d", &g_set.res3d);   // docs/434
    if (g_set.res3d < 0 || g_set.res3d > 3) g_set.res3d = 0;
    json_get_bool(s, "chat_toasts", &g_set.chat_toasts);       // docs/415
    json_get_bool(s, "swear_filter", &g_set.swear_filter);     // docs/415
    json_get_int(s, "chat_key", &g_set.chat_key);              // docs/415
    if (g_set.chat_key <= 0 || g_set.chat_key >= SDL_NUM_SCANCODES)
        g_set.chat_key = SDL_SCANCODE_T;
    json_get_int(s, "swap_key", &g_set.swap_key);              // SG-0012
    if (g_set.swap_key < 0 || g_set.swap_key >= SDL_NUM_SCANCODES) g_set.swap_key = 0;
    if (g_set.mod_palette < 0 || g_set.mod_palette > 4) g_set.mod_palette = 0;
    if (g_set.force_tileset < 0 || g_set.force_tileset > 4) g_set.force_tileset = 0;
    json_get_bool(s, "quick_keys", &g_set.quick_keys);   // docs/383, default on
    json_get_bool(s, "quick_keys_blue", &g_set.quick_keys_blue);   // docs/398
    json_get_bool(s, "cancel_queue_click", &g_set.cancel_queue_click);   // SG-0006
    // docs/384; a file without them keeps King's team and no presets or bans.
    json_get_ints(s, "team_working", g_set.team_working, LB_TEAM_SLOTS);
    json_get_ints(s, "enemy_team", g_set.enemy_team, LB_TEAM_SLOTS);
    json_get_str(s, "team_presets", g_set.team_presets, sizeof g_set.team_presets);
    json_get_str(s, "team_save_cache", g_set.team_save_cache,
                 sizeof g_set.team_save_cache);   // docs/388
    json_get_str(s, "banned_units", g_set.banned_units, sizeof g_set.banned_units);
    json_get_str(s, "banned_maps", g_set.banned_maps, sizeof g_set.banned_maps);   // docs/429
    json_get_str(s, "unit_map_bans", g_set.unit_map_bans, sizeof g_set.unit_map_bans);
    json_get_str(s, "ban_preset", g_set.ban_preset, sizeof g_set.ban_preset);
    json_get_str(s, "ban_preset_maps", g_set.ban_preset_maps, sizeof g_set.ban_preset_maps);
    json_get_bool(s, "random_maps", &g_set.random_maps);
    json_get_bool(s, "tourney_team", &g_set.tourney_team);
    json_get_int(s, "balance_patch", &g_set.balance_patch);
    json_get_int(s, "hotwire_mode", &g_set.hotwire_mode);
    json_get_int(s, "hotwire_transport", &g_set.hotwire_transport);
    json_get_bool(s, "emp_transports", &g_set.emp_transports);   // docs/406
    json_get_bool(s, "ai_hero_rebuild", &g_set.ai_hero_rebuild);
    json_get_bool(s, "transport_release", &g_set.transport_release);
    json_get_bool(s, "creatures", &g_set.creatures);   // docs/410
    json_get_int(s, "attack_boost_ranged", &g_set.attack_boost_ranged);
    json_get_bool(s, "fix_hotwired_healing", &g_set.fix_hotwired_healing);
    json_get_bool(s, "fix_monkey_cage", &g_set.fix_monkey_cage);   // docs/385
    json_get_bool(s, "own_hero_spells", &g_set.own_hero_spells);   // docs/385
    {   // docs/396: the old checkbox meant "match the Imperial builder" (4)
        bool old = false;
        json_get_bool(s, "same_builders", &old);
        if (old) g_set.same_builders = 4;
        json_get_int(s, "same_builders_from", &g_set.same_builders);
        if (g_set.same_builders < 0 || g_set.same_builders > 6) g_set.same_builders = 0;
    }
    json_get_ints(s, "cap", g_set.cap, 10);
    json_get_bools(s, "cheat", g_set.cheat, 18);
    json_get_bool(s, "ai_cheats", &g_set.ai_cheats);   // docs/406
    json_get_bool(s, "ai_cheats_only", &g_set.ai_cheats_only);   // docs/433
    json_get_bool(s, "ai_tap_spells", &g_set.ai_tap_spells);   // SG-0017
    json_get_bool(s, "ai_spectate", &g_set.ai_spectate);       // SG-0002
    json_get_bool(s, "unlock_units", &g_set.unlock_units);
    json_get_bool(s, "unlock_extras", &g_set.unlock_extras);
    json_get_bool(s, "unlock_maps", &g_set.unlock_maps);           // docs/407
    json_get_bool(s, "unlock_art", &g_set.unlock_art);
    json_get_bool(s, "unlock_campaigns", &g_set.unlock_campaigns);
    json_get_int(s, "music_mode", &g_set.music_mode);
    json_get_int(s, "notify_sound", &g_set.notify_sound);       // docs/427
    json_get_int(s, "notify_volume", &g_set.notify_volume);
    g_set.notify_sound = std::max(0, std::min(8, g_set.notify_sound));
    g_set.notify_volume = std::max(0, std::min(100, g_set.notify_volume));
    // A file written before this key existed keeps the default (off).
    json_get_int(s, "criminal_leader_name", &g_set.criminal_leader_name);
    json_get_int(s, "plantation_name", &g_set.plantation_name);     // docs/392
    json_get_int(s, "rename_claw_tank", &g_set.rename_claw_tank);   // docs/425
    json_get_int(s, "rename_gemma", &g_set.rename_gemma);
    json_get_int(s, "rename_broadside", &g_set.rename_broadside);
    json_get_int(s, "rename_assault_ship", &g_set.rename_assault_ship);
    json_get_int(s, "rename_attack_ship", &g_set.rename_attack_ship);
    json_get_int(s, "rename_earth_transport", &g_set.rename_earth_transport);
    json_get_int(s, "rename_alien_transport", &g_set.rename_alien_transport);
    json_get_int(s, "rename_starfighter", &g_set.rename_starfighter);
    json_get_int(s, "rename_starship1", &g_set.rename_starship1);
    json_get_int(s, "rename_pirate_buildings", &g_set.rename_pirate_buildings);   // SG-0021
    json_get_int(s, "rename_bounty", &g_set.rename_bounty);
    json_get_int(s, "default_opponent", &g_set.default_opponent);   // docs/392
    json_get_str(s, "master_host", g_set.master_host, sizeof g_set.master_host);
    json_get_bool(s, "master_both", &g_set.master_both);     // docs/419
    json_get_bool(s, "master_choice", &g_set.master_choice); // docs/424
    json_get_bool(s, "master_https", &g_set.master_https);   // docs/419
    json_get_bool(s, "match_picture", &g_set.match_picture);   // docs/428
    json_get_bool(s, "scores_opt_out", &g_set.scores_opt_out);   // SG-0018
    json_get_bool(s, "record_sessions", &g_set.record_sessions);
    json_get_bool(s, "discord_status", &g_set.discord_status);   // docs/430
    json_get_bool(s, "discord_hero", &g_set.discord_hero);
    json_get_bool(s, "update_check", &g_set.update_check);
    json_get_bool(s, "auto_update", &g_set.auto_update);   // docs/435
    json_get_bool(s, "fetch_maps", &g_set.fetch_maps);       // docs/420
    json_get_bool(s, "fast_link", &g_set.fast_link);         // docs/423
    json_get_int(s, "master_port", &g_set.master_port);
    // docs/394: a file that still holds the OLD default (the local Python
    // server, which every build wrote before there was a public one) moves to
    // the public server. Anything else was typed by a person and is kept.
    if (std::strcmp(g_set.master_host, "127.0.0.1") == 0 &&
        g_set.master_port == 28555) {
        std::snprintf(g_set.master_host, sizeof g_set.master_host, "%s",
                      "lbserver.viosarcade.xyz");
        g_set.master_port = 80;
    }
    // docs/424: Both became the default. A file nobody chose a server in,
    // still on the default server, follows it; a person's choice is kept.
    if (!g_set.master_choice && g_set.master_port == 80 &&
        std::strcmp(g_set.master_host, "lbserver.viosarcade.xyz") == 0)
        g_set.master_both = true;
    json_get_str(s, "player_name", g_set.player_name, sizeof g_set.player_name);
    {   // docs/404
        static char bal[32768];
        bal[0] = 0;
        json_get_str(s, "balance_custom", bal, sizeof bal);
        // TEST AID: LB_BALANCE_CUSTOM="rec:field=value,..." sets them instead,
        // without writing the player's settings.json from a harness.
        if (const char* t = std::getenv("LB_BALANCE_CUSTOM"))
            std::snprintf(bal, sizeof bal, "%s", t);
        lb_balance_custom_parse(bal);
    }
    // A file written before these keys existed keeps the defaults (all on).
    json_get_bool(s, "name_from_save", &g_set.name_from_save);
    json_get_bool(s, "skip_intro", &g_set.skip_intro);   // SG-0016
    json_get_bool(s, "auto_advertise", &g_set.auto_advertise);
    json_get_bool(s, "auto_browse", &g_set.auto_browse);
    json_get_bool(s, "upnp", &g_set.upnp);   // docs/405, default on

    // Clamp anything a hand-edited file could put out of range.
    if (g_ui_scale < 0.75f || g_ui_scale > 3.0f) g_ui_scale = 1.0f;
    if (g_set.balance_patch < 0 || g_set.balance_patch > lb_balance_pack_count())
        g_set.balance_patch = 0;
    if (g_set.criminal_leader_name < 0 || g_set.criminal_leader_name > 2)
        g_set.criminal_leader_name = 0;
    if (g_set.plantation_name < 0 || g_set.plantation_name > 2)
        g_set.plantation_name = 0;
    if (g_set.rename_claw_tank < 0 || g_set.rename_claw_tank > 1)
        g_set.rename_claw_tank = 0;
    if (g_set.rename_gemma < 0 || g_set.rename_gemma > 1) g_set.rename_gemma = 0;
    if (g_set.rename_broadside < 0 || g_set.rename_broadside > 1)
        g_set.rename_broadside = 0;
    if (g_set.rename_assault_ship < 0 || g_set.rename_assault_ship > 1) g_set.rename_assault_ship = 0;
    if (g_set.rename_attack_ship < 0 || g_set.rename_attack_ship > 1) g_set.rename_attack_ship = 0;
    if (g_set.rename_earth_transport < 0 || g_set.rename_earth_transport > 1) g_set.rename_earth_transport = 0;
    if (g_set.rename_alien_transport < 0 || g_set.rename_alien_transport > 1) g_set.rename_alien_transport = 0;
    if (g_set.rename_starfighter < 0 || g_set.rename_starfighter > 1) g_set.rename_starfighter = 0;
    if (g_set.rename_starship1 < 0 || g_set.rename_starship1 > 1) g_set.rename_starship1 = 0;
    if (g_set.rename_pirate_buildings < 0 || g_set.rename_pirate_buildings > 1)
        g_set.rename_pirate_buildings = 0;
    if (g_set.rename_bounty < 0 || g_set.rename_bounty > 1) g_set.rename_bounty = 0;
    if (g_set.default_opponent < 0 || g_set.default_opponent > 7)
        g_set.default_opponent = 1;
    std::fprintf(stderr, "[lb_settings] loaded %s\n", path.c_str());
}


// ---------------------------------------------------------------------------

// Hover text for the control just emitted.
//
// THE SHIPWRIGHT PATTERN, after the brief hover text of Shipwright's ImGui
// options: a terse label on screen; the explanation only when the pointer asks
// for it.
//
// The rule applied throughout this file: AT MOST THREE LINES in a tooltip, and
// nothing on screen but the control's own name. Everything longer than that is
// design rationale -- why an address was chosen, which doc proved it, what was
// tried and failed -- and it belongs in a code comment or a docs/ entry where
// it is read once by whoever changes the code, not printed at the player every
// time they open a tab.
void help(const char* text) {
    ImGui::SetItemTooltip("%s", text);
}

// KEEP A PANEL INSIDE THE WINDOW.
//
// imgui restores each window's position and size from imgui.ini, and those were
// saved against whatever the application window was at the time. Shrink it --
// which this port positively encourages, the entire adaptive-size feature is
// about resizing -- and a panel remembered from a bigger one hangs off the
// right edge. Captured in exp-mapfix/overlay.png: the tab bar ended in "Mus"
// and every slider label was past the edge, which reads as a truncated UI
// rather than as a stranded window, and sent one fix at the sliders instead.
//
// Call immediately after Begin(). Size is clamped to the viewport first, then
// the position is pulled back, so the whole panel stays reachable.
void keep_on_screen() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float mx = vp->WorkSize.x - 8.0f;
    const float my = vp->WorkSize.y - 8.0f;
    ImVec2 s = ImGui::GetWindowSize();
    if ((mx > 0.0f && s.x > mx) || (my > 0.0f && s.y > my)) {
        if (mx > 0.0f && s.x > mx) s.x = mx;
        if (my > 0.0f && s.y > my) s.y = my;
        ImGui::SetWindowSize(s);
    }
    ImVec2 p = ImGui::GetWindowPos();
    const float x1 = vp->WorkPos.x + vp->WorkSize.x;
    const float y1 = vp->WorkPos.y + vp->WorkSize.y;
    if (p.x + s.x > x1) p.x = x1 - s.x;
    if (p.y + s.y > y1) p.y = y1 - s.y;
    if (p.x < vp->WorkPos.x) p.x = vp->WorkPos.x;
    if (p.y < vp->WorkPos.y) p.y = vp->WorkPos.y;
    ImGui::SetWindowPos(p);
}

// A slider that stops short of the panel edge, so its right-hand label fits.
void slider_width() {
    const float avail = ImGui::GetContentRegionAvail().x;
    float w = avail - 120.0f;
    if (w < avail * 0.45f) w = avail * 0.45f;
    ImGui::SetNextItemWidth(w);
}

void badge(Status s) {
    if (s == WIRED) return;
    ImGui::SameLine();
    if (s == READ) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "[read, not wired]");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The ROM location is known and cited in docs/,\n"
                              "but this control does not affect the game yet.");
    } else if (s == UNSEEN) {
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f),
                           "[wired, not yet seen firing]");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The whole chain is present and traced, but this\n"
                              "has never been watched taking effect, because\n"
                              "the test harness cannot stage the situation it\n"
                              "needs. It is untested, not known-broken.");
    } else {
        // BLOCKED, and the only state left: WIRED returned above and the enum
        // has four members. The trailing branch that used to be here drew
        // "[unread]" for a state that no longer exists -- see the note at the
        // top of the file -- so it could not be reached and is removed with it.
        ImGui::TextColored(ImVec4(0.70f, 0.55f, 0.95f, 1.0f), "[blocked]");
        if (ImGui::IsItemHovered())
            // The old text here said the recompiler never dispatches to
            // gameplay addresses, citing docs/143. That diagnosis is RETRACTED
            // in the tree (lb_patches.cpp, beside kOverrideTable): there are no
            // direct lifted-to-lifted calls at all, every transfer goes through
            // runtime_dispatch, and those addresses were always reachable -- the
            // feature was dead because kOverrideTable was unsorted and broke the
            // binary search, which is fixed and now held by a static_assert.
            // Saying otherwise in the UI repeated a wrong conclusion at the
            // player.
            ImGui::SetTooltip(
                "Understood but not applicable.\n"
                "What this needs is not reachable in the way the port would\n"
                "have to hook it. Needs more coverage, not more research.");
    }
}

// ── Rebindable keyboard input ──────────────────────────────────────────────
//
// frontend.cpp's key_bit() is a twelve-case switch from SDL scancode to DS key
// bit. This table replaces it wholesale rather than being consulted first: a
// partial override would leave the shipped key working alongside the new one,
// so rebinding A from Z to J would give TWO keys that press A.
//
// The defaults below are exactly that switch, which is the reference for what
// "unbound back to normal" means.
struct KeyAction {
    const char* name;
    const char* id;          // stable key for the settings file
    uint16_t bit;
    int default_scancode;
    int default_pad;         // SDL_GameControllerButton
};
const KeyAction kActions[] = {
    {"A",      "a",      1u << 0,  SDL_SCANCODE_Z,         SDL_CONTROLLER_BUTTON_A},
    {"B",      "b",      1u << 1,  SDL_SCANCODE_X,         SDL_CONTROLLER_BUTTON_B},
    // docs/396: X, Y and R moved off S, A and W, which are the camera's
    // W/A/S/D aliases for the D-pad (lb_binding_key_bit).
    {"X",      "x",      1u << 10, SDL_SCANCODE_C,         SDL_CONTROLLER_BUTTON_X},
    {"Y",      "y",      1u << 11, SDL_SCANCODE_V,         SDL_CONTROLLER_BUTTON_Y},
    {"L",      "l",      1u << 9,  SDL_SCANCODE_Q,         SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {"R",      "r",      1u << 8,  SDL_SCANCODE_E,         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {"Start",  "start",  1u << 3,  SDL_SCANCODE_RETURN,    SDL_CONTROLLER_BUTTON_START},
    {"Select", "select", 1u << 2,  SDL_SCANCODE_BACKSPACE, SDL_CONTROLLER_BUTTON_BACK},
    {"Up",     "up",     1u << 6,  SDL_SCANCODE_UP,        SDL_CONTROLLER_BUTTON_DPAD_UP},
    {"Down",   "down",   1u << 7,  SDL_SCANCODE_DOWN,      SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {"Left",   "left",   1u << 5,  SDL_SCANCODE_LEFT,      SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    {"Right",  "right",  1u << 4,  SDL_SCANCODE_RIGHT,     SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
};
constexpr int kActionCount = (int)(sizeof(kActions) / sizeof(kActions[0]));

int g_binding[kActionCount];
int g_pad_binding[kActionCount];
bool g_bindings_ready = false;
int g_rebinding = -1;        // action waiting for a KEY, or -1
int g_rebinding_pad = -1;    // action waiting for a PAD BUTTON, or -1
bool g_rebinding_chat = false;   // docs/415: the chat key waits for a key
bool g_rebinding_swap = false;   // SG-0012: the swap key waits for a key
std::atomic<int> g_screens_swapped{0};   // SG-0012: this session's swap state
bool g_chat_eat_text = false;    // docs/415: swallow the opening key's text

void settings_load();

void bindings_init_once() {
    if (g_bindings_ready) return;
    for (int i = 0; i < kActionCount; ++i) {
        g_binding[i] = kActions[i].default_scancode;
        g_pad_binding[i] = kActions[i].default_pad;
    }
    g_bindings_ready = true;
    settings_load();         // a saved file overrides the defaults
    // docs/396: a file saved with the OLD defaults for X, Y and R (S, A, W)
    // moves to the new ones, so W/A/S/D become the camera keys there too. A
    // binding the player chose themselves is anything else, and is kept.
    int ix = -1, iy = -1, ir = -1;
    for (int i = 0; i < kActionCount; ++i) {
        if (kActions[i].bit == (1u << 10)) ix = i;
        if (kActions[i].bit == (1u << 11)) iy = i;
        if (kActions[i].bit == (1u << 8)) ir = i;
    }
    if (ix >= 0 && iy >= 0 && ir >= 0 && g_binding[ix] == SDL_SCANCODE_S &&
        g_binding[iy] == SDL_SCANCODE_A && g_binding[ir] == SDL_SCANCODE_W) {
        g_binding[ix] = kActions[ix].default_scancode;
        g_binding[iy] = kActions[iy].default_scancode;
        g_binding[ir] = kActions[ir].default_scancode;
        std::fprintf(stderr, "[lb_overlay] bindings: X/Y/R moved from S/A/W to "
                             "C/V/E; W/A/S/D now move the camera\n");
    }
}

// ── Settings persistence ───────────────────────────────────────────────────
//
// A plain key=value file beside the executable. Deliberately not TOML or JSON:
// the runner already parses one TOML file for the title config, and adding a
// parser to store thirty scalars would be more machinery than the job needs.
//
// UNKNOWN KEYS ARE IGNORED and missing ones keep their default, so an older or
// newer file never prevents the port from starting. Bindings are stored by a
// stable id rather than by index, so reordering kActions cannot silently
// reassign somebody's controls.
const char* kSettingsFile = "lb_settings.ini";

// TWO FILES USED TO STORE OVERLAPPING SUBSETS OF THE SAME SETTINGS, and that
// is a bug waiting rather than a redundancy.
//
// `settings.json` beside the executable is written automatically whenever
// g_set changes, and read at overlay init. This .ini was written only when the
// "Save settings" button was pressed, and read at bindings init -- so it was
// usually STALE, and whichever of the two loads ran second decided what the
// player got. The subsets were not equal either: the .ini had the key and pad
// bindings, which the JSON does not, and the JSON had the cheats, unlocks and
// scales, which the .ini did not.
//
// So the .ini keeps the one thing it uniquely owns -- the bindings -- and
// everything else has exactly one home. Existing files still load: the reader
// ignores keys it no longer recognises, which was already its documented
// behaviour, so a settings line left over from the old format is simply
// dropped rather than being allowed to overwrite the JSON's newer value.
void settings_save() {
    std::FILE* fh = std::fopen(kSettingsFile, "w");
    if (!fh) return;
    std::fprintf(fh, "# LEGO Battles port key and controller bindings.\n");
    std::fprintf(fh, "# Every OTHER setting lives in settings.json beside the "
                     "executable, which is written automatically.\n");
    for (int i = 0; i < kActionCount; ++i) {
        std::fprintf(fh, "key_%s=%d\n", kActions[i].id, g_binding[i]);
        std::fprintf(fh, "pad_%s=%d\n", kActions[i].id, g_pad_binding[i]);
    }
    std::fclose(fh);
    // SAY SO. A save button that writes in silence is indistinguishable from
    // one that failed to open the file, which is exactly the report this panel
    // should never provoke.
    std::fprintf(stderr, "[lb_overlay] bindings saved to %s\n", kSettingsFile);
}

void settings_load() {
    std::FILE* fh = std::fopen(kSettingsFile, "r");
    if (!fh) return;
    char line[512];
    while (std::fgets(line, (int)sizeof(line), fh)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* value = eq + 1;
        // strip the newline rather than storing it in a name
        for (char* p = value; *p; ++p)
            if (*p == '\n' || *p == '\r') { *p = '\0'; break; }

        const char* key = line;
        // BINDINGS ONLY. See the note on settings_save: every other key that
        // used to live here now has its single home in settings.json, and an
        // old file's leftover lines are ignored rather than being allowed to
        // overwrite a newer value from there.
        if (!std::strncmp(key, "key_", 4) || !std::strncmp(key, "pad_", 4)) {
            const bool pad = (key[0] == 'p');
            for (int i = 0; i < kActionCount; ++i)
                if (!std::strcmp(key + 4, kActions[i].id)) {
                    (pad ? g_pad_binding : g_binding)[i] = std::atoi(value);
                    break;
                }
        }
    }
    std::fclose(fh);
    std::fprintf(stderr, "[lb_overlay] bindings loaded from %s\n",
                 kSettingsFile);
}

// BR-0011 / BR-0015: while joined, the host's hotwire, build-limit, cheat and
// attack/healing settings are in force; the widgets show them and are locked.
LbHostSettings g_hs;
bool host_locked() { return lb_rules_host_settings(&g_hs) >= 0; }
void host_lock_begin(bool l) { if (l) ImGui::BeginDisabled(); }
void host_lock_end(bool l) { if (l) ImGui::EndDisabled(); }
void host_lock_note(bool l) {
    if (l) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("This is the host's setting, in force while you are in its lobby or match. Yours returns when you leave.");
        ImGui::PopTextWrapPos();
    }
}

void host_note() {
    ImGui::TextDisabled("Host-decided; joining clients conform.");
    ImGui::Separator();
}

// OPEN A CHOSEN TAB, so a run can photograph one without clicking at it.
//
// `LB_PANEL_TAB=Enhancements` (any tab's exact label) selects that tab on the
// first frame the panel is drawn. It exists because photographing a tab was
// otherwise a guessing game and the guessing was silently losing:
// tools/lb_exp.py's --overlay walked a CAPTURED mouse by a relative delta,
// which never reached imgui at all, and the resulting screenshot was simply of
// whichever tab was already open. `exp-savetab2/overlay-tab.png` -- from the
// run named after the Saves tab -- shows the Display tab.
//
// Switching that to the absolute `action="click"` the debug server already
// offers did reach imgui, and immediately proved how fragile aiming is: the
// first corrected click landed on the "DS Vanilla" radio instead of the tab
// row and turned Enhanced OFF, which `settings.json` then persisted. A stray
// click silently disabling the feature under test is exactly the sort of thing
// that invalidates later runs without anyone noticing.
//
// Naming the tab removes the aiming problem entirely.
// docs/396: the menu draws one section at a time (draw_panels), and names it
// here while it does; then a "tab" is simply that section's controls.
const char* g_menu_id = nullptr;

bool tab_begin(const char* label) {
    if (g_menu_id) return std::strcmp(g_menu_id, label) == 0;
    static const char* want = nullptr;
    static bool resolved = false, applied = false;
    if (!resolved) { want = std::getenv("LB_PANEL_TAB"); resolved = true; }
    ImGuiTabItemFlags flags = 0;
    if (want && !applied && std::strcmp(want, label) == 0) {
        flags = ImGuiTabItemFlags_SetSelected;
        applied = true;                 // first frame only, or it cannot be left
    }
    return ImGui::BeginTabItem(label, nullptr, flags);
}

// docs/396: the other half -- a menu section has no tab item to end.
void tab_end() {
    if (!g_menu_id) ImGui::EndTabItem();
}

// docs/427: one or two plain lines at the top of a page: what it does, how to
// use it, and whether it reaches online play or stays on this machine.
void guide(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void tab_display() {
    if (!tab_begin("Display")) return;

    int mode = g_set.enhanced ? 0 : 1;
    ImGui::RadioButton("Enhanced", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("DS Vanilla", &mode, 1);
    g_set.enhanced = (mode == 0);
    // ONE TOGGLE, deliberately. Adaptive window size is a single feature even
    // though several things have to be right for it to work, and splitting it
    // into a row of checkboxes made the parts look optional when they are not.
    // The one thing kept separate is HUD scale, below: how big you want the
    // buttons is a preference, not part of getting the window size right.
    //
    // The two captions this replaced said the field fills the window at its
    // own aspect and named the 1024x512 ceiling. Both are now on the
    // Magnification tooltip, beside the slider that actually decides them.
    badge(WIRED);

    // docs/411: the 3D models (ships, siege, specials) without the outline
    // and with smoothed edges. Off is the game's own look.
    ImGui::Checkbox("Smooth 3D Models", &g_set.smooth_models);
    help("The game draws its ships, siege and special units with a dark\n"
         "outline and no edge smoothing. On, they keep the smoothing and\n"
         "lose the outline. Only this machine sees it.");
    badge(WIRED);

    // docs/434 (BR-0003): the 3D layer drawn at a higher resolution, so the
    // models are sharp when the picture is shown larger than the game draws it.
    {
        const char* res[] = {"Native", "2x", "3x", "4x"};
        slider_width();
        ImGui::Combo("3D resolution", &g_set.res3d, res, IM_ARRAYSIZE(res));
        help("Draws the special units' 3D models (ships, siege, the\n"
             "other specials) at 2, 3 or 4 times the game's own\n"
             "resolution, so their outlines and shapes are sharp in a\n"
             "large window. Everything else is drawn exactly as the game\n"
             "does, and the models' textures stay as the game stores them.\n"
             "2x is free; higher costs speed. Needs Enhanced mode. Only\n"
             "this machine sees it; it does not affect online play.");
        badge(WIRED);
    }

    ImGui::Spacing();
    // LEAVE ROOM FOR THE LABEL. imgui puts a slider's label to its RIGHT and
    // does not shrink the slider to fit it, so at the panel's default width
    // these read "Magnific" and "HUD scal" -- captured in exp-mapviewer's
    // overlay.png. Naming a width that stops short of the edge is what makes
    // the label legible.
    slider_width();
    ImGui::SliderFloat("UI scale", &g_ui_scale, 0.75f, 3.0f, "%.2fx");
    help("Size of this panel. Does not touch the game.");
    // THE ONE MUTATING CONTROL IN THE PANEL WITH NO badge() CALL, on purpose.
    // The badge vocabulary is about whether a control reaches the SIMULATION,
    // and this one is not trying to: it scales the settings panel itself.
    // Badging it WIRED would draw nothing (badge returns early for WIRED) but
    // would assert it belongs to the same audit as Cheats and Build limits,
    // and it does not. Its chain is complete for what it is -- slider ->
    // g_ui_scale -> the "ui_scale" key in settings.json -> the style rebuild
    // in lb_overlay_draw -- and it has no consumer outside this file BY
    // DEFINITION, because there is nothing outside this file for it to reach.
    if (ImGui::IsItemDeactivatedAfterEdit()) g_ui_scale_dirty = true;

    ImGui::Spacing();
    slider_width();
    ImGui::SliderFloat("Magnification", &g_set.pixel_scale, 1.0f, 8.0f,
                       "%.1fx");
    // The seven help lines this replaced spelled out both caps at length. The
    // detail belongs here, where it is available to anyone changing the code
    // and is not read out at the player every time the tab is opened:
    //
    //   * Upper cap: the renderer tops out at 1024x512 guest pixels -- a
    //     vertex-format limit, not a preference -- so past that point a larger
    //     window scales the picture up again instead of showing more field.
    //   * Lower cap: the DS's own 256x192 must always fit, so a window smaller
    //     than 256x192 times this setting cannot honour it and drops to
    //     whatever does fit. At the old default of 4.0 that made several window
    //     sizes show the SAME field at different zooms -- the fault,
    //     and the reason the field-of-view readout below sits beside this.
    help("Guest pixel size. Field = window / this, so a bigger window\n"
         "shows MORE field rather than the same view blown up.\n"
         "Capped: 1024x512 guest pixels max, and 256x192 must fit.");
    badge(WIRED);
    // SHOW THE THREE NUMBERS THE SETTING ACTUALLY CONTROLS.
    //
    // A magnification slider whose effect is invisible until you resize the
    // window and squint is an unfinished control, and this one hid the exact
    // failure the setting exists to fix: at the old default of 4.0 a 640x480 and a
    // 1024x768 window both showed 1.0x the DS's field, one of them merely
    // larger. Printing field-of-view beside magnification makes that legible
    // at a glance -- FOV 1.00x with the window growing IS "blown up".
    {
        int fx = 0, fy = 0, fw = 0, fh = 0, gw = 0, gh = 0;
        if (lb_overlay_field_rect(&fx, &fy, &fw, &fh, &gw, &gh) && gw > 0 &&
            gh > 0) {
            ImGui::Text("  now: %dx%d guest pixels, %.2fx magnification, "
                        "%.2fx / %.2fx the DS's field",
                        gw, gh, (double)fw / (double)gw,
                        (double)gw / 256.0, (double)gh / 192.0);
            if (gw >= 1024 || gh >= 512)
                ImGui::TextDisabled("  at the 1024x512 ceiling: a larger "
                                    "window now scales up instead of showing "
                                    "more.");
        }
    }
    ImGui::Spacing();
    slider_width();
    ImGui::SliderFloat("HUD scale", &g_set.hud_scale, 1.0f, 3.0f, "%.2fx");
    // The in-field buttons are 3D geometry, not sprites -- OBJ is disabled
    // outright in gameplay -- and they are authored for a 256-px screen, so
    // they look tiny on a big one. Kept separate from Enhanced on purpose:
    // how big you want the buttons is a taste, not part of making the window
    // size work.
    help("The in-field buttons are drawn for a 256-px screen.\n"
         "Scaling pins the anchored corner and grows inward.");
    badge(WIRED);

    if (!g_set.enhanced) {   // SG-0011: DS Vanilla's two screens
        ImGui::Spacing();
        struct Row { const char* label; const char* id; float* v; };
        const Row rows[2] = {{"Top screen size", "##top_scale", &g_set.top_scale},
                             {"Bottom screen size", "##bottom_scale", &g_set.bottom_scale}};
        for (const Row& r : rows) {
            float pct = *r.v * 100.0f;
            ImGui::PushID(r.id);
            {   // a slider, then a box to type a value, then the label
                const float avail = ImGui::GetContentRegionAvail().x;
                float w = avail - 230.0f;
                if (w < 120.0f) w = 120.0f;
                ImGui::SetNextItemWidth(w);
                if (ImGui::SliderFloat("##sl", &pct, 25.0f, 200.0f, "%.0f%%"))
                    *r.v = pct / 100.0f;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::InputFloat("##typed", &pct, 0.0f, 0.0f, "%.0f"))
                    *r.v = pct / 100.0f;
                ImGui::SameLine();
                ImGui::TextUnformatted(r.label);
            }
            ImGui::PopID();
            if (*r.v < 0.25f) *r.v = 0.25f;
            if (*r.v > 2.0f) *r.v = 2.0f;
            help("How big this screen is next to the other, in DS Vanilla. 100%\n"
                 "each is the DS's own equal screens; the pair always fits the\n"
                 "window, and clicks follow the bottom screen. Ctrl+click a\n"
                 "slider, or use the box beside it, to type a value.\n"
                 "This machine only; no effect online.");
        }
        if (ImGui::Button("Reset screen sizes")) g_set.top_scale = g_set.bottom_scale = 1.0f;
        badge(WIRED);
    }

    ImGui::Spacing();
    ImGui::Checkbox("Steady game pacing (30 Hz)", &g_set.steady_cadence);
    // docs/351. The cartridge's loop re-waits for a VBlank only when a pass
    // took under 20 ms; this port's ARM9 finishes a pass in ~14 ms, so the
    // game steps 1, 2, 1, 2 refreshes apart. On: two refreshes every pass.
    // Chain: checkbox -> g_set.steady_cadence -> "steady_cadence" in
    // settings.json -> lb_setting_steady_cadence -> lb_cadence_effective ->
    // get_ms_override (lb_patches.cpp), consulted on every pass. A replay
    // plays at the pacing it was recorded with, whatever this says.
    help("Even steps instead of the uneven 1-2-1-2 the cartridge takes\n"
         "at this port's CPU speed. The game runs ~19% fewer steps a\n"
         "second -- probably the DS's own pace. Off: the cartridge's.");
    badge(WIRED);

    // docs/387: a Level Palettes option (Per-Mod/Vanilla/FactionWars/Random,
    // default Per-Mod) and Force Tileset (Off/Castle/Pirates/Mars/Random,
    // default Off). lb_palette.cpp rewrites
    // the level palette's main-RAM copy the game uploads from, so it changes
    // during a battle; lb_patches.cpp's map_parse_override renames the map's
    // tileset as it loads, so that one applies from the next battle.
    ImGui::Separator();
    guide("Level Palettes: the colours the maps are drawn in. Pick one and start a "
          "battle. This is local only: other players do not see it.");
    const char* palettes[] = {"Per-Mod", "Vanilla", "FactionWars / FunLite2", "Random"};
    ImGui::Combo("Level Palettes", &g_set.level_palette, palettes,
                 IM_ARRAYSIZE(palettes));
    help("FunLite2's Pirate and Mars palettes are these exactly; its King\n"
         "palette is the same except that its water colours are black,\n"
         "which this does not copy.\n"
         "The terrain's colours. FactionWars is that romhack's recolour of\n"
         "the three tilesets (REbalance ships the same); Random picks one\n"
         "per level. Per-Mod is the ROM's own palettes for now: a mod map\n"
         "found in several mods has no single palette of its own.\n"
         "Changes at once, even mid-battle. Only this machine sees it.");
    badge(WIRED);
    // docs/410: a separate choice for the mods folder's maps.
    const char* modpals[] = {"Same as Level Palettes", "Per-Mod", "Vanilla",
                             "FactionWars / FunLite2", "Random"};
    ImGui::Combo("Mod Map Palettes", &g_set.mod_palette, modpals,
                 IM_ARRAYSIZE(modpals));
    help("The colours of the maps from the mods folder. Same as Level\n"
         "Palettes follows the choice above; any other keeps mod maps on\n"
         "their own choice, so FactionWars colours on the stock maps can\n"
         "leave the mods' maps as they were.");
    badge(WIRED);
    const char* tilesets[] = {"Off", "Castle", "Pirates", "Mars", "Random"};
    ImGui::Combo("Force Tileset", &g_set.force_tileset, tilesets,
                 IM_ARRAYSIZE(tilesets));
    help("Draws every map with this world's tileset (Random: one per\n"
         "level). Gameplay is unchanged -- terrain, water and cliffs come\n"
         "from the map. Applies from the next battle, single player only.");
    badge(WIRED);
    // docs/391: what the mods folder gave, read once at launch.
    ImGui::Separator();
    ImGui::TextDisabled("Mod maps: %s", lb_mods_status());
    help("Put mod ROMs (.nds) in a folder named \"mods\" beside the game.\n"
         "Every multiplayer map a mod replaced is added after the 30 stock\n"
         "maps in Free Play's map list, with its own name, preview and\n"
         "player count. Maps several mods share are listed once. Read when\n"
         "the game starts. Not offered when hosting a multiplayer game yet:\n"
         "every player would need the same maps.");
    badge(WIRED);
    ImGui::Separator();
    // SG-0016: the logos run on the game's own clock and cannot be tapped
    // through, so the boot is simply run at full speed until the title shows.
    ImGui::Checkbox("Skip the intro screens", &g_set.skip_intro);
    help("Runs the opening logos at full speed so the game reaches its title\n"
         "screen at once. Applies at start-up and after Reset. Local to this\n"
         "machine; online play is unaffected.");
    badge(WIRED);
    tab_end();
}

void draw_balance_pack();         // docs/404, below
void draw_after_balance_pack();
void tab_enhancements() {
    if (!tab_begin("Enhancements")) return;
    host_note();

    // docs/392: a drop-down for the default Free Play opponent, since the
    // game's Free Play menu always defaults to Wizard. The
    // portrait's frame is the whole of the choice (FUN_020C0374 reads only
    // it), and the Wizard comes from UI/FE/OpponentSetup.bin; lb_patches.cpp
    // sets it as the screen finishes building (opponent_setup_override).
    ImGui::Text("Default Free Play Opponent");
    // docs/393: the dropdown's random choice is labelled "Randomizer".
    const char* opponents[] = {"King", "Wizard", "Pirate", "Imperial",
                               "Earth", "Alien", "Custom", "Randomizer"};
    ImGui::Combo("##default_opponent", &g_set.default_opponent, opponents,
                 IM_ARRAYSIZE(opponents));
    help("The opponent Free Play's opponent screen opens on. The arrows\n"
         "still cycle as before. Wizard is the game's own default; Custom\n"
         "is the Team Builder's enemy team, Randomizer a new army each\n"
         "match.");
    badge(WIRED);
    ImGui::Separator();

    // docs/406: FUN_0208A5C0 freed a dead hero's build record from the army
    // list instead of the hero's, so the AI's second hero was its last.
    ImGui::Checkbox("The AI Rebuilds Its Hero Every Time", &g_set.ai_hero_rebuild);
    help("The computer rebuilds its hero once, then never again: the game\n"
         "loses track of the second one. On, it rebuilds after every death.\n"
         "Free Play only.");
    badge(WIRED);
    // SG-0002: AI vs AI.
    ImGui::Checkbox("AI Plays My Side (Spectate)", &g_set.ai_spectate);
    help("Free Play only: the computer plays your side as well, so two\n"
         "computer players fight and you watch. The map is revealed.\n"
         "You can still give orders to your units. Takes effect when a\n"
         "battle starts. Not online.");
    badge(WIRED);
    ImGui::Spacing();
    ImGui::Text("Attack Boost Reaches Ranged Units");
    const char* atk_modes[] = {"Off (vanilla)", "On",
                               "On, shots only (no blast)"};
    {
        const bool lk = host_locked();
        host_lock_begin(lk);
        int atk = lk ? g_hs.attack_boost : g_set.attack_boost_ranged;
        ImGui::Combo("##attack_boost_ranged", &atk, atk_modes, IM_ARRAYSIZE(atk_modes));
        if (!lk) g_set.attack_boost_ranged = atk;
        host_lock_end(lk);
        host_lock_note(lk);
    }
    help("Vanilla buffs melee damage only; ranged keeps the projectile's own.\n"
         "On adds the same delta to a projectile's hit in flight.\n"
         "Shots only adds it to every shot that lands on its target too,\n"
         "but never to the splash of a boulder, fireball or cannonball.\n"
         "Online, the host's setting applies to everyone.");
    badge(UNSEEN);
    // The reasoning behind this setting lived in TEN TextDisabled lines, which
    // the panel then followed with a two-line summary of the same thing.
    // Labels are kept short, so the detail moved here, where it is still
    // readable by anyone changing the code and is not read out at the player
    // every time they open the tab:
    //
    //   * entity+0x170 is the runtime melee damage and equals blueprint+0x68
    //     exactly (docs/146). Melee damage comes from the attacker's buffed
    //     stats; ranged damage comes from the projectile's OWN blueprint,
    //     which never reads them.
    //   * Ranged units have melee_damage 0, so a naive buff would zero them.
    //     "On" therefore adds the buff's DELTA -- buffed minus the blueprint's
    //     melee_damage -- to the shot, so a hero with melee_damage 0 keeps its
    //     projectile damage instead of being zeroed (docs/95).
    //   * The override is installed and the dispatch mechanism is proven
    //     (docs/175), but projectile_damage_override has not been SEEN firing.
    //
    //     THE REST OF THIS NOTE USED TO SAY the address has zero dispatching
    //     call sites (docs/143) so an override there never runs. That is
    //     retracted in the tree beside kOverrideTable: there are no direct
    //     lifted-to-lifted calls anywhere, every transfer goes through
    //     runtime_dispatch, and the feature was dead because kOverrideTable was
    //     unsorted. It also promised a caveat "below in the UI" that no longer
    //     exists. So the note argued the opposite of what the code shows.
    //
    //     WHAT IS ACTUALLY TRUE: the chain is complete -- widget, accessor, and
    //     a consumer that adds the delta -- and nothing has watched it fire. A
    //     counter added to projectile_damage_override shows why: twelve minutes
    //     of passive free play produced ZERO ranged attacks, so the boost had
    //     nothing to act on. Settling it needs a battle driven to real combat
    //     with a boosted ranged unit shooting. Badged UNSEEN, not READ.
    ImGui::Spacing();

    {   // docs/396: a choice of builder, not only the Imperial one
        static const char* const kFrom[7] = {"Off", "King", "Wizard", "Pirate",
                                             "Imperial", "Earth", "Alien"};
        ImGui::Combo("Make All Builders The Same", &g_set.same_builders, kFrom, 7);
    }
    // With a balance patch active this equalises to the PATCHED builder,
    // because the patch is applied first. See apply_same_builders.
    help(g_set.balance_patch != 0
         ? "Every builder takes the chosen one's 14 stats, as patched.\n"
           "Takes effect at once outside a battle or lobby; Off undoes it."
         : "Every builder takes the chosen one's 14 stats (speed, cost,\n"
           "build time, health, damage, yield, range...).\n"
           "Takes effect at once outside a battle or lobby; Off undoes it.");
    badge(WIRED);
    // docs/386: a change made in a battle or lobby waits; say so.
    if (lb_patches_blueprint_base() &&
        lb_same_builders_applied() != lb_setting_same_builders()) {
        const char* why = lb_blueprint_edit_blocked();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("  waiting: %s", why ? why : "next frame");
        ImGui::PopTextWrapPos();
    }


    ImGui::Separator();
    // docs/404: the packs moved to the Balance menu, beside the player's own
    // values (draw_balance_pack).
    ImGui::Text("Balance Patch: %s", lb_balance_name(g_set.balance_patch));
    ImGui::SameLine();
    ImGui::TextDisabled("(set it, and every unit's values, in the Balance menu)");

    // COSMETIC AND THIS MACHINE'S ONLY -- see below.
    draw_after_balance_pack();
}

// SG-0001: the hotwire, EMP and healing options, in Balance > Spells.
void tab_balance_spells() {
    if (!tab_begin("Balance Spells")) return;
    guide("Rules for the Hotwire, EMP and Heal spells. Online, the host's choices apply to everyone.");
    ImGui::Text("Hotwire targets");
    const char* hotwire_modes[] = {
        "Vanilla",
        "Specials only",
        "Anything (cheat)"};
    const bool hw_lock = host_locked();   // BR-0011
    host_lock_begin(hw_lock);
    {
        int hm = hw_lock ? g_hs.hotwire_mode : g_set.hotwire_mode;
        ImGui::Combo("##hotwire_mode", &hm, hotwire_modes, IM_ARRAYSIZE(hotwire_modes));
        if (!hw_lock) g_set.hotwire_mode = hm;
    }
    // docs/392: the targeting rule is in the stylus tap (func_020A9350's
    // filter, tags 0x2B enemy + 0x3B transport-or-special), not only in the
    // validator func_0207B73C (docs/236's farm cap). Anything requires 0x2B
    // twice, so a tap takes any enemy, and also drops the farm cap; Specials
    // only refuses, as the validator's 0 would, a hotwire whose target is not
    // category 5 or 6, which is exactly the glitch (cast_spell_override).
    help("Vanilla aims Hotwire at enemy transports and specials; the glitch\n"
         "steals whatever is under a special's corner. Specials only refuses\n"
         "that glitch. Anything (cheat) lets Hotwire target any enemy unit,\n"
         "hero or building directly, and drops the farm limit.");
    badge(WIRED);

    ImGui::Spacing();
    ImGui::Text("Transport Occupants");
    const char* transport_modes[] = {
        "Off (vanilla)",
        "Destroy them",
        "Hotwire them"};
    // docs/392: both live. When a transport is hotwired its occupants are
    // destroyed with the game's own immediate kill (FUN_0205E78C, the call a
    // dying transport makes on its occupants) or handed to the new owner by
    // its own SetOwner, one guest call at a time (set_owner_override). The
    // greyed-out "Destroy them" waited on a resumable call chain; the hop
    // pattern of docs/383 is that.
    if (g_set.hotwire_transport < 0 || g_set.hotwire_transport > 2)
        g_set.hotwire_transport = 0;
    {
        int ht = hw_lock ? g_hs.hotwire_transport : g_set.hotwire_transport;
        ImGui::Combo("##hotwire_transport", &ht, transport_modes,
                     IM_ARRAYSIZE(transport_modes));
        if (!hw_lock) g_set.hotwire_transport = ht;
    }
    help("What happens to the units inside a transport when it is hotwired.\n"
         "Off leaves them the old owner's, as the game does.");
    badge(WIRED);
    // docs/408: the computer player that owned it kept ordering it about.
    {
        bool tr = hw_lock ? g_hs.transport_release != 0 : g_set.transport_release;
        ImGui::Checkbox("Hotwired Transports Leave Their Old Owner", &tr);
        if (!hw_lock) g_set.transport_release = tr;
    }
    help("In the game, a transport stolen from the computer still sails\n"
         "where its old owner sends it, and is let go when that order\n"
         "ends. On, the old owner forgets it the moment it is hotwired.");
    badge(WIRED);
    ImGui::Spacing();
    // docs/406: FUN_020775F0 collects the EMP's victims by category 6 only;
    // the patch lets category 5 (transports) in as well.
    ImGui::Checkbox("EMP Disables Transports", &g_set.emp_transports);
    help("The EMP spell disables special units only.\n"
         "On, it disables transports caught in it too. Online, the host's\n"
         "setting applies to everyone.");
    badge(WIRED);
    if (lb_rules_host_flags() >= 0)
        ImGui::TextDisabled("In this lobby, the host's: %s",
                            lb_emp_transports_live() ? "On" : "Off");
    ImGui::Spacing();

    {
        bool fh = hw_lock ? g_hs.fix_healing != 0 : g_set.fix_hotwired_healing;
        ImGui::Checkbox("Fix Hotwired Healing", &fh);
        if (!hw_lock) g_set.fix_hotwired_healing = fh;
    }
    help("HealSpell skips hotwired units; On removes that filter.\n"
         "Online, the host's setting applies to everyone.");
    badge(WIRED);
    host_lock_end(hw_lock);
    host_lock_note(hw_lock);

    // SG-0017 step 1: the AI's single-target spells.
    ImGui::Spacing();
    ImGui::Checkbox("AI Uses Targeted Spells", &g_set.ai_tap_spells);
    help("The computer's heroes also cast the spells you aim at one unit\n"
         "(damage spells, unit boosts), and Hotwire and EMP, which the game's\n"
         "AI never chooses. In the game they only cast spells that hit the\n"
         "ground or themselves. Not online.");
    badge(WIRED);
    tab_end();
}

// SG-0001: the build limits, in Balance > Unit Caps.
void tab_balance_caps() {
    if (!tab_begin("Balance Caps")) return;
    guide("How many heroes, units, towers and buildings each player may have. Online, the host's limits apply to everyone.");
    ImGui::Text("Build limits");
    // The allowance table at 0x02126D94 is one byte per build-limit CLASS,
    // not per unit type, which is why the names below are classes.
    help("0 keeps the cartridge's own value; max 255.\n"
         "One limit per build class, not per unit type.\n"
         "Live and reversible: clearing a box restores the shipped byte.");
    // THE THREE UNNAMED CLASSES, SETTLED BY READING func_020860A8.
    //
    // docs/58 and docs/68 left 4, 5 and 9 unnamed, and the boxes went out
    // labelled "class 4", "class 5" and "class 9" -- three numbers the player
    // was invited to change with nothing to tell them what they did. The
    // dispatcher answers all three.
    //
    // It is a jump table on the class. At 020860C4 `addls pc, pc, fp, lsl #2`
    // reads PC as 020860CC, so the arms are:
    //
    //     0 -> 20860F0   1 -> 2086140   2 -> 20861B4   3 -> 2086210
    //     4 -> 2086254   5 -> 2086348   6 -> 2086104   7 -> 2086118
    //     8 -> 208612C   and `cmp fp,#8` drops 9 through to 20860C8 -> 2086348
    //
    // CLASSES 5 AND 9 ARE INERT, and that is not an inference. 2086348 is
    //
    //     mov r0, #0
    //     pop {...pc}
    //
    // -- it returns zero without ever touching the allowance byte, while every
    // other arm exits via 2086350, which loads the byte and returns
    // allowance - used. Class 5 is routed there by the table and class 9 is out
    // of the table's range, so both land on it. Their shipped bytes, 29 and 5,
    // are never read by anything. Raising them cannot raise a limit, because
    // no limit is being consulted. So the boxes are GONE rather than relabelled
    // "does nothing" -- a control that cannot act is worse than no control.
    //
    // CLASS 4 IS THE BUILDING CAP. Its arm walks the owner's entities and
    // counts by the category byte at +0x5C, through two filters: the first
    // (sub #0xd / cmp #6) drops 13, 14, 15, 17 and 19; the second (cmp #0x13)
    // keeps only 8..19. What survives both is 8, 9, 10, 11, 12, 16 and 18 --
    // Mill, Mine, Farm, Barracks, VehicleCenter, Shipyard, Gate, per the
    // measured table in tools/lb_blueprints.py. The categories it drops are
    // exactly the ones with classes of their own: Towers (13-15, class 3),
    // Bridges (17, class 7), Walls (19, class 6). Units and the Base (0-7) are
    // never counted by it.
    //
    // docs/405: NO "GATES" LINE. Class 8 counts gates (GateH/GateV, category
    // 18, blueprints 127/128), and the cartridge ships it at 0: no faction's
    // roster has a gate, and nothing in the game places one (docs/367: the
    // factory builds a Sim::Gate for category 18, which no build list
    // offers). A limit on a building nobody can build was only confusing.
    static const char* kCapName[10] = {
        "Heroes", "Minifigures", "Specials", "Towers", "Buildings",
        nullptr, "Walls", "Bridges", nullptr, nullptr};
    const bool cap_lock = host_locked();
    host_lock_note(cap_lock);
    for (int i = 0; i < 10; ++i) {
        if (!kCapName[i]) continue;          // 5, 8 and 9: see above
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(120.0f);
        if (cap_lock) {
            int hv = g_hs.caps[i];   // BR-0015: the host's, locked
            ImGui::BeginDisabled();
            ImGui::InputInt(kCapName[i], &hv);
            ImGui::EndDisabled();
        } else {
            ImGui::InputInt(kCapName[i], &g_set.cap[i]);
            if (g_set.cap[i] < 0) g_set.cap[i] = 0;
            if (g_set.cap[i] > 255) g_set.cap[i] = 255;
        }
        if (i == 4)
            help("Mill, Mine, Farm, Barracks, Vehicle Center and Shipyard.\n"
                 "Towers, Walls and Bridges have their own limits below;\n"
                 "the Base and all units are counted elsewhere.");
        ImGui::SameLine();
        ImGui::TextDisabled("(shipped %d)", kShippedCap[i]);
        ImGui::PopID();
    }
    badge(WIRED);
    tab_end();
}

// docs/404: the balance packs, in Balance > Presets.
void draw_balance_pack() {
    ImGui::Text("Balance Patch");
    ImGui::Combo("##balance_patch", &g_set.balance_patch,
                 [](void*, int i) { return lb_balance_name(i); }, nullptr,
                 lb_balance_pack_count() + 1);
    // Only the STAT changes are bundled. These mods also alter maps and
    // tilesets -- REbalance ships 14 custom maps -- and those are not
    // distributed here; see docs/101 for what each pack contains.
    // docs/386: live outside a battle and a lobby (it used to need a restart).
    help("Stat changes only; the packs' custom maps are not bundled.\n"
         "Takes effect at once outside a battle or lobby, including\n"
         "switching packs or back to Off; a change made during a battle\n"
         "or in a lobby waits until you leave it.");
    // WIRED, and measured rather than read off the code. The badge said READ,
    // "the ROM location is known but not hooked up", which understated it: the
    // chain is this combo -> lb_setting_balance_patch -> lb_patches.cpp:3195 at
    // the blueprint load -> lb_balance_apply, which writes the pack's bytes
    // into the game with bus_write_u8. A run with the Rebalance pack selected
    // reports
    //
    //     [lb_balance] Rebalance applied: 520 runs, 830 of 835 bytes changed
    //                                     (5 already matched vanilla)
    //
    // and a vanilla run prints no such line at all, which is the control.
    //
    // An earlier note here read "830 of 835" as five bytes the loader failed to
    // write. It is not a shortfall: lb_balance_apply writes only where the
    // current byte DIFFERS, so the counter is bytes CHANGED and the remaining
    // five are bytes the pack and vanilla already agree on. Nothing is missing.
    //
    // RETRACTED, docs/386: something WAS missing. The pack was applied the
    // frame the blueprint block's fingerprint matched, while the loader was
    // still writing its later records (145..199 differed from the finished
    // block), so the loader overwrote the pack's changes there, and the "5
    // already matched" were bytes of that unfinished block. Applied once the
    // block has settled, all 835 change ("835 of 835", run t386c).
    badge(WIRED);
    // docs/386: what is in the game now, and why a change is not yet.
    if (!lb_patches_blueprint_base()) {
        if (g_set.balance_patch > 0)
            ImGui::TextDisabled("  %d bytes, pending the blueprint load",
                                lb_balance_byte_count(g_set.balance_patch));
    } else if (lb_balance_applied_slot() != g_set.balance_patch) {
        const char* why = lb_blueprint_edit_blocked();
        ImGui::PushTextWrapPos(0.0f);   // the panel is narrow
        ImGui::TextDisabled("  in the game: %s; the change waits: %s",
                            lb_balance_name(lb_balance_applied_slot()),
                            why ? why : "next frame");
        ImGui::PopTextWrapPos();
    } else if (g_set.balance_patch > 0) {
        ImGui::TextDisabled("  applied: %s",
                            lb_balance_name(lb_balance_applied_slot()));
    }
}

// The rest of the Enhancements section, after the balance pack moved out.
void draw_after_balance_pack() {
    // COSMETIC AND THIS MACHINE'S ONLY, unlike everything above it -- hence
    // the "(this machine only)" beside the label, in a tab that opens with
    // "Host-decided". apply_leader_name in lb_patches.cpp rewrites the loaded
    // LOC table in guest RAM, which only screens read (docs/356).
    //
    // WIRED, and seen: with LB_LEADER_NAME=2 the harness's Free Play route logs
    // `[lb_leader] "Plisken": 9 string(s) rewritten`, and with the hero
    // selected the top screen's unit panel reads "Plisken" under his portrait
    // (docs/356 s6). The cheat-keyboard route in docs/356 is the other place
    // it shows.
    // (The two LOC-table renames that were here moved to the Renames section,
    // tab_renames below.)

    // docs/385: an enhancement, on by default, that fixes the monkey cage.
    // Cosmetic and this machine's only, like the name above:
    // apply_cage_fix in lb_patches.cpp corrects one word of
    // Unit_BuildBillboard's cage choice, which only picks a picture.
    ImGui::Separator();
    ImGui::Checkbox("Fix the Monkey Cage", &g_set.fix_monkey_cage);
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    help("Caught monkeys (Monkey Business) show the game's own monkey\n"
         "cage instead of the Space Criminal's. The game ships that picture\n"
         "but its check reads the wrong field, so it never appears.\n"
         "Cosmetic: nothing reaches other players; gameplay is unchanged.");
    badge(WIRED);

    // docs/385: the spells of the menu vary when several heroes are
    // selected; "Prioritize Own Hero Spells" always defers to the hero
    // matching the one picked for Free Play.
    ImGui::Separator();
    ImGui::Checkbox("Prioritize Own Hero Spells", &g_set.own_hero_spells);
    help("With several heroes selected -- your own and one you hotwired,\n"
         "say -- the yellow brick shows YOUR hero's spells: the hero of\n"
         "the army on your Customize Army screen. The game otherwise shows\n"
         "whichever hero came first in the selection, which depends on\n"
         "where the box was dragged.\n"
         "Only this player's menu changes; no rule of the match does.");
    badge(WIRED);
    tab_end();
}

// docs/425: Enhancements > Renames. Every cosmetic rename, all Off (the
// cartridge's own words) by default; each is this machine's text only.
// Plantation and Space Criminal Leader edit the loaded LOC table in place
// (apply_plantation_name / apply_leader_name, docs/392, docs/356); the other
// three are longer than the words they replace, so string_from_id_override in
// lb_patches.cpp supplies the rewritten text as it is fetched.
void tab_renames() {
    if (!tab_begin("Renames")) return;
    guide("Change the names the game shows for some characters and places. Pick a name "
          "below. This is local only: it changes text on your screens, other players "
          "do not see it, and gameplay is unchanged.");

    // docs/356: apply_leader_name; seen with LB_LEADER_NAME=2 (9 strings).
    ImGui::Separator();
    ImGui::Text("Change Space Criminal Leader Name");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* leader_names[] = {"Off", "Kranxx", "Plisken"};
    ImGui::Combo("##criminal_leader_name", &g_set.criminal_leader_name,
                 leader_names, IM_ARRAYSIZE(leader_names));
    help("Cosmetic and local: renames him in this game's text only.\n"
         "Nothing reaches other players; gameplay is unchanged.\n"
         "A screen already open shows the new name once reopened.");
    badge(WIRED);

    // docs/392: "Plantation" -> "Hide-Away" or "Storehouse" (string 445, the
    // building's name, and 866-868, "Build a Plantation").
    ImGui::Text("Change Pirate Plantation Name");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* plantation_names[] = {"Off", "Hide-Away", "Storehouse"};
    ImGui::Combo("##plantation_name", &g_set.plantation_name,
                 plantation_names, IM_ARRAYSIZE(plantation_names));
    help("Cosmetic and local: renames the Pirates' farm building in this\n"
         "game's text only (each language gets its own word). Gameplay is\n"
         "unchanged. A screen already open shows it once reopened.");
    badge(WIRED);

    // docs/425: the three whole-word renames.
    ImGui::Separator();
    ImGui::Text("Rename Claw Tank");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* claw_names[] = {"Off", "Crystal Reaper"};
    ImGui::Combo("##rename_claw_tank", &g_set.rename_claw_tank,
                 claw_names, IM_ARRAYSIZE(claw_names));
    help("Cosmetic and local: \"Claw Tank\" reads \"Crystal Reaper\" in this\n"
         "game's text only. Gameplay and other players are unaffected.\n"
         "A screen already open shows it once reopened.");
    badge(WIRED);

    ImGui::Text("Rename Gemma");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* gemma_names[] = {"Off", "Doctor Kelvin"};
    ImGui::Combo("##rename_gemma", &g_set.rename_gemma,
                 gemma_names, IM_ARRAYSIZE(gemma_names));
    help("Cosmetic and local: \"Gemma\" reads \"Doctor Kelvin\" in this\n"
         "game's text only (the credits keep her full name). Gameplay and\n"
         "other players are unaffected. A screen already open shows it once\n"
         "reopened.");
    badge(WIRED);

    ImGui::Text("Rename Broadside");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* broadside_names[] = {"Off", "Governor Tyranny"};
    ImGui::Combo("##rename_broadside", &g_set.rename_broadside,
                 broadside_names, IM_ARRAYSIZE(broadside_names));
    help("Cosmetic and local: \"Broadside\" reads \"Governor Tyranny\" in this\n"
         "game's text only (\"Governor Broadside\" becomes \"Governor\n"
         "Tyranny\"; the ship Lady Broadside is unchanged). Gameplay and other\n"
         "players are unaffected. A screen already open shows it once reopened.");
    badge(WIRED);

    ImGui::Separator();
    ImGui::Text("Rename Alien Assault Ship");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* rename_assault_ship_names[] = {"Off", "Dragon Cruiser"};
    ImGui::Combo("##rename_assault_ship", &g_set.rename_assault_ship,
                 rename_assault_ship_names, IM_ARRAYSIZE(rename_assault_ship_names));
    help("Cosmetic and local: Alien Assault Ship reads \"Dragon Cruiser\" in this\n"
         "game's text only. Gameplay and other players are unaffected.\n"
         "A screen already open shows it once reopened.");
    badge(WIRED);

    ImGui::Text("Rename Alien Attack Ship");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* rename_attack_ship_names[] = {"Off", "Crystal Extractor"};
    ImGui::Combo("##rename_attack_ship", &g_set.rename_attack_ship,
                 rename_attack_ship_names, IM_ARRAYSIZE(rename_attack_ship_names));
    help("Cosmetic and local: Alien Attack Ship reads \"Crystal Extractor\" in this\n"
         "game's text only. Gameplay and other players are unaffected.\n"
         "A screen already open shows it once reopened.");
    badge(WIRED);

    ImGui::Text("Rename Earth Transport Ship");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* rename_earth_transport_names[] = {"Off", "Recon Dropship"};
    ImGui::Combo("##rename_earth_transport", &g_set.rename_earth_transport,
                 rename_earth_transport_names, IM_ARRAYSIZE(rename_earth_transport_names));
    help("Cosmetic and local: the Earth Transport Ship reads \"Recon Dropship\" in this\n"
         "game's text only. Gameplay and other players are unaffected.\n"
         "A screen already open shows it once reopened.");
    badge(WIRED);

    ImGui::Text("Rename Alien Transport Ship");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* rename_alien_transport_names[] = {"Off", "Hyper Carrier"};
    ImGui::Combo("##rename_alien_transport", &g_set.rename_alien_transport,
                 rename_alien_transport_names, IM_ARRAYSIZE(rename_alien_transport_names));
    help("Cosmetic and local: the Alien Transport Ship reads \"Hyper Carrier\" in this\n"
         "game's text only. Gameplay and other players are unaffected.\n"
         "A screen already open shows it once reopened.");
    badge(WIRED);

    ImGui::Text("Rename Starfighter");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* rename_starfighter_names[] = {"Off", "Switch Fighter"};
    ImGui::Combo("##rename_starfighter", &g_set.rename_starfighter,
                 rename_starfighter_names, IM_ARRAYSIZE(rename_starfighter_names));
    help("Cosmetic and local: Starfighter reads \"Switch Fighter\" in this\n"
         "game's text only. Gameplay and other players are unaffected.\n"
         "A screen already open shows it once reopened.");
    badge(WIRED);

    ImGui::Text("Rename Starship 1");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* rename_starship1_names[] = {"Off", "Rocket"};
    ImGui::Combo("##rename_starship1", &g_set.rename_starship1,
                 rename_starship1_names, IM_ARRAYSIZE(rename_starship1_names));
    help("Cosmetic and local: Starship 1 reads \"Rocket\" in this\n"
         "game's text only. Gameplay and other players are unaffected.\n"
         "A screen already open shows it once reopened.");
    badge(WIRED);

    // SG-0021: the names the Pirate buildings had in the game's prototypes,
    // where they were the other way round. The two go together, or two
    // buildings would share a name.
    ImGui::Text("Pirate buildings: prototype names");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* rename_pirate_buildings_names[] = {"Off", "Tavern and Shanty Shack"};
    ImGui::Combo("##rename_pirate_buildings", &g_set.rename_pirate_buildings,
                 rename_pirate_buildings_names, IM_ARRAYSIZE(rename_pirate_buildings_names));
    help("Cosmetic and local, from the Pirate prototypes: the barracks (Shanty\n"
         "Shack) reads \"Tavern\" and the Plantation reads \"Shanty Shack\". The\n"
         "Plantation name option above, when set, wins over this one.\n"
         "A screen already open shows it once reopened.");
    badge(WIRED);

    ImGui::Text("Rename Brickbeard's Bounty");
    ImGui::SameLine();
    ImGui::TextDisabled("(this machine only)");
    const char* rename_bounty_names[] = {"Off", "Flying Dutchman"};
    ImGui::Combo("##rename_bounty", &g_set.rename_bounty, rename_bounty_names,
                 IM_ARRAYSIZE(rename_bounty_names));
    help("Cosmetic and local, from the Pirate prototypes: Brickbeard's Bounty\n"
         "reads \"Flying Dutchman\" in this game's text only.\n"
         "A screen already open shows it once reopened.");
    badge(WIRED);
    tab_end();
}

// The cartridge's 18 cheats, in the order of the flags-block bytes they set,
// so the index IS the byte. Names are the game's own LOC strings and the
// codes are the ones in the cartridge's record table (docs/171); nothing here
// is transcribed from a walkthrough.
struct CheatDef { const char* name; const char* code; };
static const CheatDef kCheats[] = {
    {"Reveal Map",            "SKQMXPL"},   // flags[0]
    {"One Hit Kill (Heroes)", "AVMPWHK"},
    {"Fast Building",         "QMSLPOE"},
    {"Fast Magic",            "JRTPASX"},
    {"Double LEGO Bricks",    "BGQOYRT"},
    {"Show Minikit",          "LJYQRAC"},
    {"Show LEGO Studs",       "CPLYREK"},
    {"Regenerating Health",   "ABABLRX"},
    {"Long Range Magic",      "ZPWJFUQ"},
    {"Super Magic",           "DWFTBNS"},
    {"Show Red Bricks",       "RTGYPKC"},   // flags[10]
    // docs/406: from here the record and the byte part. FUN_020088D4's jump
    // table sends records 11..17 to bytes 16, 17, 11, 12, 13, 14, 15, and the
    // readers agree: byte 11 is FUN_020531F8's harvest time, 12 the unit cap
    // (FUN_02085E78), 13 the hero's health, 14 the mining yield (FUN_0206D7F0),
    // 15 the visibility test beside Reveal Map's (FUN_02025C14), 16 the tower
    // upgrade (category 13), 17 the doubled blast. This list had the record
    // order, so ticking Upgraded Towers turned Fast Harvesting on.
    {"Fast Harvesting",       "PQZLJOB"},   // flags[11]
    {"Full Unit Cap",         "UMSXIRQ"},
    {"Invincible Hero",       "HJCRAWK"},
    {"Fast Mining",           "KVBPQRJ"},
    {"Show Enemies",          "IBGOFWX"},
    {"Upgraded Towers",       "EDRFTGY"},
    {"Super Explosions",      "THNBGRE"},   // flags[17]
};
static const int kCheatCount = (int)(sizeof(kCheats) / sizeof(kCheats[0]));

void tab_cheats() {
    if (!tab_begin("Cheats")) return;


    // Hotwire Anything moved to the Enhancements tab, as one option of the
    // Hotwire targets dropdown: it and Patch the glitch are mutually
    // exclusive, and one setting states that without two checkboxes that can
    // disagree. The three-line notice that said so here has served its turn.
    //
    // The cartridge's own cheats. docs/171: record i of the table at
    // 0x02125FF4 names cheat i, and func_020088D4's jump table reads
    // flags[i] for every case 0..17 -- so the mapping is the game's, not a
    // guess. Verified live: setting flags[0] revealed the whole minimap.
    // BR-0011: the host's cheats ride in the rules datagram's S page, and a
    // joiner plays with those, not its own.
    ImGui::TextDisabled("Online, the host's cheats apply to everyone.");
    ImGui::Spacing();
    const bool cheat_lock = host_locked();
    host_lock_note(cheat_lock);
    if (ImGui::BeginTable("cheats", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Cheat");
        ImGui::TableSetupColumn("Code");
        ImGui::TableSetupColumn("Flag");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kCheatCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            if (cheat_lock) {
                bool hv = (g_hs.cheats >> i) & 1u;   // BR-0011: the host's, locked
                ImGui::BeginDisabled();
                ImGui::Checkbox(kCheats[i].name, &hv);
                ImGui::EndDisabled();
            } else {
                ImGui::Checkbox(kCheats[i].name, &g_set.cheat[i]);
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", kCheats[i].code);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("flags[%d]", i);
        }
        ImGui::EndTable();
    }
    badge(WIRED);
    ImGui::Spacing();
    host_lock_begin(cheat_lock);
    if (ImGui::Button("Enable every cheat")) {
        for (int i = 0; i < kCheatCount; ++i) g_set.cheat[i] = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Disable every cheat")) {
        for (int i = 0; i < kCheatCount; ++i) g_set.cheat[i] = false;
    }
    host_lock_end(cheat_lock);
    ImGui::Spacing();
    // docs/406: each gameplay cheat tests that the unit is the local
    // player's (owner byte against FUN_020A2230's); kAiCheats passes that
    // test for every owner. Super Magic and Long Range Magic have no test.
    // docs/433: one choice of who the ticked cheats help: you (the game's own),
    // you and the AI (docs/406), or the AI only (a handicap).
    ImGui::Text("The Cheats Help");
    {
        const char* who[] = {"Me (vanilla)", "Me and the AI", "The AI only"};
        int w = !g_set.ai_cheats ? 0 : (g_set.ai_cheats_only ? 2 : 1);
        ImGui::Combo("##cheats_help", &w, who, IM_ARRAYSIZE(who));
        g_set.ai_cheats = w != 0;
        g_set.ai_cheats_only = w == 2;
    }
    help("Who the gameplay cheats ticked above (faster building, harvesting\n"
         "and mining, magic, health, double bricks, the unit cap, towers,\n"
         "explosions, the hero's invincibility and one hit kills) help.\n"
         "Me and the AI: the computer's armies get them as well.\n"
         "The AI only: they stop helping you, for a harder game. Super\n"
         "Magic and Long Range Magic always help every army, and the\n"
         "reveal cheats stay yours. Not online.");
    badge(WIRED);

    ImGui::Separator();
    // docs/407: WHAT A PROFILE EARNS, set where the game keeps it. The live
    // state is the UnlockableManager (*0x021585A8): four 289-bit arrays,
    // committed A/B at +4/+0x14 and their working copies at +0x24/+0x34, one
    // bit per collectable slot (A = unlocked, B = bought or owned). The game
    // builds the save from them at its next save; it is not written here.
    // Campaigns are each mission's +0x15F and the 91-bit array at 0x02154E18.
    // apply_unlocks in lb_patches.cpp; only ever SETS, so unticking a box
    // locks nothing again.
    ImGui::Text("Unlocks");
    help("Unlocks things on the current profile the way earning them would,\n"
         "and the game saves them at its next save. Unticking a box does\n"
         "not lock anything again.");
    ImGui::Checkbox("Unlock All Units", &g_set.unlock_units);
    help("All 85 minifigures and vehicles in the Character Collection,\n"
         "bought, so every unit can go in a Custom Army (Santa too).");
    badge(WIRED);
    ImGui::Checkbox("Unlock All Maps", &g_set.unlock_maps);
    help("All 30 Free Play and multiplayer maps, as if bought in\n"
         "Extras > Maps.");
    badge(WIRED);
    ImGui::Checkbox("Unlock All Cheat Codes", &g_set.unlock_extras);
    help("The 18 red-brick cheats, found and bought, so the game's own\n"
         "Extras > Cheat Codes screen can switch them.");
    badge(WIRED);
    ImGui::Checkbox("Unlock All Concept Art", &g_set.unlock_art);
    help("All 66 pieces in Extras > Concept Art, bought.");
    badge(WIRED);
    ImGui::Checkbox("Complete Every Campaign", &g_set.unlock_campaigns);
    help("Marks all 90 story missions done: every mission is open and\n"
         "every cutscene shows in Extras > Cutscenes.");
    badge(WIRED);

    ImGui::Spacing();
    if (ImGui::Button("Unlock All")) {
        g_set.unlock_units = g_set.unlock_extras = g_set.unlock_maps =
            g_set.unlock_art = g_set.unlock_campaigns = true;
    }
    help("Ticks every box above, so a new profile is ready for online\n"
         "play: every unit for a Custom Army and every map to host on.");
    badge(WIRED);
    tab_end();
}

void tab_input() {
    if (!tab_begin("Input")) return;
    bindings_init_once();

    ImGui::TextDisabled("Click a binding to rebind; Esc cancels.");
    badge(WIRED);
    ImGui::Separator();

    // docs/383/385: the battle shortcuts. Each runs the cartridge's own
    // callback for the button (lb_patches.cpp, view_update_override).
    ImGui::Checkbox("Battle shortcuts", &g_set.quick_keys);
    help("Space or right-click: Deselect.\n"
         "Mouse 5 (forward side button): the blue brick menu.\n"
         "Mouse 4 (back side button): the red, yellow or white brick\n"
         "menu, whichever is shown.\n"
         "1-9 (number row or keypad): that item of the red, yellow or\n"
         "white brick's menu -- a builder's building, a hero's spell --\n"
         "counted left to right. The menu opens if it is closed.\n"
         "Each runs the game's own function for that button, without\n"
         "touching the screen, and does nothing when it is not shown.\n"
         "A key bound to a DS button above stays that DS button.");
    ImGui::Checkbox("Number keys pick from the open blue menu", &g_set.quick_keys_blue);
    help("While the blue brick's menu is the open one, 1-9 pick from it\n"
         "instead of the red, yellow or white brick's. Off: 1-9 always\n"
         "pick from the red, yellow or white brick's menu.");
    ImGui::Checkbox("Click a queued unit to cancel it", &g_set.cancel_queue_click);
    help("Select a building that is training units. Click a unit's\n"
         "icon in its queue, on the Top Screen panel, to cancel it and\n"
         "get its bricks back. It sends the game's own cancel command,\n"
         "so it is safe in online play. Needs the Top Screen panel.");
    ImGui::Separator();
    {   // SG-0012: swap the two screens, from a key or this button.
        const char* key = g_rebinding_swap
                              ? "press a key..."
                              : (g_set.swap_key ? SDL_GetScancodeName((SDL_Scancode)g_set.swap_key)
                                                : "(none)");
        char label[64];
        std::snprintf(label, sizeof label, "Swap screens key: %s###swapkey",
                      (key && *key) ? key : "(none)");
        if (ImGui::Button(label)) g_rebinding_swap = !g_rebinding_swap;
        ImGui::SameLine();
        {   // SG-0012: the DS layouts only; Enhanced has one field, not two screens
            const bool enh = g_set.enhanced;
            if (enh) ImGui::BeginDisabled();
            if (ImGui::Button("Swap screens")) lb_request_swap_screens();
            if (enh) ImGui::EndDisabled();
            if (enh && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Swapping the screens works in DS Vanilla only.\n"
                                  "Enhanced shows one field, which does not swap.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear key")) g_set.swap_key = 0;
        help("DS Vanilla only: the two screens change places, and the touch\n"
             "follows the one that is the touch screen. Press again to swap\n"
             "back. Local to this machine; the key is unbound until you set it.");
        badge(WIRED);
    }
    ImGui::Separator();

    if (ImGui::BeginTable("binds", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("DS button");
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Controller");
        ImGui::TableSetupColumn("Shipped key");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kActionCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kActions[i].name);

            ImGui::TableNextColumn();
            ImGui::PushID(i);
            const char* label =
                (g_rebinding == i)
                    ? "press a key..."
                    : SDL_GetScancodeName((SDL_Scancode)g_binding[i]);
            if (ImGui::Button((label && *label) ? label : "(none)",
                              ImVec2(-FLT_MIN, 0.0f)))
                g_rebinding = (g_rebinding == i) ? -1 : i;
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::PushID(1000 + i);
            const char* pad =
                (g_rebinding_pad == i)
                    ? "press a button..."
                    : SDL_GameControllerGetStringForButton(
                          (SDL_GameControllerButton)g_pad_binding[i]);
            if (ImGui::Button((pad && *pad) ? pad : "(none)", ImVec2(-FLT_MIN, 0.0f)))
                g_rebinding_pad = (g_rebinding_pad == i) ? -1 : i;
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::TextDisabled(
                "%s", SDL_GetScancodeName((SDL_Scancode)kActions[i].default_scancode));
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset all to shipped")) {
        for (int i = 0; i < kActionCount; ++i) {
            g_binding[i] = kActions[i].default_scancode;
            g_pad_binding[i] = kActions[i].default_pad;
        }
        g_rebinding = g_rebinding_pad = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save bindings")) settings_save();
    // CORRECTED IN PLACE. This said "Settings are saved on demand, not
    // automatically", which described neither file: the bindings were on
    // demand and everything else had been autosaving to settings.json all
    // along. A panel that misdescribes where a setting goes is as much an
    // unfinished control as one that is not wired at all.
    //
    // The touch screen is the mouse and is not rebindable: the main window is
    // the stylus by design.
    help("Bindings save only when you press this.\n"
         "Every OTHER setting autosaves a second or two after it moves.");
    badge(WIRED);
    ImGui::SameLine();
    ImGui::TextDisabled("to %s", kSettingsFile);
    tab_end();
}

void tab_multiplayer() {
    if (!tab_begin("Multiplayer")) return;
    guide("Choose the server and how you connect. To play online, one player hosts and "
          "the others join from the game's own menu. Keep Both servers and the fast "
          "online link the same as the host's; the relay starts by itself if a direct "
          "link fails.");

    // docs/400: the known servers by name; Custom shows the fields.
    {
        struct Known { const char* label; const char* host; int port; };
        static const Known kKnown[] = {
            {"lbserver.viosarcade.xyz", "lbserver.viosarcade.xyz", 80},
            {"king.legobattles.win", "king.legobattles.win", 28555},
        };
        const int nk = (int)(sizeof kKnown / sizeof kKnown[0]);
        int cur = nk;   // Custom
        for (int i = 0; i < nk; ++i)
            if (std::strcmp(g_set.master_host, kKnown[i].host) == 0 &&
                g_set.master_port == kKnown[i].port)
                cur = i;
        static bool custom_open = false;
        if (cur < nk) custom_open = false;
        // docs/419: "Both" is the first server with the second beside it.
        const bool both = g_set.master_both && cur == 0 && !custom_open;
        const char* shown = both ? "Both (default)"
                          : (cur < nk && !custom_open) ? kKnown[cur].label : "Custom";
        if (ImGui::BeginCombo("Master server", shown)) {
            for (int i = 0; i < nk; ++i)
                if (ImGui::Selectable(kKnown[i].label, cur == i && !custom_open && !both)) {
                    std::snprintf(g_set.master_host, sizeof g_set.master_host,
                                  "%s", kKnown[i].host);
                    g_set.master_port = kKnown[i].port;
                    g_set.master_both = false;
                    g_set.master_choice = true;   // docs/424
                    custom_open = false;
                }
            if (ImGui::Selectable("Both (default)", both)) {
                std::snprintf(g_set.master_host, sizeof g_set.master_host, "%s",
                              kKnown[0].host);
                g_set.master_port = kKnown[0].port;
                g_set.master_both = true;
                g_set.master_choice = true;   // docs/424
                custom_open = false;
            }
            if (ImGui::Selectable("Custom", (cur == nk || custom_open) && !both)) {
                custom_open = true;
                g_set.master_both = false;
                g_set.master_choice = true;   // docs/424
            }
            ImGui::EndCombo();
        }
        help("Where lobbies are listed. It lists them and introduces a\n"
             "joining player to the host so both routers let the game's\n"
             "traffic through; no port forwarding is needed on most home\n"
             "routers. king.legobattles.win is another player's server and\n"
             "may be offline. Both lists a hosted lobby on both servers and\n"
             "shows the lobbies of both.");
        badge(WIRED);

        // docs/425 item 7: a ping per server -- one /health round trip each,
        // timed on a worker thread (as the game itself talks to it: HTTPS
        // when that is on). Once when the page first opens, then on demand.
        {
            struct Ping { std::string label; int ms = -2; };   // -2 not asked, -1 no answer
            static std::mutex mu;
            static std::vector<Ping> pings;
            static std::atomic<bool> busy{false};
            static bool asked = false;
            const bool go = ImGui::Button("Ping servers");
            if ((go || !asked) && !busy.exchange(true)) {
                asked = true;
                std::vector<std::pair<std::string, int>> targets;
                for (int i = 0; i < nk; ++i) targets.push_back({kKnown[i].host, kKnown[i].port});
                if (cur == nk) targets.push_back({g_set.master_host, g_set.master_port});
                {
                    std::lock_guard<std::mutex> lk(mu);
                    pings.clear();
                    for (auto& t : targets) pings.push_back({t.first, -2});
                }
                std::thread([targets]() {
                    for (size_t i = 0; i < targets.size(); ++i) {
                        static char buf[1024];
                        const auto t0 = std::chrono::steady_clock::now();
                        const int n = lb_net_http_get(targets[i].first.c_str(), targets[i].second,
                                                      "/health", buf, (int)sizeof buf);
                        const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - t0).count();
                        const bool ok = n > 0 && std::strstr(buf, "\"ok\"") != nullptr;
                        std::lock_guard<std::mutex> lk(mu);
                        if (i < pings.size()) pings[i].ms = ok ? ms : -1;
                    }
                    busy = false;
                }).detach();
            }
            help("Times one request to each server: lower is better. A server that\n"
                 "does not answer may be offline.");
            std::lock_guard<std::mutex> lk(mu);
            for (const Ping& p : pings) {
                ImGui::SameLine();
                if (p.ms == -2) ImGui::TextDisabled("%s: ...", p.label.c_str());
                else if (p.ms < 0) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                                                      "%s: no answer", p.label.c_str());
                else ImGui::Text("%s: %d ms", p.label.c_str(), p.ms);
            }
        }
        if (cur == nk || custom_open) {
            ImGui::InputText("Address", g_set.master_host,
                             IM_ARRAYSIZE(g_set.master_host));
            ImGui::InputInt("Port", &g_set.master_port);
        }
        ImGui::Checkbox("Use HTTPS with the master server", &g_set.master_https);
        help("Talks to the master server over an encrypted connection when\n"
             "it offers one (port 443 for a server on port 80), and over\n"
             "plain HTTP when it does not.");
        ImGui::Checkbox("Share Match Telemetry", &g_set.match_picture);
        help("When you host, a picture of the match (teams, units, map and\n"
             "banned units) is sent to the master server for its Discord log.");
        ImGui::Checkbox("Fetch a host's mod map I don't have", &g_set.fetch_maps);
        help("When a host picks a mod map that is not in your mods folder,\n"
             "look for it by name in the community map archive (GitHub) and\n"
             "put it in mods/maps/_archive. Restart the game to use it.");
        ImGui::Checkbox("Fast online link", &g_set.fast_link);
        help("Carries the game's wireless traffic in a pipeline instead of\n"
             "one network round trip per step, so an online match keeps\n"
             "its frame rate. Every player in a game needs the same\n"
             "setting; it takes effect the next time you host or join.\n"
             "Off is the original DS wireless protocol.");
    }

    ImGui::Separator();
    ImGui::Checkbox("Use the save's profile name", &g_set.name_from_save);
    help("The name the game itself shows in its lobby.\n"
         "Untick to advertise a different one.");
    badge(WIRED);   // docs/396: seen in the host lobby
    if (g_set.name_from_save) {
        // Read-only mirror of the loaded profile. Not g_set.player_name, which
        // stays what the player last typed for when this is unticked.
        char shown[32];
        if (lb_net_profile_name(shown, (int)sizeof shown) <= 0)
            std::snprintf(shown, sizeof shown, "%s", "(no profile loaded yet)");
        ImGui::BeginDisabled();
        ImGui::InputText("Player name", shown, sizeof shown,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
    } else {
        // docs/396: up to 10 characters, as the game's own name entry.
        ImGui::InputText("Player name", g_set.player_name, 11);
        help("The name the other players see in the game's lobby and in\n"
             "the match. Your save keeps its own name unless you write\n"
             "this one into it below.");
        if (ImGui::Button("Write this name into the save")) {
            lb_request_profile_rename(g_set.player_name);
            g_set.name_from_save = true;   // the profile now carries it
        }
        help("Renames the loaded profile. The game shows it at once and\n"
             "saves it the next time its main menu is up.");
    }
    ImGui::InputText("Room code", g_set.room_code, IM_ARRAYSIZE(g_set.room_code));
    help("Name is how an advertised lobby is listed.\n"
         "Code is what Join by code looks up.");
    badge(WIRED);

    // docs/415: chat.
    ImGui::SeparatorText("Chat");
    ImGui::Checkbox("Chat toasts", &g_set.chat_toasts);
    help("Shows a message from another player for a few seconds, led by\n"
         "their hero's icon, while the chat box is closed.");
    ImGui::Checkbox("Swear filter", &g_set.swear_filter);
    help("Shows swear words in chat and in players' names as \"ziff\"\n"
         "(\"ziffing\" for -ing words). Switching it off shows them as\n"
         "written, at once; nothing sent or stored is changed.");
    {
        const char* key = g_rebinding_chat
                              ? "press a key..."
                              : SDL_GetScancodeName((SDL_Scancode)g_set.chat_key);
        char label[64];
        std::snprintf(label, sizeof label, "Chat key: %s###chatkey",
                      (key && *key) ? key : "(none)");
        if (ImGui::Button(label)) g_rebinding_chat = !g_rebinding_chat;
        help("Opens the chat box while you are in an online lobby or match\n"
             "(and does nothing otherwise). Enter sends, Esc closes.");
    }
    badge(WIRED);   // docs/415: runs ch415a/b

    // The game's own screens drive the master server.
    ImGui::SeparatorText("Master server");
    ImGui::Checkbox("Advertise when the game hosts", &g_set.auto_advertise);
    help("Opening a lobby from the game's Multiplayer > Host lists it\n"
         "on the master server; Start or leaving takes it off.");
    badge(WIRED);   // docs/400: run t400b
    // docs/405
    ImGui::Checkbox("Open the port on my router when hosting (UPnP)", &g_set.upnp);
    help("Asks your router to forward the game's UDP port to this PC while\n"
         "you host, and closes it again when the lobby or match ends. Most\n"
         "home routers allow it; some have UPnP switched off in their own\n"
         "settings. Without it, joining still works on most routers.");
    badge(WIRED);
    if (const char* u = lb_net_upnp_text())
        if (*u) ImGui::TextDisabled("  %s", u);
    {   // docs/424: the master server's relay, for when nothing else connects
        char r[160];
        if (lb_relay_status(r, (int)sizeof r) > 0 && std::strcmp(r, "off") != 0)
            ImGui::TextDisabled("  Relay: %s", r);
    }
    ImGui::Checkbox("Game's Join screen uses the master server",
                    &g_set.auto_browse);
    help("While the game's Join screen is open the list is fetched every\n"
         "few seconds and the newest host (or the one you picked) is made\n"
         "reachable, so the game lists it.");
    badge(WIRED);   // docs/400: run t400b
    ImGui::TextDisabled("Game network: %s", lb_net_guest_state_text());

    ImGui::Spacing();
    ImGui::BeginDisabled(lb_net_busy() != 0);
    if (ImGui::Button("Browse lobbies"))
        lb_net_refresh_lobbies(g_set.master_host, g_set.master_port);
    ImGui::EndDisabled();
    badge(WIRED);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", lb_net_status());

    const int n = lb_net_lobby_count();
    if (n > 0 && ImGui::BeginTable("lobbies", 7,
                                   ImGuiTableFlags_Borders |
                                   ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Code");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Host");
        ImGui::TableSetupColumn("Map");
        ImGui::TableSetupColumn("Players");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; ++i) {
            LbLobby lobby{};
            if (!lb_net_lobby_at(i, &lobby)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(lobby.code);
            // docs/415: the swear filter covers the names lobbies carry.
            ImGui::TableNextColumn(); ImGui::TextUnformatted(lb_swear_shown(lobby.name));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(lb_swear_shown(lobby.host_name));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(lobby.map_name);
            ImGui::TableNextColumn();
            ImGui::Text("%d/%d", lobby.players, lobby.max_players);
            ImGui::TableNextColumn();
            ImGui::Text("%s:%d", lobby.address, lobby.port);
            // A row already carries the address, so joining from the list
            // needs no second round trip -- unlike "Join by code", which has
            // to ask the directory again.
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::SmallButton("Join")) lb_net_join_index(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (lb_net_is_hosting()) {
        if (ImGui::Button("Stop advertising")) lb_net_stop_hosting();
        ImGui::SameLine();
        ImGui::Text("Room code: %s", lb_net_host_code());
    } else {
        // The port this instance ACTUALLY listens on, not a constant. A lobby
        // advertising a port nobody is bound to is a directory entry and
        // nothing else -- and the 28700 this used to fall back to when local
        // wireless was off is exactly what a 2026-09-24 test advertised
        // (the master server's log). No port, no button.
        const unsigned short wp = lb_net_wireless_port();
        ImGui::BeginDisabled(lb_net_busy() != 0 || wp == 0);
        if (ImGui::Button("Advertise a lobby")) {
            lb_net_announce(g_set.master_host, g_set.master_port,
                            "LEGO Battles", mp_player_name(), "",
                            (int)wp, 1, 3);
        }
        ImGui::EndDisabled();
    }
    // Three players maximum, which the ROM confirms: Play3PlayerMP is in the
    // achievement table (docs/97).
    help("Puts the lobby on the master server and keeps it alive\n"
         "with a heartbeat. Three players maximum.");
    badge(WIRED);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", lb_net_host_status());

    ImGui::Spacing();
    ImGui::BeginDisabled(lb_net_busy() != 0);
    if (ImGui::Button("Join by code"))
        lb_net_join_by_code(g_set.master_host, g_set.master_port,
                            g_set.room_code);
    ImGui::EndDisabled();
    help("Looks the code up and points this machine's wireless bridge\n"
         "at the host it names. Once the bridge has somewhere to send,\n"
         "a host on another machine looks like one in the same room.");
    badge(WIRED);
    ImGui::TextDisabled("  then join from the game's own Multiplayer > Join "
                        "screen");
    ImGui::Spacing();
    {
        const unsigned short wp = lb_net_wireless_port();
        if (wp)
            ImGui::Text("This machine listens on UDP %u", (unsigned)wp);
        else
            ImGui::TextDisabled(
                "Local wireless is OFF -- start with --local-wireless on "
                "(play.bat does)");
        const char* peer = lb_net_peer_text();
        if (peer && *peer)
            ImGui::Text("Peer: %s", peer);
        else
            ImGui::TextDisabled("No peer set. Hosting learns one from the "
                                "first datagram; joining sets it from the code.");
        // docs/400: what the router gave this machine, and whether anything
        // has come back -- a dead link must not look like a slow one.
        char pub[40];
        if (lb_net_public_text(pub, (int)sizeof pub))
            ImGui::Text("Public address: %s", pub);
        else
            ImGui::TextDisabled("Public address: not known yet (found when "
                                "hosting or joining)");
        unsigned sent = 0, recvd = 0;
        lb_net_link_stats(&sent, &recvd);
        if (peer && *peer) {
            if (recvd)
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                                   "Connected: %u datagram(s) from the other "
                                   "player", recvd);
            else
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f),
                                   "Nothing from the other player yet "
                                   "(%u sent)", sent);
        }
    }
    // Two instances on ONE machine need no peer at all: different
    // --instance-index values find each other over loopback, which is how
    // docs/216 got two players into one battle.
    tab_end();
}

// ── Save manager ───────────────────────────────────────────────────────────
//
// An ImGui save menu that can load/export a LEGO Battles save file from any
// region.
//
// FROM ANY REGION is the whole point and the only subtle part. A LEGO Battles
// save is 512 bytes beginning with the tag `XXX_BATTLE_SAVES`, and the three
// character prefix is a COMPILE-TIME CONSTANT in the game binary --
// func_0204b1d4 writes 'A', 'F', 'E' as literal immediates. It is not derived
// from the console's region at runtime. So a save from a cartridge whose build
// used a different prefix is rejected by this one, and the game shows empty
// profile slots rather than any error. tools/lb_save.py established that, and
// that the checksum does not cover the tag, so rewriting the prefix is enough
// and nothing else in the file needs touching.
//
// Import therefore RETAGS to whatever prefix the running image already uses,
// which is read from the live save rather than assumed.
namespace {

char g_save_path[512] = "";
std::string g_save_msg;
bool g_save_msg_bad = false;

const uint32_t kSaveSize = 512;
const uint32_t kTagLen = 16;

// The live save image, or null before the cartridge is up.
const uint8_t* live_save(uint32_t* size) {
    const uint8_t* data = nullptr;
    uint32_t sz = 0;
    bool dirty = false;
    if (!nds_io_cartridge_save_snapshot(&data, &sz, &dirty)) return nullptr;
    if (size) *size = sz;
    return data;
}

}  // namespace

char g_demo_path[512] = "demo.lbdemo";

// A demo is a RECORDING OF WHAT THE CONSOLE RECEIVED, replayed frame for frame.
//
// It exists because every automated probe this port has written for the touch
// screen has had to guess something -- where a sprite's clickable pixels are,
// what deselects, whether the thing selected is the thing aimed at -- and each
// guess produced a confident wrong answer at least once (docs/214, docs/217,
// docs/218). A player guesses none of it. Record once, replay for ever.
void tab_demo() {
    if (!tab_begin("Demo")) return;
    // What is stored is what the CONSOLE received -- touch, buttons, and this
    // port's own out-of-band coordinate -- indexed by frame, so a replay lands
    // every input on the frame it was made on however fast the host runs, and
    // at any window size.
    ImGui::TextDisabled("Record your play, then replay it exactly.");
    ImGui::Separator();
    ImGui::Checkbox("Record every session", &g_set.record_sessions);
    help("Each time the game starts, record it to a new lb-session-*.lbdemo\n"
         "beside the game, so a bug can be replayed exactly. Takes effect the\n"
         "next time the game starts. Off by default.");
    ImGui::Separator();
    ImGui::InputText("File", g_demo_path, IM_ARRAYSIZE(g_demo_path));
    help("Written as you play, one line per input, so closing the\n"
         "window loses nothing. LB_DEMO_REC / LB_DEMO_PLAY do the\n"
         "same from the command line.");
    const int st = lb_demo_state();
    ImGui::BeginDisabled(st != 0);
    if (ImGui::Button("Record")) lb_demo_start_record(g_demo_path);
    ImGui::SameLine();
    if (ImGui::Button("Play")) lb_demo_start_play(g_demo_path);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(st == 0);
    if (ImGui::Button("Stop")) lb_demo_stop();
    ImGui::EndDisabled();
    badge(WIRED);
    ImGui::Text("%s -- frame %u, %u event(s)",
                lb_demo_status(), lb_demo_frame(), lb_demo_event_count());
    ImGui::TextDisabled("  start from the title screen to reproduce a whole "
                        "session");
    tab_end();
}

void tab_saves() {
    if (!tab_begin("Saves")) return;
    guide("Import brings in a save from a cartridge or another emulator (any region); "
          "Export keeps a copy of yours. Pick the file with Browse, then press the "
          "button. Saves are local only: they never go online.");

    uint32_t live_size = 0;
    const uint8_t* live = live_save(&live_size);
    if (!live) {
        ImGui::TextDisabled("No cartridge save is loaded yet.");
        tab_end();
        return;
    }

    char tag[17] = {0};
    std::memcpy(tag, live, kTagLen < 16 ? kTagLen : 16);
    for (int i = 0; i < 16; ++i)
        if (tag[i] < 32 || tag[i] > 126) tag[i] = '.';
    ImGui::Text("Live save: %u bytes, tag \"%s\"", live_size, tag);
    const bool tag_ok =
        live_size >= kTagLen &&
        std::memcmp(live + 3, "_BATTLE_SAVES", 13) == 0;
    if (!tag_ok)
        ImGui::TextDisabled("  Tag does not look like a LEGO Battles save.");

    ImGui::Separator();
    // LB_SAVE_PATH pre-fills the field, so Export and Import can be exercised
    // without typing into an imgui text box.
    //
    // Everything else on this tab could be verified by pressing a button, but
    // these two refuse without a path and there is no file picker, so the only
    // way to reach them from a harness was synthetic keystrokes into a focused
    // InputText -- fragile, and fragile UI driving is exactly what produced a
    // year of screenshots of the wrong tab (see tab_begin above). The same
    // shape as LB_DEMO_REC and LB_PANEL_TAB: name the input, do not aim at it.
    {
        static bool seeded = false;
        if (!seeded) {
            seeded = true;
            if (const char* p = std::getenv("LB_SAVE_PATH")) {
                std::snprintf(g_save_path, sizeof g_save_path, "%s", p);
            }
        }
    }
    ImGui::InputText("Save file", g_save_path, sizeof(g_save_path));
    badge(WIRED);   // consumed by Export/Import below
    ImGui::SetItemTooltip("Full path to a .sav file. LB_SAVE_PATH presets it.");
    if (ImGui::Button("Browse...")) {
        lb_pick_save_file(g_save_path, (int)sizeof g_save_path, 0);
    }
    ImGui::SetItemTooltip("Pick an existing .sav to import.");
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) {
        lb_pick_save_file(g_save_path, (int)sizeof g_save_path, 1);
    }
    ImGui::SetItemTooltip("Choose where Export writes.");

    if (ImGui::Button("Export live save")) {
        g_save_msg.clear();
        g_save_msg_bad = false;
        if (g_save_path[0] == '\0') {
            g_save_msg = "Give a path first.";
            g_save_msg_bad = true;
        } else if (FILE* f = std::fopen(g_save_path, "wb")) {
            const size_t wrote = std::fwrite(live, 1, live_size, f);
            std::fclose(f);
            if (wrote == live_size) {
                g_save_msg = "Exported " + std::to_string(live_size) +
                             " bytes.";
            } else {
                g_save_msg = "Short write; the file is incomplete.";
                g_save_msg_bad = true;
            }
        } else {
            g_save_msg = "Could not open that path for writing.";
            g_save_msg_bad = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import (retag to this build)")) {
        g_save_msg.clear();
        g_save_msg_bad = false;
        FILE* f = g_save_path[0] ? std::fopen(g_save_path, "rb") : nullptr;
        if (!f) {
            g_save_msg = "Could not open that path for reading.";
            g_save_msg_bad = true;
        } else {
            // docs/401: an emulator's or flashcart's file may be padded (with
            // 0xFF to 8 KB and beyond) or carry DeSmuME's footer; the game's
            // bytes are at the front. Read it all and keep the save's size.
            std::vector<uint8_t> buf;
            {
                uint8_t chunk[4096];
                size_t n = 0;
                while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0 &&
                       buf.size() < (1u << 20))
                    buf.insert(buf.end(), chunk, chunk + n);
            }
            std::fclose(f);
            size_t got = buf.size();
            if (got > live_size && live_size >= kTagLen &&
                std::memcmp(buf.data() + 3, "_BATTLE_SAVES", 13) == 0)
                got = live_size;
            if (got != live_size) {
                // Refuse rather than pad. A file that is not the right size is
                // not this game's save, and importing it would leave the
                // profile slots empty with no error shown.
                g_save_msg = "That file is " + std::to_string(got) +
                             " bytes; this save is " +
                             std::to_string(live_size) + ".";
                g_save_msg_bad = true;
            } else if (std::memcmp(buf.data() + 3, "_BATTLE_SAVES", 13) != 0) {
                g_save_msg = "That file has no _BATTLE_SAVES tag.";
                g_save_msg_bad = true;
            } else {
                const char from[4] = {char(buf[0]), char(buf[1]),
                                      char(buf[2]), 0};
                // Retag to the prefix this build already uses, taken from the
                // live save rather than hard-coded, so it is right for whatever
                // region the port was built for.
                // docs/401: a NEW player's live save is blank (0xFF), and
                // retagging to that wrote a tag the game refuses -- the
                // import "did not apply". Then the build's own constant is
                // used: AFE, which func_0204B1D4 writes as immediates.
                const char want[3] = {tag_ok ? (char)live[0] : 'A',
                                      tag_ok ? (char)live[1] : 'F',
                                      tag_ok ? (char)live[2] : 'E'};
                buf[0] = (uint8_t)want[0];
                buf[1] = (uint8_t)want[1];
                buf[2] = (uint8_t)want[2];
                if (nds_io_cartridge_save_restore(buf.data(), live_size)) {
                    // docs/401: and load it now, by the game's own restart
                    // (docs/396), unless a match or a wireless session is on.
                    int cx = 0, cy = 0;
                    const bool busy = lb_field_camera(&cx, &cy) ||
                                      lb_net_guest_state() > 0;
                    if (!busy) lb_request_game_reset();
                    g_save_msg = std::string("Imported (retagged ") + from +
                                 " -> " + want[0] + want[1] + want[2] + "). " +
                                 (busy ? "Leave the match or the lobby, then "
                                         "press Reset to load it."
                                       : "The game restarts to load it.");
                } else {
                    g_save_msg = "The runner refused the image.";
                    g_save_msg_bad = true;
                }
            }
        }
    }

    if (!g_save_msg.empty()) {
        if (g_save_msg_bad)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s",
                               g_save_msg.c_str());
        else
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s",
                               g_save_msg.c_str());
    }

    ImGui::Separator();
    // Import overwrites the live save image and marks it dirty, so the runner
    // writes it out on its next flush.
    //
    // No unlock-everything button here, on purpose: the four bytes at the
    // start of each profile record are a checksum the game verifies, and
    // clearing the unlock bits without recomputing it does not fail quietly --
    // the game reports "The LEGO Battles data is corrupted and will be
    // deleted" and does exactly that (docs/207). Importing a whole save is the
    // safe way to change unlocks; the checksum travels with its own data.
    // docs/401
    ImGui::TextDisabled("Any region's save, from a cartridge or an emulator, "
                        "imports; the game restarts to load it.");
    ImGui::TextDisabled("Your save is saves\\LEGO Battles.sav, whatever your "
                        "ROM file is called.");
    tab_end();
}

// ---- docs/384: TEAM BUILDER AND BAN UNITS ---------------------------------
//
// The grid mimics the Free Play army
// screen (UI/FE/CustomizeArmy.bin): the nine slot buttons where the game has
// them, beside a 5x5 grid of the selected slot's other units, in the order the
// game lists them, with the game's own head icons (lb_teams.cpp).
SDL_Renderer* g_team_renderer = nullptr;   // this frame's, from lb_overlay_draw
SDL_Renderer* g_overlay_renderer = nullptr;   // docs/396: for event coordinates

struct TeamPreset {
    std::string name;
    LbTeam team;
};

// "Name=p0,...,p8;..." -- the presets, and (docs/388) the save cache.
std::vector<TeamPreset> teams_parse(const char* p) {
    std::vector<TeamPreset> out;
    while (*p) {
        const char* eq = std::strchr(p, '=');
        const char* end = std::strchr(p, ';');
        if (!end) end = p + std::strlen(p);
        if (eq && eq < end) {
            TeamPreset tp;
            tp.name.assign(p, (size_t)(eq - p));
            int v[LB_TEAM_SLOTS];
            int n = 0;
            for (const char* q = eq + 1; q < end && n < LB_TEAM_SLOTS; ++n) {
                v[n] = std::atoi(q);
                while (q < end && *q != ',') ++q;
                if (q < end) ++q;
            }
            for (int i = 0; i < LB_TEAM_SLOTS; ++i)
                tp.team.pick[i] = (unsigned short)(i < n ? v[i] : 0);
            if (!tp.name.empty() && lb_teams_valid(&tp.team)) out.push_back(tp);
        }
        p = *end ? end + 1 : end;
    }
    return out;
}

bool teams_write(const std::vector<TeamPreset>& list, char* dst, size_t cap) {
    std::string o;
    for (const TeamPreset& tp : list) {
        if (!o.empty()) o += ';';
        o += tp.name;
        o += '=';
        for (int i = 0; i < LB_TEAM_SLOTS; ++i) {
            if (i) o += ',';
            o += std::to_string(tp.team.pick[i]);
        }
    }
    if (o.size() + 1 > cap) return false;
    std::snprintf(dst, cap, "%s", o.c_str());
    return true;
}

std::vector<TeamPreset> presets_parse() { return teams_parse(g_set.team_presets); }

bool presets_write(const std::vector<TeamPreset>& list) {
    return teams_write(list, g_set.team_presets, sizeof g_set.team_presets);
}

bool same_team(const LbTeam& a, const LbTeam& b) {
    for (int i = 0; i < LB_TEAM_SLOTS; ++i)
        if (a.pick[i] != b.pick[i]) return false;
    return true;
}

// docs/388: THE SAVE'S LAST FIVE ARMIES -- a recent cache, at most five, that
// the panel cannot override, of teams loaded from the save file with a date
// timestamp: a quick revert if the save's team is overwritten. Once a second,
// whether or not the tab is open:
// the loaded profile's army, when the game has committed one and it is not
// King's default (always in the list already), goes to the front stamped
// with the time it was read. A team met again moves to the front with a new
// stamp; the sixth falls off. The autosave persists it with the rest.
void save_cache_tick() {
    static int tick = 0;
    if (++tick < 60) return;
    tick = 0;
    LbTeam save;
    int confirmed = 0;
    if (!lb_teams_read_save(&save, &confirmed) || !confirmed ||
        lb_teams_is_default(&save))
        return;
    std::vector<TeamPreset> cache = teams_parse(g_set.team_save_cache);
    if (!cache.empty() && same_team(cache.front().team, save)) return;
    for (size_t i = 0; i < cache.size(); ++i)
        if (same_team(cache[i].team, save)) {
            cache.erase(cache.begin() + (long)i);
            break;
        }
    char when[32] = "?";
    const std::time_t now = std::time(nullptr);
    if (const std::tm* lt = std::localtime(&now))
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", lt);
    cache.insert(cache.begin(), TeamPreset{when, save});
    if (cache.size() > 5) cache.resize(5);
    teams_write(cache, g_set.team_save_cache, sizeof g_set.team_save_cache);
}

LbTeam team_of(const int* v) {
    LbTeam t;
    for (int i = 0; i < LB_TEAM_SLOTS; ++i) t.pick[i] = (unsigned short)v[i];
    if (!lb_teams_valid(&t)) lb_teams_king_default(&t);
    return t;
}

void team_put(int* v, const LbTeam& t) {
    for (int i = 0; i < LB_TEAM_SLOTS; ++i) v[i] = t.pick[i];
}

const char* faction_name(int f) {
    static const char* const k[7] = {"King", "Wizard", "Pirate",
                                     "Imperial", "Earth", "Alien",
                                     "Neutral"};   // docs/410: the creatures
    return (f >= 0 && f < 7) ? k[f] : "?";
}

// One head, as a button. `grey` draws it from the greyscale sheet; `alpha`
// is the game's translucency for a unit that is unlocked but not bought
// (button byte +0x0C = 10 of 31). Returns true when clicked.
bool unit_button(const char* id, int bp, float size, bool grey, float alpha,
                 bool selected) {
    // docs/410: the battle HUD's heads first (the front end's grid cut gives
    // the creatures and two pirates someone else's); a locked one is dimmed.
    SDL_Texture* hud = lb_teams_hud_atlas(g_team_renderer);
    SDL_Texture* tex = lb_teams_atlas(g_team_renderer, grey ? 1 : 0);
    float u0, v0, u1, v1;
    const ImVec4 bg = selected ? ImVec4(0.95f, 0.75f, 0.15f, 1.0f)
                               : ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    bool hit;
    if (hud && lb_teams_hud_icon_uv(bp, &u0, &v0, &u1, &v1)) {
        const float k = grey ? 0.4f : 1.0f;
        hit = ImGui::ImageButton(id, (ImTextureID)(intptr_t)hud,
                                 ImVec2(size, size), ImVec2(u0, v0),
                                 ImVec2(u1, v1), bg, ImVec4(k, k, k, alpha));
    } else if (tex && lb_teams_icon_uv(bp, &u0, &v0, &u1, &v1)) {
        hit = ImGui::ImageButton(id, (ImTextureID)(intptr_t)tex,
                                 ImVec2(size, size), ImVec2(u0, v0),
                                 ImVec2(u1, v1), bg,
                                 ImVec4(1.0f, 1.0f, 1.0f, alpha));
    } else {
        // No heads yet (before the game has booted): the button still works.
        ImGui::PushID(id);
        hit = ImGui::Button(lb_teams_unit_name(bp), ImVec2(size + 8, size + 8));
        ImGui::PopID();
    }
    return hit;
}

void unit_tooltip(int bp, int state) {
    if (!ImGui::IsItemHovered()) return;
    const char* st = state == 2   ? ""
                     : state == 1 ? "\nNot bought on this save"
                     : state == 0 ? "\nNot unlocked on this save"
                                  : "";
    ImGui::SetTooltip("%s (%s)%s", lb_teams_unit_name(bp),
                      faction_name(lb_teams_unit_faction(bp)), st);
}

// The army screen, drawn at `s` ImGui pixels per native pixel: the slot panel
// on the left, the selected slot's other units in the 5x5 grid on the right.
void team_screen(LbTeam* team, int* slot, float s) {
    const ImVec2 origin = ImGui::GetCursorPos();
    const float cell = 24.0f * s;
    for (int k = 0; k < LB_TEAM_SLOTS; ++k) {
        int x = 0, y = 0;
        lb_teams_slot_pos(k, &x, &y);
        ImGui::SetCursorPos(ImVec2(origin.x + (x - 20) * s, origin.y + (y - 11) * s));
        char id[16];
        std::snprintf(id, sizeof id, "##slot%d", k);
        const int bp = team->pick[k];
        const int st = lb_teams_unit_state(bp);
        if (unit_button(id, bp, cell, false, st == 1 ? 10.0f / 31.0f : 1.0f,
                        *slot == k))
            *slot = k;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: %s", lb_teams_slot_label(k),
                              lb_teams_unit_name(bp));
    }
    // The grid: the slot's candidates in the game's order, less the one in
    // the slot, as FUN_020B96FC fills it. A pick swaps in.
    int g = 0;
    const int n = lb_teams_slot_count(*slot);
    for (int k = 0; k < n && g < 25; ++k) {
        const int bp = lb_teams_slot_unit(*slot, k);
        if (bp == team->pick[*slot]) continue;
        const float x = (112 - 20 + 28 * (g % 5)) * s, y = (28 * (g / 5)) * s;
        ImGui::SetCursorPos(ImVec2(origin.x + x, origin.y + y));
        char id[16];
        std::snprintf(id, sizeof id, "##grid%d", bp);
        const int st = lb_teams_unit_state(bp);
        // Unlocked but not bought: translucent, as the game draws it. Not
        // unlocked: the game shows the faction emblem; here the grey head.
        if (unit_button(id, bp, cell, st == 0, st == 1 ? 10.0f / 31.0f : 1.0f,
                        false))
            team->pick[*slot] = (unsigned short)bp;
        unit_tooltip(bp, st);
        ++g;
    }
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + 146.0f * s));
    ImGui::Dummy(ImVec2(236.0f * s, 1.0f));
}

void tab_team_builder() {
    if (!tab_begin("Team Builder")) return;
    static int slot = 0, preset = 0;
    static std::string preset_label;   // docs/388: the choice, by name
    static char name[32] = "";
    static std::string msg;
    static bool msg_bad = false;
    LbTeam team = team_of(g_set.team_working);

    // docs/410: the neutral creatures as army units, at real prices.
    ImGui::Checkbox("Neutral Creatures in Armies", &g_set.creatures);
    help("Offers the Wolf and the Robot as Close Combat units and the\n"
         "Shark as Special Two, for your army and the Custom opponent.\n"
         "They are in the game's own army screen too, with their heads.\n"
         "The game gives them a price of 1 brick; on, they cost:\n"
         "Wolf 200 (build 420), Robot 80 (270), Shark 400 (660).\n"
         "The Shark swims: only a Shipyard builds it, so it is for water\n"
         "maps. Online, the host's setting applies.");
    badge(WIRED);
    if (lb_rules_host_flags() >= 0)
        ImGui::TextDisabled("In this lobby, the host's: %s",
                            lb_creatures_live() ? "On" : "Off");
    ImGui::Separator();

    // The dropdown: King (Free Play's default), the save's team when it is
    // not King's, the save's recent armies (docs/388), then the player's own
    // presets. Everything before the presets is read-only here.
    std::vector<TeamPreset> list;
    {
        TeamPreset king;
        king.name = "King (default)";
        lb_teams_king_default(&king.team);
        list.push_back(king);
        LbTeam save;
        int confirmed = 0;
        if (lb_teams_read_save(&save, &confirmed) && confirmed &&
            !lb_teams_is_default(&save))
            list.push_back(TeamPreset{"Save File Team", save});
        for (const TeamPreset& c : teams_parse(g_set.team_save_cache))
            list.push_back(TeamPreset{"Recent save team, " + c.name, c.team});
    }
    const size_t builtin = list.size();
    std::vector<TeamPreset> mine = presets_parse();
    list.insert(list.end(), mine.begin(), mine.end());
    // A new recent entry shifts every index after it, so the choice is kept
    // by its label.
    if (!preset_label.empty()) {
        preset = 0;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].name == preset_label) { preset = (int)i; break; }
    }
    if (preset >= (int)list.size()) preset = 0;

    // docs/388: choosing a team in the dropdown loads it; there is no Load
    // button.
    ImGui::SetNextItemWidth(230.0f * g_ui_scale);
    if (ImGui::BeginCombo("##preset", list[(size_t)preset].name.c_str())) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (i == builtin && i > 0) ImGui::Separator();
            if (ImGui::Selectable(list[i].name.c_str(), (int)i == preset)) {
                preset = (int)i;
                preset_label = list[i].name;
                team = list[i].team;
                msg = "Loaded " + list[i].name + ".";
                msg_bad = false;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Randomize")) {
        lb_teams_randomize(&team, lb_teams_unit_state(0) >= 0 ? 1 : 0);
        msg = "Each slot drawn at random.";
        msg_bad = false;
    }
    help("Choosing a team in the list puts it in the grid. Randomize draws\n"
         "each slot independently from the units this save can field (all\n"
         "units before a profile is loaded).\n\n"
         "\"Recent save team\" entries are the last five armies read from the\n"
         "save, newest first, with the time each was read: a way back if Set\n"
         "Team, or the game's own army screen, replaced one. They are kept\n"
         "automatically and cannot be saved over or deleted here.");
    ImGui::SetNextItemWidth(170.0f * g_ui_scale);
    ImGui::InputTextWithHint("##name", "preset name", name, sizeof name);
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        std::string nm;
        for (const char* c = name; *c; ++c)
            if (*c != '=' && *c != ';' && *c != '"' && *c != '\\') nm += *c;
        if (nm.empty() && preset >= (int)builtin)
            nm = list[(size_t)preset].name;   // overwrite the one selected
        if (nm.empty()) {
            msg = "Give the preset a name.";
            msg_bad = true;
        } else {
            bool found = false;
            for (TeamPreset& tp : mine)
                if (tp.name == nm) { tp.team = team; found = true; }
            if (!found) mine.push_back(TeamPreset{nm, team});
            if (presets_write(mine)) {
                msg = "Saved \"" + nm + "\".";
                msg_bad = false;
                preset = (int)builtin;
                for (size_t i = 0; i < mine.size(); ++i)
                    if (mine[i].name == nm) preset = (int)(builtin + i);
                preset_label = nm;
                name[0] = 0;
            } else {
                msg = "Too many presets to store.";
                msg_bad = true;
            }
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(preset < (int)builtin);
    if (ImGui::Button("Delete")) {
        const std::string nm = list[(size_t)preset].name;
        mine.erase(mine.begin() + (preset - (int)builtin));
        presets_write(mine);
        preset = 0;
        preset_label.clear();
        msg = "Deleted \"" + nm + "\".";
        msg_bad = false;
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    if (!lb_teams_atlas(g_team_renderer, 0))
        ImGui::TextDisabled("%s", lb_teams_icons_status());
    const float s = 1.5f * g_ui_scale;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
    team_screen(&team, &slot, s);
    ImGui::PopStyleVar();
    ImGui::Text("%s: %s", lb_teams_slot_label(slot),
                lb_teams_unit_name(team.pick[slot]));
    ImGui::Separator();

    const char* blocked = lb_teams_set_team_blocked();
    ImGui::BeginDisabled(blocked != nullptr);
    if (ImGui::Button("Set Team")) {
        char buf[160];
        msg_bad = !lb_teams_set_team(&team, buf, sizeof buf);
        msg = buf;
    }
    ImGui::EndDisabled();
    if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", blocked);
    ImGui::SameLine();
    if (ImGui::Button("Set Enemy Team")) {
        team_put(g_set.enemy_team, team);
        msg = "Free Play's Custom opponent is now this team.";
        msg_bad = false;
    }
    help("Set Team makes this your army (the save's own army record), as the\n"
         "game's army screen would, and only with units this save has\n"
         "unlocked and bought. It applies only outside battles (the pause\n"
         "menu counts as a battle), and never while hosting, in a lobby or\n"
         "browsing games.\n\n"
         "Set Enemy Team is what Free Play's opponent selector fields when\n"
         "set to Custom (the crown), after the six factions. Random (the ?)\n"
         "draws every slot at random each match.");
    if (blocked) ImGui::TextDisabled("Set Team: %s", blocked);
    if (!msg.empty()) {
        if (msg_bad)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", msg.c_str());
        else
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", msg.c_str());
    }
    // The enemy team as it stands.
    ImGui::TextDisabled("Custom opponent:");
    const LbTeam enemy = team_of(g_set.enemy_team);
    for (int k = 0; k < LB_TEAM_SLOTS; ++k) {
        ImGui::SameLine();
        char id[16];
        std::snprintf(id, sizeof id, "##enemy%d", k);
        unit_button(id, enemy.pick[k], 16.0f * g_ui_scale, false, 1.0f, false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: %s", lb_teams_slot_label(k),
                              lb_teams_unit_name(enemy.pick[k]));
    }
    badge(WIRED);
    team_put(g_set.team_working, team);
    tab_end();
}

// The ban list as bits over blueprints 0..191, from g_set.banned_units.
void bans_bits(unsigned char bits[24]) {
    std::memset(bits, 0, 24);
    for (const char* p = g_set.banned_units; *p;) {
        const int bp = std::atoi(p);
        if (bp >= 0 && bp < 192 && !lb_teams_is_king_default(bp))
            bits[bp >> 3] |= (unsigned char)(1u << (bp & 7));
        while (*p && *p != ',') ++p;
        if (*p) ++p;
    }
}

void bans_store(const unsigned char bits[24]) {
    std::string o;
    for (int bp = 0; bp < 192; ++bp)
        if ((bits[bp >> 3] >> (bp & 7)) & 1) {
            if (!o.empty()) o += ',';
            o += std::to_string(bp);
        }
    std::snprintf(g_set.banned_units, sizeof g_set.banned_units, "%s", o.c_str());
}

// docs/410: the ban list as a file, like the Balance values: the blueprint
// numbers, with the names beside them for a reader (ignored on import).
bool bans_export(const char* path, const unsigned char bits[24]) {
    std::string names;
    for (int bp = 0; bp < 192; ++bp)
        if ((bits[bp >> 3] >> (bp & 7)) & 1) {
            if (!names.empty()) names += ", ";
            names += lb_teams_unit_name(bp);
        }
    std::string o = "{\n  \"lb_bans\": 1,\n";
    json_put_str(o, "banned_units", g_set.banned_units, true);
    json_put_str(o, "names", names.c_str(), false);
    o += "}\n";
    FILE* f = std::fopen(path, "wb");
    const bool ok = f && std::fwrite(o.data(), 1, o.size(), f) == o.size();
    if (f) std::fclose(f);
    return ok;
}
// Into g_set.banned_units and `bits`; the count of banned units, or -1 when
// the file is not a ban list.
int bans_import(const char* path, unsigned char bits[24]) {
    std::string s;
    if (FILE* f = std::fopen(path, "rb")) {
        char chunk[4096];
        size_t got;
        while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0 && s.size() < (1u << 16))
            s.append(chunk, got);
        std::fclose(f);
    }
    if (s.find("\"lb_bans\"") == std::string::npos ||
        s.find("\"banned_units\"") == std::string::npos)
        return -1;
    char list[512] = "";
    json_get_str(s, "banned_units", list, sizeof list);
    std::snprintf(g_set.banned_units, sizeof g_set.banned_units, "%s", list);
    bans_bits(bits);   // drops anything out of range, and the King team
    bans_store(bits);
    int n = 0;
    for (int bp = 0; bp < 192; ++bp) n += (bits[bp >> 3] >> (bp & 7)) & 1;
    return n;
}

// docs/419: LB_BAN_ROUNDTRIP=<path> exports the current list there, clears
// it, imports the file back and logs whether the list came back the same:
// the Export and Import buttons' own code, without their file dialogs.
// LB_BAN_ROUNDTRIP_SET=<list> first sets a list to try. Test aid only.
void bans_roundtrip_if_asked() {
    static bool done = false;
    const char* path = std::getenv("LB_BAN_ROUNDTRIP");
    if (done || !path || !lb_teams_unit_name(1)) return;
    done = true;
    const std::string mine = g_set.banned_units;   // put back afterwards
    if (const char* set = std::getenv("LB_BAN_ROUNDTRIP_SET"))
        std::snprintf(g_set.banned_units, sizeof g_set.banned_units, "%s", set);
    unsigned char bits[24], back[24];
    bans_bits(bits);
    bans_store(bits);
    const std::string before = g_set.banned_units;
    const bool wrote = bans_export(path, bits);
    g_set.banned_units[0] = '\0';
    const int n = bans_import(path, back);
    std::fprintf(stderr, "[lb_bans] round trip through %s: wrote %s, read %d unit(s), "
                 "list %s (\"%s\" -> \"%s\")\n", path, wrote ? "yes" : "NO", n,
                 before == g_set.banned_units && std::memcmp(bits, back, 24) == 0
                     ? "IDENTICAL" : "DIFFERS",
                 before.c_str(), g_set.banned_units);
    std::fflush(stderr);
    std::snprintf(g_set.banned_units, sizeof g_set.banned_units, "%s", mine.c_str());
}

// ---- docs/429: map bans, per-map unit bans, ban presets ---------------------

bool ban_preset_apply(int i, std::string* msg);
bool ban_preset_export(const char* path);
bool ban_preset_import(const char* path);

// Once per frame: this machine's map settings to lb_mapbans.cpp (parsed only
// when they changed). Test aids, all off by default: LB_RANDOM_MAPS=1 adds the
// Random entries, LB_TOURNEY_TEAM=1 turns Random team (tourney) on, and
// LB_TEST_BAN_PRESET=<index> applies that bundled preset once the game can
// name its units.
void maps_sync_local() {
    static std::string last;
    static bool preset_done = false;
    if (!preset_done) {
        if (const char* p = std::getenv("LB_TEST_BAN_PRESET")) {
            std::string m;
            if (ban_preset_apply(std::atoi(p), &m)) {
                preset_done = true;
                std::fprintf(stderr, "[lb_maps] test preset %s: %s\n", p, m.c_str());
                std::fflush(stderr);
            } else if (m.find("not ") == std::string::npos) {
                preset_done = true;   // a real failure: do not retry forever
            }
        } else {
            preset_done = true;
        }
    }
    // LB_BANPRESET_ROUNDTRIP=<path>: once a preset is in force, export it, clear
    // the lists, import the file and log whether they came back the same.
    if (const char* path = std::getenv("LB_BANPRESET_ROUNDTRIP")) {
        static bool rt_done = false;
        if (!rt_done && g_set.ban_preset[0]) {
            rt_done = true;
            {   // the stored order is by blueprint: compare like with like
                unsigned char nb[24];
                bans_bits(nb);
                bans_store(nb);
            }
            const std::string a = std::string(g_set.banned_units) + "|" + g_set.banned_maps +
                                  "|" + g_set.unit_map_bans + "|" + g_set.ban_preset + "|" +
                                  g_set.ban_preset_maps;
            const bool wrote = ban_preset_export(path);
            g_set.banned_units[0] = g_set.banned_maps[0] = g_set.unit_map_bans[0] = '\0';
            g_set.ban_preset[0] = g_set.ban_preset_maps[0] = '\0';
            const bool read = ban_preset_import(path);
            const std::string b = std::string(g_set.banned_units) + "|" + g_set.banned_maps +
                                  "|" + g_set.unit_map_bans + "|" + g_set.ban_preset + "|" +
                                  g_set.ban_preset_maps;
            std::fprintf(stderr, "[lb_maps] ban preset round trip through %s: wrote %s, "
                         "read %s, lists %s (\"%s\")\n", path, wrote ? "yes" : "NO",
                         read ? "yes" : "NO", a == b ? "IDENTICAL" : "DIFFER", b.c_str());
            std::fflush(stderr);
        }
    }
    // LB_PANEL_OPEN_AT=<seconds>: opens the settings panel once, that long after
    // launch (a held two-instance test photographs a joiner's pages this way).
    if (const char* v = std::getenv("LB_PANEL_OPEN_AT")) {
        static const auto t0 = std::chrono::steady_clock::now();
        static bool opened = false;
        if (!opened && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                               .count() >= std::atof(v)) {
            opened = true;
            g_visible = true;
        }
    }
    // LB_BANNED_MAPS=<tokens> sets the map bans (for example every stock map,
    // "s1,s2,...,s30", to list only mod maps); LB_UNIT_MAP_BANS the per-map
    // unit bans.
    if (const char* v = std::getenv("LB_BANNED_MAPS"))
        std::snprintf(g_set.banned_maps, sizeof g_set.banned_maps, "%s", v);
    if (const char* v = std::getenv("LB_UNIT_MAP_BANS"))
        std::snprintf(g_set.unit_map_bans, sizeof g_set.unit_map_bans, "%s", v);
    if (const char* v = std::getenv("LB_BANNED_UNITS"))
        std::snprintf(g_set.banned_units, sizeof g_set.banned_units, "%s", v);
    if (const char* v = std::getenv("LB_RANDOM_MAPS")) g_set.random_maps = std::atoi(v) != 0;
    if (const char* v = std::getenv("LB_TOURNEY_TEAM")) g_set.tourney_team = std::atoi(v) != 0;
    std::string key = std::string(g_set.banned_maps) + "|" + g_set.unit_map_bans + "|" +
                      g_set.ban_preset_maps + "|" + (g_set.ban_preset[0] ? "1" : "0") +
                      (g_set.tourney_team ? "t" : "-") + (g_set.random_maps ? "r" : "-") +
                      std::to_string(lb_mods_count());
    if (key == last) return;
    last = key;
    lb_maps_set_local(g_set.banned_maps, g_set.unit_map_bans, g_set.ban_preset_maps,
                      g_set.ban_preset[0] ? 1 : 0, g_set.tourney_team ? 1 : 0,
                      g_set.random_maps ? 1 : 0);
}


std::string g_bp_msg;          // the presets section's last result
bool g_bp_msg_bad = false;
int g_bp_choice = 1;           // the preset picker
char g_bp_path[512] = "ban-preset.json";

// The tokens of a '+' list, one by one.
void tok_split(const std::string& s, char sep, std::vector<std::string>* out) {
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out->push_back(s.substr(a, b - a));
        a = b + 1;
    }
}

// A ban preset as a file: the three lists (language-independent numbers and
// hashes) with the names beside them for a reader, ignored on import.
bool ban_preset_export(const char* path) {
    char units[512], maps_names[1024], umb_names[1024];
    units[0] = '\0';
    for (int bp = 0, first = 1; bp < 192; ++bp) {
        unsigned char b[24];
        bans_bits(b);
        if (!((b[bp >> 3] >> (bp & 7)) & 1)) continue;
        std::snprintf(units + std::strlen(units), sizeof units - std::strlen(units),
                      "%s%s", first ? "" : ", ", lb_teams_unit_name(bp));
        first = 0;
    }
    lb_maps_names_of(g_set.banned_maps, maps_names, sizeof maps_names);
    std::string um;
    std::vector<std::string> es;
    tok_split(g_set.unit_map_bans, ';', &es);
    for (const std::string& e : es) {
        const size_t c = e.find(':');
        if (c == std::string::npos) continue;
        lb_maps_names_of(e.c_str() + c + 1, umb_names, sizeof umb_names);
        if (!um.empty()) um += "; ";
        um += std::string(lb_teams_unit_name(std::atoi(e.c_str()))) + " on " + umb_names;
    }
    std::string names = std::string("Units: ") + units + ". Maps: " + maps_names +
                        ". On some maps only: " + um + ".";
    std::string o = "{\n  \"lb_ban_preset\": 1,\n";
    json_put_str(o, "name", g_set.ban_preset[0] ? g_set.ban_preset : "My bans", true);
    json_put_str(o, "banned_units", g_set.banned_units, true);
    json_put_str(o, "banned_maps", g_set.banned_maps, true);
    json_put_str(o, "unit_map_bans", g_set.unit_map_bans, true);
    json_put_str(o, "names", names.c_str(), false);
    o += "}\n";
    FILE* f = std::fopen(path, "wb");
    const bool ok = f && std::fwrite(o.data(), 1, o.size(), f) == o.size();
    if (f) std::fclose(f);
    return ok;
}

bool ban_preset_import(const char* path) {
    std::string s;
    if (FILE* f = std::fopen(path, "rb")) {
        char chunk[4096];
        size_t got;
        while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0 && s.size() < (1u << 16))
            s.append(chunk, got);
        std::fclose(f);
    }
    if (s.find("\"lb_ban_preset\"") == std::string::npos) return false;
    char name[96] = "Imported";
    json_get_str(s, "name", name, sizeof name);
    g_set.banned_units[0] = g_set.banned_maps[0] = g_set.unit_map_bans[0] = '\0';
    json_get_str(s, "banned_units", g_set.banned_units, sizeof g_set.banned_units);
    json_get_str(s, "banned_maps", g_set.banned_maps, sizeof g_set.banned_maps);
    json_get_str(s, "unit_map_bans", g_set.unit_map_bans, sizeof g_set.unit_map_bans);
    unsigned char bits[24];
    bans_bits(bits);    // drops anything out of range, and the King team
    bans_store(bits);
    std::snprintf(g_set.ban_preset, sizeof g_set.ban_preset, "%s", name);
    std::snprintf(g_set.ban_preset_maps, sizeof g_set.ban_preset_maps, "%s",
                  g_set.banned_maps);
    return true;
}

// Preset `i` applied to the lists. Bundled "None" clears every ban and leaves no
// preset active.
bool ban_preset_apply(int i, std::string* msg) {
    char units[512], maps[512], umb[768], note[160];
    if (!lb_maps_preset_resolve(i, units, sizeof units, maps, sizeof maps, umb,
                                sizeof umb, note, sizeof note)) {
        *msg = note;
        return false;
    }
    std::snprintf(g_set.banned_units, sizeof g_set.banned_units, "%s", units);
    std::snprintf(g_set.banned_maps, sizeof g_set.banned_maps, "%s", maps);
    std::snprintf(g_set.unit_map_bans, sizeof g_set.unit_map_bans, "%s", umb);
    if (i == 0) {
        g_set.ban_preset[0] = '\0';
        g_set.ban_preset_maps[0] = '\0';
    } else {
        std::snprintf(g_set.ban_preset, sizeof g_set.ban_preset, "%s",
                      lb_maps_preset_name(i));
        std::snprintf(g_set.ban_preset_maps, sizeof g_set.ban_preset_maps, "%s", maps);
    }
    *msg = i == 0 ? std::string("Cleared: no bans.")
                  : std::string("Applied \"") + lb_maps_preset_name(i) + "\". " + note;
    return true;
}

// The Presets section at the foot of Ban Units and Ban Maps.
void draw_ban_presets(bool idle) {
    ImGui::Separator();
    ImGui::TextUnformatted("Presets");
    guide("A preset fills the unit bans, the map bans and the unit bans on some maps in "
          "one go; you can change them after. Export and Import keep a set as a file. "
          "Online, the host's bans apply to everyone, so the host's preset is the "
          "players'.");
    ImGui::BeginDisabled(!idle);
    const int n = lb_maps_preset_count();
    ImGui::SetNextItemWidth(360.0f * g_ui_scale);
    if (ImGui::BeginCombo("Preset", lb_maps_preset_name(g_bp_choice))) {
        for (int i = 0; i < n; ++i)
            if (ImGui::Selectable(lb_maps_preset_name(i), i == g_bp_choice))
                g_bp_choice = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply")) g_bp_msg_bad = !ban_preset_apply(g_bp_choice, &g_bp_msg);
    ImGui::SetItemTooltip("Replaces your unit and map bans with the preset's.");
    ImGui::TextDisabled("In force: %s", g_set.ban_preset[0] ? g_set.ban_preset
                                                            : "None (your own bans)");
    ImGui::InputText("File", g_bp_path, sizeof g_bp_path);
    if (ImGui::Button("Export bans (.json)...") &&
        lb_pick_json_file(g_bp_path, (int)sizeof g_bp_path, 1)) {
        g_bp_msg_bad = !ban_preset_export(g_bp_path);
        g_bp_msg = g_bp_msg_bad ? "Could not write that file." : "Exported the bans.";
    }
    ImGui::SetItemTooltip("Saves the unit bans, the map bans and the bans on some maps.");
    ImGui::SameLine();
    if (ImGui::Button("Import bans (.json)...") &&
        lb_pick_json_file(g_bp_path, (int)sizeof g_bp_path, 0)) {
        g_bp_msg_bad = !ban_preset_import(g_bp_path);
        g_bp_msg = g_bp_msg_bad ? "That is not a ban preset file."
                                : std::string("Imported \"") + g_set.ban_preset + "\".";
    }
    ImGui::SetItemTooltip("Loads a file made by Export. It replaces your bans.");
    ImGui::Spacing();
    ImGui::Checkbox("Random team (tourney)", &g_set.tourney_team);
    help("Every player in a game you host, and the Random opponent in Free Play, fields\n"
         "a random army drawn from a seed, skipping every banned unit (on this match's\n"
         "map too). Players are not limited to what their save has bought.\n"
         "Online, the host sends the seed and every machine builds the same teams.");
    if (g_set.tourney_team) {
        ImGui::SameLine();
        ImGui::TextDisabled("Seed: %u", lb_maps_current_seed());
    }
    ImGui::EndDisabled();
    if (!g_bp_msg.empty())
        ImGui::TextColored(g_bp_msg_bad ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f)
                                        : ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                           "%s", g_bp_msg.c_str());
}

// One unit's maps, for the right-click popup and the table's Edit.
int g_umb_unit = -1;
bool g_umb_request = false;   // OpenPopup must run at the page's own ID level

void draw_umb_popup(bool idle) {
    if (g_umb_request) {
        g_umb_request = false;
        ImGui::OpenPopup("Ban on maps");
    }
    if (!ImGui::BeginPopup("Ban on maps")) return;
    const int bp = g_umb_unit;
    ImGui::Text("%s", lb_teams_unit_name(bp));
    guide("Tick the maps this unit is banned on. It stays allowed on the others.");
    ImGui::BeginDisabled(!idle);
    char toks[256];
    lb_maps_umb_get(g_set.unit_map_bans, bp, toks, sizeof toks);
    std::string cur = toks;
    bool changed = false;
    ImGui::BeginChild("##umbmaps", ImVec2(340.0f * g_ui_scale, 280.0f * g_ui_scale), true);
    const int count = lb_maps_count();
    for (int i = 0; i < count; ++i) {
        char tok[32], name[64];
        int players = 0;
        if (!lb_maps_entry(i, tok, sizeof tok, name, sizeof name, &players)) continue;
        std::vector<std::string> v;
        tok_split(cur, '+', &v);
        bool on = std::find(v.begin(), v.end(), std::string(tok)) != v.end();
        std::string label = name;
        if (i >= 30) label += " (mod)";
        if (ImGui::Checkbox(label.c_str(), &on)) {
            auto it = std::find(v.begin(), v.end(), std::string(tok));
            if (on && it == v.end()) v.push_back(tok);
            if (!on && it != v.end()) v.erase(it);
            cur.clear();
            for (const std::string& t : v) {
                if (!cur.empty()) cur += '+';
                cur += t;
            }
            changed = true;
        }
    }
    ImGui::EndChild();
    if (changed) lb_maps_umb_set(g_set.unit_map_bans, sizeof g_set.unit_map_bans, bp,
                                 cur.c_str());
    if (ImGui::Button("Clear")) {
        lb_maps_umb_set(g_set.unit_map_bans, sizeof g_set.unit_map_bans, bp, "");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Done")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void tab_ban_units() {
    if (!tab_begin("Ban Units")) return;
    const int net = lb_net_guest_state();
    const bool idle = (net == 0 || net == -1);
    const bool joined = net == 9 || net == 6 || net == 7 || net == 5 || net == 12;
    guide("Units greyed out here are not allowed in online games you host: a player who "
          "brings one is switched to King as the match starts. Click a unit to ban it "
          "everywhere; right-click it to ban it only on the maps you pick. Online, the "
          "host's bans apply to everyone.");
    ImGui::TextDisabled("The King team cannot be banned, as it is the default.");
    unsigned char bits[24];
    bans_bits(bits);
    unsigned char shown[24];
    std::memcpy(shown, bits, sizeof shown);
    unsigned from = 0;
    const bool got = joined && lb_teams_received_bans(shown, 24, &from);
    char rmaps[512] = "", rumb[768] = "";
    const bool got_maps = joined && lb_maps_received(rmaps, sizeof rmaps, rumb, sizeof rumb);
    const char* umb_shown = joined ? (got_maps ? rumb : "") : g_set.unit_map_bans;
    if (joined)
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                           got ? "Showing the host's bans for this game."
                               : "Waiting for the host's bans.");
    else if (!idle)
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                           "Hosting: these bans are sent to players who join. "
                           "They are fixed until you leave.");
    ImGui::Separator();
    const float size = 24.0f * 1.25f * g_ui_scale;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
    const float pitch = size + 2.0f + ImGui::GetStyle().ItemSpacing.x;
    int per_row = (int)(ImGui::GetContentRegionAvail().x / pitch);
    if (per_row < 4) per_row = 4;
    bool changed = false;
    ImGui::BeginDisabled(!idle);
    for (int slot = 0; slot < LB_TEAM_SLOTS; ++slot) {
        ImGui::TextDisabled("%s", lb_teams_slot_label(slot));
        const int n = lb_teams_slot_count(slot);
        for (int k = 0; k < n; ++k) {
            const int bp = lb_teams_slot_unit(slot, k);
            if (k % per_row) ImGui::SameLine();
            const bool king = lb_teams_is_king_default(bp) != 0;
            const bool banned = !king && ((shown[bp >> 3] >> (bp & 7)) & 1);
            char toks[256];
            const bool on_maps = !king && lb_maps_umb_get(umb_shown, bp, toks, sizeof toks);
            char id[16];
            std::snprintf(id, sizeof id, "##ban%d", bp);
            ImGui::BeginDisabled(king);
            if (unit_button(id, bp, size, banned,
                            banned ? 10.0f / 31.0f : on_maps ? 0.55f : 1.0f,
                            false) && !king) {
                bits[bp >> 3] ^= (unsigned char)(1u << (bp & 7));
                changed = true;
            }
            ImGui::EndDisabled();
            if (!king && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                g_umb_unit = bp;
                g_umb_request = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                char where[512] = "";
                if (on_maps) lb_maps_names_of(toks, where, sizeof where);
                ImGui::SetTooltip("%s (%s)%s%s%s", lb_teams_unit_name(bp),
                                  faction_name(lb_teams_unit_faction(bp)),
                                  king ? "\nThe King team cannot be banned, as "
                                         "it is the default."
                                       : banned ? "\nBanned everywhere" : "",
                                  on_maps ? "\nBanned on: " : "", where);
            }
        }
    }
    ImGui::PopStyleVar();
    if (ImGui::Button("Allow all")) {
        std::memset(bits, 0, sizeof bits);
        g_set.unit_map_bans[0] = '\0';
        changed = true;
    }
    ImGui::SetItemTooltip("Clears the unit bans, on every map and on some maps only.");
    // docs/410: the list as a file, like the Balance values: the blueprint
    // numbers, with the names beside them for a reader (ignored on import).
    static char path[512] = "bans.json";
    static std::string msg;
    static bool msg_bad = false;
    ImGui::SameLine();
    if (ImGui::Button("Export...") &&
        lb_pick_json_file(path, (int)sizeof path, 1)) {
        msg_bad = !bans_export(path, bits);
        msg = msg_bad ? "Could not write that file." : "Exported the ban list.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Import...") &&
        lb_pick_json_file(path, (int)sizeof path, 0)) {
        const int n = bans_import(path, bits);
        if (n < 0) {
            msg = "That is not a ban list file.";
            msg_bad = true;
        } else {
            changed = true;
            msg = "Imported " + std::to_string(n) + " banned unit(s).";
            msg_bad = false;
        }
    }
    ImGui::EndDisabled();
    badge(WIRED);
    if (!msg.empty())
        ImGui::TextColored(msg_bad ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f)
                                   : ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                           "%s", msg.c_str());
    if (changed) bans_store(bits);
    // docs/429: the units banned on some maps only, with the maps beside them.
    {
        std::vector<std::string> es;
        tok_split(umb_shown, ';', &es);
        if (!es.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Banned on some maps only (right-click a unit to change):");
            if (ImGui::BeginTable("##umbtab", 3, ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Banned on", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthFixed, 70.0f * g_ui_scale);
                ImGui::TableHeadersRow();
                for (const std::string& e : es) {
                    const size_t c = e.find(':');
                    if (c == std::string::npos) continue;
                    const int bp = std::atoi(e.c_str());
                    char where[1024];
                    lb_maps_names_of(e.c_str() + c + 1, where, sizeof where);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(lb_teams_unit_name(bp));
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", where);
                    ImGui::TableNextColumn();
                    ImGui::PushID(bp);
                    ImGui::BeginDisabled(!idle);
                    if (ImGui::SmallButton("Edit")) {
                        g_umb_unit = bp;
                        g_umb_request = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }
    draw_umb_popup(idle);
    draw_ban_presets(idle);
    tab_end();
}

// docs/429: Balance > Ban Maps.
void tab_ban_maps() {
    if (!tab_begin("Ban Maps")) return;
    const int net = lb_net_guest_state();
    const bool idle = (net == 0 || net == -1);
    const bool joined = net == 9 || net == 6 || net == 7 || net == 5 || net == 12;
    guide("Maps ticked here are left out of the map list in Free Play and in games you "
          "host. Online, the host's list applies to everyone. The box under this text adds "
          "a Random entry to the map lists, which picks one of the maps left.");
    char rmaps[512] = "", rumb[768] = "";
    const bool got = joined && lb_maps_received(rmaps, sizeof rmaps, rumb, sizeof rumb);
    const char* list = joined ? (got ? rmaps : "") : g_set.banned_maps;
    if (joined)
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                           got ? "Showing the host's map bans for this game."
                               : "Waiting for the host's map bans.");
    else if (!idle)
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                           "Hosting: these bans are sent to players who join. "
                           "They are fixed until you leave.");
    ImGui::BeginDisabled(!idle);
    ImGui::Checkbox("Add Random to the map lists", &g_set.random_maps);
    help("Puts \"Random\" first in the Free Play and multiplayer map lists. It picks one\n"
         "of the maps that are not banned. With a ban preset in force there is also\n"
         "\"Random (tourney)\", which picks only from that preset's allowed maps.\n"
         "In a game you host, you draw it and the lobby shows the result.");
    ImGui::EndDisabled();
    ImGui::Separator();
    const int count = lb_maps_count();
    int banned_n = 0;
    ImGui::BeginDisabled(!idle);
    const float col_x0 = ImGui::GetCursorPosX();
    const float col_w = ImGui::GetContentRegionAvail().x * 0.5f;
    for (int i = 0; i < count; ++i) {
        char tok[32], name[64];
        int players = 0;
        if (!lb_maps_entry(i, tok, sizeof tok, name, sizeof name, &players)) continue;
        if (i == 30) ImGui::TextDisabled("Mod maps (from your mods folder)");
        if (i < 30 && (i & 1)) ImGui::SameLine(col_x0 + col_w);   // two columns
        bool on = lb_maps_list_has(list, tok) != 0;
        banned_n += on;
        char label[160];
        std::snprintf(label, sizeof label, "%s  (%d players)##map%d", name, players, i);
        if (ImGui::Checkbox(label, &on) && idle)
            lb_maps_list_toggle(g_set.banned_maps, sizeof g_set.banned_maps, tok);
    }
    if (ImGui::Button("Allow all")) g_set.banned_maps[0] = '\0';
    ImGui::SetItemTooltip("Clears the map bans.");
    ImGui::EndDisabled();
    ImGui::TextDisabled("%d map(s) banned. At most 16 mod maps can be banned.", banned_n);
    badge(WIRED);
    draw_ban_presets(idle);
    tab_end();
}

void tab_music() {
    if (!tab_begin("Music")) return;
    // Measured: on a castle map with an EARTH builder on the field, the
    // game's own side word reads 0 (King).
    ImGui::TextDisabled("Stock, Free Play and multiplayer always use the King "
                        "tracks.");
    ImGui::Separator();
    // docs/387: 4..9 always play one faction's music, whatever the map.
    const char* modes[] = {"Vanilla (King tracks)", "Random",
                           "Faction-aligned", "Map-specific",
                           "Always King", "Always Wizard", "Always Pirate",
                           "Always Imperial", "Always Earth", "Always Alien"};
    ImGui::Combo("Mode", &g_set.music_mode, modes, IM_ARRAYSIZE(modes));
    // Faction-aligned reads the side off YOUR BUILDER's blueprint: there is
    // exactly one builder per faction and every side has one, so it still
    // works in a campaign mission that starts without a hero. Map-specific
    // reads the tileset the level actually loaded -- King, Pirate or Mars --
    // which is what "castle / pirate / space" means to the cartridge.
    help("Vanilla overrides nothing.\n"
         "Faction-aligned follows your builder. Map-specific plays one of\n"
         "the map's two factions, picked each match: King or Wizard on a\n"
         "castle map, Pirate or Imperial on a pirate map, Earth or Alien on\n"
         "Mars. Random picks any faction once per match.\n"
         "Always <faction> plays that faction's music in every battle,\n"
         "whatever the map (menus keep their own theme).");
    badge(WIRED);
    ImGui::Spacing();
    // WHAT IS ACTUALLY PLAYING, read back from the game rather than from what
    // this port asked for. "The setting is wired" and "the setting took
    // effect" are different claims and only the second one is audible.
    {
        int side = -1, kind = -1, strm = -1;
        lb_music_state(&side, &kind, &strm);
        static const char* const kSideName[6] = {
            "King", "Wizard", "Pirate", "Imperial", "Earth", "Alien"};
        static const char* const kKindName[4] = {
            "calm", "battle", "victory", "defeat"};
        ImGui::Text("Now: side %d (%s), %s, STRM base %d",
                    side,
                    (side >= 0 && side < 6) ? kSideName[side] : "?",
                    (kind >= 0 && kind < 4) ? kKindName[kind] : "?",
                    strm);
        const int want = lb_music_wanted_side();
        if (want < 0)
            ImGui::TextDisabled("  This port is not overriding it.");
        else
            ImGui::TextDisabled("  This port wants side %d (%s).", want,
                                (want < 6) ? kSideName[want] : "?");
    }
    ImGui::Spacing();
    // docs/427: the notification sound -- local only, its own audio device.
    ImGui::Separator();
    guide("Notification sound: a short sound when another player's chat message or a "
          "notice arrives. Local only: it plays on your computer, never online.");
    {
        const char* items[9];
        for (int i = 0; i < 9; ++i) items[i] = lb_sfx_choice_name(i);
        ImGui::Combo("Notification sound", &g_set.notify_sound, items, 9);
        ImGui::SliderInt("Volume##notify", &g_set.notify_volume, 0, 100, "%d%%");
        if (ImGui::Button("Play")) lb_sfx_preview();
        ImGui::SetItemTooltip("Plays the chosen sound once (it is read from your own ROM).");
    }
    ImGui::Spacing();
    // HOW IT WORKS, kept here rather than printed at the player.
    //
    // The cartridge picks a track with func_02089c38(side, kind), a nested
    // jump table whose arms are `movs r0, #imm; bx lr` -- which is why
    // docs/98's six searches for the base ids as DATA all failed. The 24
    // immediates read out of it match the SDAT's faction sets exactly,
    // including that Imperial has seven calm tracks and nine battle ones. The
    // side it is called with is one word at 0x0214B41C+0x0C, and that word is
    // what this setting writes.
    tab_end();
}

// ── The top screen, as an overlay panel ────────────────────────────────────
//
// The main game window is the touch screen; the top screen is a movable and
// scalable ImGui overlay that starts in the lower-right corner.
//
// The pixels arrive from present_screens each frame. They cannot be drawn from
// the existing top-screen texture because that belongs to the TOP renderer and
// this overlay is bound to the bottom one, so they are re-uploaded into a
// texture of our own.
const void* g_top_pixels = nullptr;
// The panel's own scrubbed copy of the top screen, with the cartridge's camera
// box painted out. Never the frontend's buffer: see draw_top_screen.
std::vector<uint32_t> g_top_scrub;
// DID THIS FRAME'S TOP SCREEN ACTUALLY CONTAIN THE CARTRIDGE'S CAMERA BOX?
//
// Doubles as "is the minimap on the top screen right now", which is the real
// precondition for drawing the corrected box -- see draw_top_screen.
bool g_top_box_found = false;
int g_top_w = 0, g_top_h = 0;
SDL_Texture* g_top_tex = nullptr;
int g_top_tex_w = 0, g_top_tex_h = 0;
// LB_NOTOP=1 starts with the top-screen panel hidden.
//
// It is a HARNESS switch, and it exists because that panel has now broken two
// probes in the same way. ImGui windows swallow the clicks under them, the
// panel is drawn in the middle of the host window, and the guest's native 256
// columns are drawn in the middle too -- so a probe that clicks an in-band
// target through the frontend aims straight underneath it. The last one lost a
// whole run to it: a click grid around the hero reported 0 hits from 25 clicks
// that never reached the game at all, which reads as "selection is broken"
// when it is the probe that is.
//
// Only the INITIAL value. The checkbox still owns it from there, so nothing
// about the interactive UI changes.
bool g_top_visible = [] {
    const char* v = std::getenv("LB_NOTOP");
    return !(v && v[0] != '0');
}();

// The window's coverage in map pixels, published by draw_wide_field() so the
// minimap overlay can draw the box the cartridge cannot.
float g_view_x0 = 0.0f, g_view_y0 = 0.0f, g_view_x1 = 0.0f, g_view_y1 = 0.0f;
bool g_view_valid = false;

// Map a click to where the player actually pointed.
//
// With the wide field up, the window shows a REGION OF THE MAP, not the stock
// 256x192 screen, so the frontend's window-to-guest mapping sends the touch to
// an unrelated place -- clicks landing nowhere near the cursor. draw_wide_field
// publishes the covered region; this converts a normalised window position
// through it and back into guest screen coordinates, which is what the
// simulation expects.
//
// (u, v) are fractions of the window. Returns 0 when the wide field is not up,
// leaving the stock path alone.
extern "C" int lb_overlay_wide_touch(float u, float v, int* tx, int* ty) {
    if (!g_wide_field || !g_view_valid) return 0;
    int camx = 0, camy = 0;
    if (!lb_field_camera(&camx, &camy)) return 0;
    const float mx = g_view_x0 + u * (g_view_x1 - g_view_x0);
    const float my = g_view_y0 + v * (g_view_y1 - g_view_y0);
    int sx = (int)(mx - (float)camx);
    int sy = (int)(my - (float)camy);
    // Outside the camera the guest has no touch coordinate to give: the
    // simulation only accepts a point on its own screen. Clamping keeps the
    // click on the nearest edge rather than teleporting it somewhere arbitrary.
    if (sx < 0) sx = 0;
    if (sx > 255) sx = 255;
    if (sy < 0) sy = 0;
    if (sy > 191) sy = 191;
    if (tx) *tx = sx;
    if (ty) *ty = sy;
    return 1;
}
// FIND THE CARTRIDGE'S CAMERA BOX INSTEAD OF PREDICTING IT (docs/346).
//
// The defect: sometimes the original camera bounding box was still visible on
// the minimap alongside the one which represents the adaptive game window's
// aspect.
//
// The erase used to PREDICT where the cartridge drew its box -- from
// lb_widemap_cam_px(), the live pixel camera -- and scrub only within +/-2 px
// of that. Three ways that misses, each producing exactly "sometimes":
//
//   * that accessor returns invalid on ~1% of frames (docs/341), and the whole
//     erase was skipped on those;
//   * the cartridge draws its box with the camera IT used for that frame, and
//     during a pan the live reading can be a camera step away -- more than the
//     2 px margin covers on the minimap;
//   * lb_field_map_to_minimap() documents that it takes MAP coordinates and
//     adds the camera->map offset itself, and it was being handed the camera.
//
// And a miss did double damage: g_top_box_found (erased >= 8) is also the gate
// for drawing the CORRECTED box, so on a missed frame the original stayed and
// the corrected one could vanish.
//
// The box is a one-pixel near-white outline, (251,251,251) per docs/180, about
// 16x18 on the minimap, and nothing else inside the minimap is that bright
// (terrain tops out at (130,162,113)). So: within the minimap's own extent,
// take the first horizontal run of >= 12 such pixels as the top edge, confirm
// a matching run 12..24 rows below as the bottom edge, and return that
// rectangle. No camera involved.
extern "C" int lb_widemap_map_px(int* w, int* h);   // lb_widemap.cpp
static bool lb_find_cartridge_box(const uint32_t* src, int* ox0, int* oy0,
                                  int* ox1, int* oy1) {
    auto is_box = [](uint32_t px) {
        return ((px >> 16) & 0xFFu) >= 200u && ((px >> 8) & 0xFFu) >= 200u &&
               (px & 0xFFu) >= 200u;
    };
    // The minimap's extent: the map's corners through the same fit, with a
    // generous margin, clamped to the screen. Without a map size the whole
    // screen would include the white resource-bar digits, so refuse instead.
    int mw = 0, mh = 0;
    if (!lb_widemap_map_px(&mw, &mh) || mw <= 0 || mh <= 0) return false;
    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
    lb_field_map_to_minimap(-64, -64, &ax, &ay);
    lb_field_map_to_minimap(mw + 64, mh + 64, &bx, &by);
    int rx0 = (int)ax, ry0 = (int)ay, rx1 = (int)bx, ry1 = (int)by;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > 255) rx1 = 255;
    if (ry1 > 191) ry1 = 191;
    if (rx1 <= rx0 || ry1 <= ry0) return false;

    int tx0 = -1, tx1 = -1, ty = -1;
    for (int y = ry0; y <= ry1 && ty < 0; ++y) {
        int run = 0;
        for (int x = rx0; x <= rx1 + 1; ++x) {
            if (x <= rx1 && is_box(src[y * 256 + x])) {
                ++run;
                continue;
            }
            if (run >= 12) {
                tx0 = x - run;
                tx1 = x - 1;
                ty = y;
                break;
            }
            run = 0;
        }
    }
    if (ty < 0) return false;
    // The bottom edge: most of the same columns, 12..24 rows further down.
    const int need = (tx1 - tx0 + 1) * 3 / 4;
    for (int y = ty + 12; y <= ty + 24 && y <= ry1; ++y) {
        int cnt = 0;
        for (int x = tx0; x <= tx1; ++x)
            if (is_box(src[y * 256 + x])) ++cnt;
        if (cnt >= need) {
            *ox0 = tx0 - 1;
            *oy0 = ty - 1;
            *ox1 = tx1 + 1;
            *oy1 = y + 1;
            return true;
        }
    }
    return false;
}

// THE TOP SCREEN PANEL ONLY COMES IN THE DS SCREEN'S SHAPE.
//
// The floating top-screen panel is constrained to the DS screen's aspect, so
// it cannot be stretched into a shape that leaves empty space.
//
// The panel used to resize to any shape while the picture was FITTED inside
// it, so every shape but 4:3 was part picture and part bare panel. The
// imgui.ini beside the exe in port/runner/build had saved Size=338,214: at 1x
// that is a 322x179 content region, which the fit filled with a 238.7x179
// picture and 83 empty columns. Even the out-of-the-box size was one row off:
// 228 minus the 19-px title bar and 2 x 8 padding is 193 rows for a 192-row
// picture, so the default panel carried a 1-px strip under it.
//
// So the WINDOW is constrained, because that is where the empty space came
// from. imgui runs this callback on every size it is about to use -- the
// re-check at the top of every Begin, which is also how a size restored from
// imgui.ini reaches it on the first frame, and every drag of a grip or border
// (CalcWindowSizeAfterConstraint in imgui.cpp) -- and it returns a size whose
// content region holds the picture exactly.
//
// WHAT SURROUNDS THE PICTURE, read from the live style so the UI scale slider
// is honoured. ScaleAllSizes truncates every padding and imgui rounds the font
// size, so all of these are whole pixels at every scale:
//   * the title bar, GetFrameHeight(), which is exactly how imgui sizes it
//     (TitleBarHeight = FontSize + 2 * FramePadding.y);
//   * WindowPadding on each side;
//   * Image()'s own border, ImageBorderSize on each side (0 in this style).
// WindowBorderSize is left out on purpose: the border is drawn over the
// padding, and the content origin and extent (CursorStartPos,
// ContentRegionRect) are computed from WindowPadding alone.
//
// WHOLE STEPS, NOT A FREE SCALE. imgui truncates the returned size to whole
// pixels, and at 4:3 only a width that is a multiple of 4 has a whole-pixel
// height. So the size is a count of steps of the picture's shape in lowest
// terms -- 4x3 -- which keeps both sides integral AND the ratio exact. Native
// is 64 steps. (A shape that does not reduce to small terms would get coarse
// steps but keep its ratio; this panel is only ever handed 256x192, because
// the port widens the bottom screen and not the top -- apply_hooks.py 3c2.)
//
// WHICH AXIS DRIVES. A size can follow only one of the mouse's coordinates:
//   * An edge drag follows that edge. imgui sets the resize cursor for the
//     grip or border it is holding immediately before it asks for the
//     constrained size (UpdateWindowManualResize), so the cursor names it.
//     Guessing from which component of the size changed is not good enough:
//     in a corner drag either component equals the current size on any frame
//     the mouse happens to sit on that edge's line, and the frame would be
//     misread as an edge drag of the other axis -- a one-frame jump.
//   * A corner drag puts the corner at the aspect-correct point NEAREST the
//     mouse: the projection of the desired size onto the 4:3 diagonal. It
//     grows and shrinks alike in every direction and depends only on where
//     the mouse is, so it cannot oscillate. "Whichever axis changed more since
//     last frame" does -- modelled, not captured: with the mouse HELD STILL
//     after a mostly-rightward drag it alternated 392x317 / 324x266 every
//     frame. Once a frame follows x, the y the mouse points at can be further
//     from the new height than the mouse moved, so the next frame follows y,
//     and the one after that x again.
//   * No change at all -- the per-frame re-check, and so a restored size --
//     keeps the largest picture that fits inside the size as it stands, and
//     never grows the panel by itself. The 338x214 above comes back as
//     252x212: the picture it already showed, less the empty columns.
//
// THE FLOOR AND THE CAP live here too, in whole steps, because a clamp applied
// to each axis afterwards is exactly what would bend the ratio. The floor is
// the quarter scale the old constraint rectangle enforced (see the note at the
// call), raised if need be to imgui's own WindowMinSize, which imgui applies
// AFTER this callback. The cap is the host window's work area less the same
// 8 px keep_on_screen() and the clamp after Begin shrink to, so neither of
// those has to squeeze the panel off 4:3. Where the two conflict -- a host
// window smaller than the smallest panel -- the floor wins, as it did before.
//
// IT MUST STAY A PURE FUNCTION OF ITS INPUTS. imgui also calls it at the top
// of every Begin for an auto-fit estimate that it discards unless the window
// is auto-fitting (CalcWindowAutoFitSize -> CalcWindowSizeAfterConstraint), so
// any state kept here would be advanced by calls that resize nothing.
struct TopScreenFit {
    int w, h;   // the picture, in guest pixels: 256x192
};

static void top_screen_size_cb(ImGuiSizeCallbackData* data) {
    const TopScreenFit* fit = static_cast<const TopScreenFit*>(data->UserData);
    const ImGuiStyle& style = ImGui::GetStyle();

    // One step is the picture's shape in lowest terms: 256x192 -> 4x3, and
    // native size is `steps` (64) of them.
    int steps = fit->w, rem = fit->h;
    while (rem != 0) {
        const int t = steps % rem;
        steps = rem;
        rem = t;
    }
    const float uw = (float)(fit->w / steps);
    const float uh = (float)(fit->h / steps);

    const float edge_w = 2.0f * (style.WindowPadding.x + style.ImageBorderSize);
    const float edge_h = ImGui::GetFrameHeight() +
                         2.0f * (style.WindowPadding.y + style.ImageBorderSize);

    const ImVec2 want = data->DesiredSize;
    const ImVec2 have = data->CurrentSize;
    const float kx = (want.x - edge_w) / uw;   // the desired size, in steps
    const float ky = (want.y - edge_h) / uh;
    const bool moved_x = std::fabs(want.x - have.x) >= 0.5f;
    const bool moved_y = std::fabs(want.y - have.y) >= 0.5f;

    float k;
    if (!moved_x && !moved_y) {
        // Fit inside, rounding down. A size that is already whole steps
        // divides exactly; the epsilon is belt and braces for the / 3.
        k = std::floor(std::min(kx, ky) + 0.001f);
    } else {
        const float corner =
            (kx * uw * uw + ky * uh * uh) / (uw * uw + uh * uh);
        const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
        if (cursor == ImGuiMouseCursor_ResizeEW)
            k = kx;                     // left or right border
        else if (cursor == ImGuiMouseCursor_ResizeNS)
            k = ky;                     // top or bottom border
        else if (cursor == ImGuiMouseCursor_ResizeNWSE ||
                 cursor == ImGuiMouseCursor_ResizeNESW)
            k = corner;                 // lower-right or lower-left grip
        else
            // Not a mouse drag -- imgui's keyboard window resize, or its
            // per-Begin auto-fit estimate -- so follow whichever component
            // actually changed.
            k = !moved_y ? kx : !moved_x ? ky : corner;
        k = std::floor(k + 0.5f);
    }

    // The cap, skipped while the work area is degenerate: the SDL2 backend
    // reports a minimised window as 0x0 (imgui_impl_sdl2.cpp, NewFrame), and a
    // cap computed from that would shrink the panel to its floor on minimise.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float lim_w = vp->WorkSize.x - 8.0f;
    const float lim_h = vp->WorkSize.y - 8.0f;
    if (lim_w > 0.0f && lim_h > 0.0f) {
        const float kmax = std::floor(std::min((lim_w - edge_w) / uw,
                                               (lim_h - edge_h) / uh));
        if (k > kmax) k = kmax;
    }
    float kmin = std::ceil((float)steps * 0.25f);
    kmin = std::max(kmin, std::ceil((style.WindowMinSize.x - edge_w) / uw));
    kmin = std::max(kmin, std::ceil((style.WindowMinSize.y - edge_h) / uh));
    if (k < kmin) k = kmin;

    data->DesiredSize = ImVec2(k * uw + edge_w, k * uh + edge_h);
}

void draw_top_screen(SDL_Renderer* renderer) {
    if (!g_top_visible || !g_top_pixels || g_top_w <= 0 || g_top_h <= 0) return;
    // DS Vanilla shows the top screen above the bottom one already.
    if (!g_set.enhanced) return;

    if (!g_top_tex || g_top_tex_w != g_top_w || g_top_tex_h != g_top_h) {
        if (g_top_tex) SDL_DestroyTexture(g_top_tex);
        g_top_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      g_top_w, g_top_h);
        g_top_tex_w = g_top_w;
        g_top_tex_h = g_top_h;
        if (!g_top_tex) return;
        SDL_SetTextureBlendMode(g_top_tex, SDL_BLENDMODE_NONE);
    }
    // ERASE THE CARTRIDGE'S OWN CAMERA BOX.
    //
    // The game draws a 16x18 white indicator for the 256x192 it can show. With
    // the adaptive surface covering most of the battlefield that box is simply
    // wrong, and the corrected one is drawn over the panel below -- so the
    // player was left looking at two boxes: the yellow outline of the expanded
    // view did not hide the original white one.
    //
    // INTO A PRIVATE COPY, NOT THE FRAMEBUFFER. g_top_pixels is a pointer to
    // the frontend's own buffer, and lb_field's shroud readback samples that
    // same top screen every frame -- it classifies (251,251,251) as explored
    // ground. Scrubbing in place would rewrite the input of an unrelated
    // measurement. The copy costs one 192 KB memcpy, and only while the panel
    // is open.
    //
    // Both the colour and the geometry are measured, not chosen. docs/180 read
    // the outline as (251,251,251), 64 px, and fitted the map->minimap
    // transform against that same indicator; 256*ax = 15.3 and 192*ay = 17.5
    // predicted its 16.0 x 17.7 size, which is the check that the rect below
    // lands where the box actually is.
    const void* top_src = g_top_pixels;
    // NOT WHILE THE CARTRIDGE DRAWS THE WIDENED BOX ITSELF (docs/370). Its box
    // is then the right one: shown as drawn, neither scrubbed (the finder
    // would take a 13-25 px tall box for the old one) nor overdrawn (the
    // corrected box below needs g_top_box_found).
    if (lb_minimap_box_live()) g_top_box_found = false;
    if (g_top_w == 256 && g_top_h == 192 && !lb_minimap_box_live()) {
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
        bool located =
            lb_find_cartridge_box((const uint32_t*)g_top_pixels, &x0, &y0,
                                  &x1, &y1);
        // LB_MMTRACE=1: the found box beside the rectangle the OLD code would
        // have predicted, per frame, so the old miss rate is measured on the
        // same frames rather than argued.
        {
            static const bool mm_trace = std::getenv("LB_MMTRACE") != nullptr;
            if (mm_trace) {
                int pcx = 0, pcy = 0, px0 = -1, py0 = -1, px1 = -1, py1 = -1;
                const int pvalid = lb_widemap_cam_px(&pcx, &pcy);
                if (pvalid) {
                    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
                    lb_field_map_to_minimap(pcx, pcy, &ax, &ay);
                    lb_field_map_to_minimap(pcx + 256, pcy + 192, &bx, &by);
                    px0 = (int)ax - 2; py0 = (int)ay - 2;
                    px1 = (int)bx + 2; py1 = (int)by + 2;
                }
                // Did the old rectangle contain the whole found box?
                const int covers = located && pvalid && px0 <= x0 + 1 &&
                                   py0 <= y0 + 1 && px1 >= x1 - 1 &&
                                   py1 >= y1 - 1;
                std::fprintf(stderr,
                    "[lb_mm] frame %u found %d box %d,%d..%d,%d  old %d "
                    "rect %d,%d..%d,%d  old_covers %d\n",
                    lb_dump_frame_get(), located ? 1 : 0, x0, y0, x1, y1,
                    pvalid, px0, py0, px1, py1, covers);
            }
        }
        // IS IT THE CAMERA BOX, OR JUST SOMETHING WHITE AND SQUARE?
        //
        // docs/347 item 2: the custom minimap-width white square showed even
        // while the minimap was not visible on the top screen, e.g. while a
        // unit or building was selected. The unit page has
        // white-outlined elements (a white frame around a portrait slot), so
        // "a white box was found" is not "the minimap is up". The cartridge's
        // box sits where the camera says; measured over 5,018 minimap frames,
        // its centre was within 5 px of the camera's prediction every time and
        // within 2 px on 97.6%. A white frame on the unit page sits at a fixed
        // UI position. So a found box is accepted only within 8 px of the
        // prediction -- or, on a frame with no valid camera reading, only as a
        // continuation of last frame's accepted box.
        //
        // And NO FALLBACK when nothing is found: the old code then scrubbed
        // the white pixels wherever the camera said, which on the unit page
        // meant scrubbing the page, and reported the minimap as up.
        static bool prev_ok = false;
        static int prev_cx2 = 0, prev_cy2 = 0;   // doubled centre
        if (located) {
            const int fcx2 = x0 + x1, fcy2 = y0 + y1;
            int pcx = 0, pcy = 0;
            if (lb_widemap_cam_px(&pcx, &pcy)) {
                float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
                lb_field_map_to_minimap(pcx, pcy, &ax, &ay);
                lb_field_map_to_minimap(pcx + 256, pcy + 192, &bx, &by);
                const int dx = std::abs(fcx2 - (int)(ax + bx));
                const int dy = std::abs(fcy2 - (int)(ay + by));
                located = dx <= 16 && dy <= 16;   // 8 px, in doubled units
            } else {
                located = prev_ok && std::abs(fcx2 - prev_cx2) <= 4 &&
                          std::abs(fcy2 - prev_cy2) <= 4;
            }
            if (located) {
                prev_cx2 = fcx2;
                prev_cy2 = fcy2;
            }
        }
        prev_ok = located;
        g_top_box_found = false;   // set below only when a box is erased
        if (located) {
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > 255) x1 = 255;
            if (y1 > 191) y1 = 191;
            if (x1 > x0 && y1 > y0) {
                g_top_scrub.assign((const uint32_t*)g_top_pixels,
                                   (const uint32_t*)g_top_pixels + 256 * 192);
                // The outline only. Terrain tops out at (130,162,113) in the
                // sampled palette, so "every channel >= 200 and near-grey" can
                // only be the indicator.
                auto is_box = [](uint32_t px) {
                    const int r = (int)((px >> 16) & 0xFF);
                    const int g = (int)((px >> 8) & 0xFF);
                    const int b = (int)(px & 0xFF);
                    return r >= 200 && g >= 200 && b >= 200;
                };
                const uint32_t* src = (const uint32_t*)g_top_pixels;
                int erased = 0;
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        if (!is_box(src[y * 256 + x])) continue;
                        ++erased;
                        // Average the neighbours that are NOT the outline. The
                        // box is one pixel wide, so a 3x3 window around any of
                        // its pixels is mostly minimap, and the result reads as
                        // the panel closing over it rather than as a hole.
                        unsigned rs = 0, gs = 0, bs = 0, n = 0;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx < 0 || nx > 255 || ny < 0 || ny > 191)
                                    continue;
                                const uint32_t q = src[ny * 256 + nx];
                                if (is_box(q)) continue;
                                rs += (q >> 16) & 0xFF;
                                gs += (q >> 8) & 0xFF;
                                bs += q & 0xFF;
                                ++n;
                            }
                        }
                        if (n) {
                            g_top_scrub[y * 256 + x] =
                                0xFF000000u | ((rs / n) << 16) |
                                ((gs / n) << 8) | (bs / n);
                        }
                    }
                }
                top_src = g_top_scrub.data();
                // THE CARTRIDGE'S BOX IS THE PROOF THE MINIMAP IS UP.
                //
                // A 16x18 outline is ~64 px, so 8 is a generous floor that
                // still cannot be reached by stray bright pixels.
                g_top_box_found = erased >= 8;
            }
        }
    }
    SDL_UpdateTexture(g_top_tex, nullptr, top_src,
                      g_top_w * (int)sizeof(uint32_t));

    // Lower-right on first run, by design. Movable and
    // resizable after that, and ImGui remembers it for the session.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = (float)g_top_w, h = (float)g_top_h;

    // Default to native size, but never wider than 40% of the window nor
    // taller than half of it: at native 256x192 this is comfortable on a
    // 896x384 bottom screen and still sane if the player shrinks the window.
    float s = 1.0f;
    const float max_w = vp->WorkSize.x * 0.40f;
    const float max_h = vp->WorkSize.y * 0.50f;
    if (w + 16.0f > max_w) s = max_w / (w + 16.0f);
    if ((h + 36.0f) * s > max_h) s = max_h / (h + 36.0f);
    const float pw = (w + 16.0f) * s, ph = (h + 36.0f) * s;

    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - pw - 24.0f,
               vp->WorkPos.y + vp->WorkSize.y - ph - 24.0f),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_FirstUseEver);

    // A FLOOR, because a degenerate saved size is permanent otherwise.
    //
    // The clamp further down only ever SHRINKS the panel, so nothing could
    // rescue one that had been saved too small. Found in this port's own
    // imgui.ini: `[Window][Top Screen] Size=272,32`. 32 px is less than the
    // title bar plus padding, so the content region came out ~0 tall, the
    // picture fitted into it went to ~0, and the panel rendered as a blank
    // strip -- for the whole session, and for every session after it, since
    // imgui writes the size straight back out again. That is what the
    // top-screen panel had been in every capture until this was fixed.
    //
    // Quarter scale rather than the full default: a player who deliberately
    // made it small keeps a small panel, they just cannot make it a panel
    // that shows nothing. imgui applies constraints to the restored size as
    // well as to dragging, so an already-broken ini self-heals on load.
    //
    // AND NOW A SHAPE. That floor used to be this call's minimum rectangle,
    // (w/4 + 16, h/4 + 36). A rectangle bounds each axis on its own, so above
    // the floor the panel could still be dragged into any shape at all -- the
    // empty space. The floor and the 4:3 ratio are both applied by
    // top_screen_size_cb, in whole steps of the picture's shape and from the
    // live style rather than the 16/36 estimated here. The rectangle is left
    // wide open so the callback sees each size exactly as imgui proposes it.
    //
    // `top_fit` is read during Begin() and not after it (imgui clears the
    // next-window data inside Begin), so a local is enough.
    TopScreenFit top_fit = {g_top_w, g_top_h};
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(FLT_MAX, FLT_MAX),
                                        top_screen_size_cb, &top_fit);

    // NO SCROLLBAR, because the picture IS the content region and a scrollbar
    // could only take width away from it. imgui decides the bar from LAST
    // frame's content against THIS frame's padding (Begin, "SCROLLBAR
    // VISIBILITY" in imgui.cpp), so on the frame the UI scale slider grows the
    // padding, last frame's picture plus the new padding reads as overflow: a
    // vertical bar would take ScrollbarSize columns for that frame and the
    // picture would be drawn squeezed. Nothing in this panel ever scrolls.
    if (ImGui::Begin("Top Screen", &g_top_visible,
                     ImGuiWindowFlags_NoScrollbar)) {
        keep_on_screen();
        // KEEP IT ON SCREEN WHEN THE WINDOW SHRINKS.
        //
        // The placement above is FirstUseEver, so it is chosen once and ImGui
        // then remembers it for the session. Resize the window smaller and the
        // panel stays at its old coordinates, hanging off the bottom-right --
        // at 640x480 it ends up half outside with its contents unreachable.
        //
        // Clamping here rather than forcing the position every frame keeps the
        // panel where the player dragged it: it only moves when it would
        // otherwise be off the edge, and it only shrinks when it no longer
        // fits at all.
        const ImVec2 cur_pos = ImGui::GetWindowPos();
        const ImVec2 cur_size = ImGui::GetWindowSize();
        const float lim_w = vp->WorkSize.x - 8.0f;
        const float lim_h = vp->WorkSize.y - 8.0f;
        if (cur_size.x > lim_w || cur_size.y > lim_h) {
            const float fit = std::min(lim_w / cur_size.x, lim_h / cur_size.y);
            ImGui::SetWindowSize(ImVec2(cur_size.x * fit, cur_size.y * fit));
        }
        const ImVec2 size_now = ImGui::GetWindowSize();
        float px = cur_pos.x, py = cur_pos.y;
        const float max_x = vp->WorkPos.x + vp->WorkSize.x - size_now.x - 4.0f;
        const float max_y = vp->WorkPos.y + vp->WorkSize.y - size_now.y - 4.0f;
        if (px > max_x) px = max_x;
        if (py > max_y) py = max_y;
        if (px < vp->WorkPos.x + 4.0f) px = vp->WorkPos.x + 4.0f;
        if (py < vp->WorkPos.y + 4.0f) py = vp->WorkPos.y + 4.0f;
        if (px != cur_pos.x || py != cur_pos.y)
            ImGui::SetWindowPos(ImVec2(px, py));

        // THE PICTURE IS THE CONTENT REGION, not a fit inside it.
        //
        // It used to be scaled by min(avail.x / w, avail.y / h) and drawn at
        // that size, so whatever the other axis had left over was bare panel.
        // top_screen_size_cb now makes the region itself whole 4x3 steps, so
        // the fit and the region are the same rectangle -- and drawing AT the
        // region, rather than at a fit recomputed from it, means no float
        // rounding can open a 1-px strip along either edge. Image() adds
        // ImageBorderSize on each side of the size it is given (ImageWithBg in
        // imgui_widgets.cpp), so that comes off first; it is 0 in this style.
        //
        // The camera box below maps through the same two factors, so it sits
        // on the picture exactly as drawn.
        const float ib = ImGui::GetStyle().ImageBorderSize;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 pic(avail.x - 2.0f * ib, avail.y - 2.0f * ib);
        const float sx = pic.x / w, sy = pic.y / h;
        ImGui::Image((ImTextureID)(intptr_t)g_top_tex, pic);

        // SG-0006: a unit queued in a building can be cancelled by clicking
        // its icon. The game draws the queue here, on the top screen, with
        // three buttons (rows 148..172; columns 32..56, 64..88, 96..120) whose
        // handler sends its own cancel command -- but the top screen has no
        // touch, so no click ever reaches them. A click on one asks the game to
        // run that handler (lb_patches.cpp, kQuickCancel); it declines unless
        // the game itself would take the click, so an empty slot does nothing.
        if (g_set.cancel_queue_click && ImGui::IsItemHovered() && sx > 0.0f &&
            sy > 0.0f) {
            const unsigned slots = lb_cancel_slots();
            const ImVec2 i0 = ImGui::GetItemRectMin();
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const float u = (mp.x - i0.x - ib) / sx;
            const float v = (mp.y - i0.y - ib) / sy;
            int hit = -1;
            if (v >= 148.0f && v < 172.0f)
                for (int s = 0; s < 3; ++s)
                    if (u >= 32.0f + 32.0f * s && u < 56.0f + 32.0f * s)
                        hit = s;
            if (hit >= 0 && ((slots >> hit) & 1u)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Click to cancel this unit\nand get its bricks back");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                    ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                    lb_quick_request(40 + hit);
            }
        }

        // The corrected camera box. The cartridge draws its own indicator sized
        // to the 256x192 it can show; once the window is covering a 21:9
        // surface with most of the battlefield, that box is simply wrong -- it
        // marks a small square inside an area the player is already looking at.
        //
        // Drawn over the panel rather than into the guest's framebuffer: the
        // top screen is the game's own output and the port does not repaint it.
        // (lb_field's shroud readback SAMPLES that framebuffer's minimap, so
        // painting a box into it would be read back as terrain.)
        //
        // IT USED TO REQUIRE g_wide_field, AND THAT IS WHY NOBODY EVER SAW IT.
        //
        // g_wide_field is the whole-battlefield panel -- a separate host-side
        // renderer that is OFF in ordinary play. So in a normal enhanced
        // session, which is the only mode the adaptive surface runs in, the
        // condition was false every frame and the only indicator on screen was
        // the cartridge's fixed one. The corrected box existed, was measured,
        // was documented in docs/180, and was unreachable.
        //
        // The adaptive surface knows exactly what it covers without the wide
        // field being up: the guest's own view starts at the camera, the band
        // says how far left and up of it the surface reaches, and the surface's
        // own size says how much ground that is. Same rectangle, from the
        // renderer that is actually running.
        float vx0 = g_view_x0, vy0 = g_view_y0;
        float vx1 = g_view_x1, vy1 = g_view_y1;
        bool have_rect = g_wide_field && g_view_valid;
        if (!have_rect) {
            const int ow = (int)nds_gpu3d_output_width();
            const int oh = (int)nds_gpu3d_output_height();
            int cx = 0, cy = 0;
            if (ow > 256 && lb_widemap_cam_px(&cx, &cy)) {
                const float x0 = (float)(cx - lb_band_left(ow));
                const float y0 = (float)(cy - lb_band_top(oh));
                vx0 = x0;
                vy0 = y0;
                vx1 = x0 + (float)ow;
                vy1 = y0 + (float)oh;
                have_rect = true;
            }
        }
        // ONLY WHILE THE MINIMAP IS ACTUALLY ON THE TOP SCREEN.
        //
        // The report was "draws even while a unit is selected which hides the
        // minimap", and the first attempt at this gated on "is anything
        // selected" via the in-field HUD quad count. That rule is WRONG, and a
        // capture said so: selecting the player's BASE puts 6 HUD quads on
        // screen while the top screen still shows the minimap, so the box would
        // have been hidden in a state where it is perfectly valid. "Selected"
        // and "the minimap is gone" are not the same condition.
        //
        // The cartridge's own camera box is the honest signal, and it costs
        // nothing extra because the scrub above is already looking for it: the
        // game draws that box only as part of the minimap, so finding it proves
        // the minimap is up AND says where the camera is. When a unit panel
        // replaces the minimap there is no box to find, and the corrected one
        // stays away with it.
        if (have_rect && !g_top_box_found) have_rect = false;
        if (have_rect) {
            // The picture's own corner, inside Image()'s border.
            const ImVec2 item0 = ImGui::GetItemRectMin();
            const ImVec2 img0(item0.x + ib, item0.y + ib);
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            lb_field_map_to_minimap((int)vx0, (int)vy0, &ax, &ay);
            lb_field_map_to_minimap((int)vx1, (int)vy1, &bx, &by);
            // WHITE, to match the cartridge's own indicator.
            //
            // It was IM_COL32(255, 240, 64, 255) -- yellow -- from when this
            // box and the debug aspect outline both needed to be told apart
            // from the game's. It is not a debug aid any more, it is the
            // player's view indicator, so it looks like the one it replaces:
            // white, to match the vanilla one.
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(img0.x + ax * sx, img0.y + ay * sy),
                ImVec2(img0.x + bx * sx, img0.y + by * sy),
                IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
        }
    }
    ImGui::End();
}

// -- the whole-battlefield view ---------------------------------------------
//
// docs/167: the emulated 2D path can never show more field than the DS does,
// because BG1/BG2 are a scroll double-buffer and only one screen's worth of
// terrain is ever in VRAM. docs/168/172: the level grid describes the WHOLE
// map, so lb_field draws it host-side with no DS limit at all.
//
// This is the consumer. It also meets the requirement that the camera box
// show what the window actually covers: the rectangle is computed
// from the located camera cell and the guest viewport, not from a fixed 16x18
// box like the cartridge's own minimap.
LbFieldInfo g_field_info;
// The located map chain, cached for lb_field_map_chain(). Deliberately NOT
// g_field_located, which churns -- see the note on that accessor.
uint32_t g_chain_map = 0;
int g_chain_w = 0, g_chain_h = 0;
bool g_field_located = false;
SDL_Texture* g_field_tex = nullptr;
int g_field_tex_w = 0, g_field_tex_h = 0;
// Frames until the whole-level texture is decoded again. 0 forces it now.
int g_field_redraw = 0;
std::vector<uint32_t> g_field_px;
// LB_FIELDPANEL=1 opens the battlefield panel at startup. The panel's code
// only runs when it is drawn, so without this a headless run proves nothing
// about it either way.
bool g_field_visible = std::getenv("LB_FIELDPANEL") != nullptr;

// -- the wide field, drawn as the WINDOW's content --------------------------
//
// This is the actual goal rather than a panel beside it: the whole
// battlefield filling a 1920x1080 or 21:9 ultrawide window. The frontend blits the
// guest's 256x192 screen first and the overlay draws after it, so covering the
// window here replaces the stock view.
//
// Terrain is STATIC for a level, so it is rendered once and kept; only the
// guest's live output and the learned unit sprites are composited per frame.
// Re-rendering 1.5M pixels of terrain every frame would be pure waste.
std::vector<uint32_t> g_wide_terrain;    // rendered once per level
std::vector<uint32_t> g_wide_frame;      // terrain + guest + sprites, per frame
SDL_Texture* g_wide_tex = nullptr;
int g_wide_w = 0, g_wide_h = 0;
bool g_wide_field = false;

void draw_wide_field(SDL_Renderer* renderer) {
    // LB_WIDE_FIELD=1 starts in wide mode without having to tick the box, the
    // same way LB_BALANCE and LB_CHEATS exist.
    static int forced = -1;
    if (forced < 0) {
        forced = std::getenv("LB_WIDE_FIELD") ? 1 : 0;
        if (forced) g_wide_field = true;
    }
    if (!g_wide_field || !renderer) return;
    if (!g_field_located) {
        // RATE-LIMITED, and it matters. lb_field_locate() copies and scans 4 MB
        // of main RAM; it fails whenever there is no level up, which is every
        // frame the player spends in a menu. Retrying it per frame would make
        // simply having this option enabled turn the front end to treacle -- a
        // bug the player would meet before ever seeing the feature work.
        static int retry = 0;
        if (--retry > 0) return;
        retry = 90;
        g_field_located = lb_field_locate(&g_field_info);
        if (!g_field_located) return;
        g_wide_terrain.clear();
    }
    // A level change moves the tables and the grid, so a located level has to
    // be re-checked or the map would be drawn from stale addresses.
    if ((ImGui::GetFrameCount() % 60) == 0 &&
        !lb_field_still_valid(&g_field_info)) {
        g_field_located = false;
        g_wide_terrain.clear();
        return;
    }

    int mw = 0, mh = 0;
    lb_field_size(&g_field_info, &mw, &mh);
    if (mw <= 0 || mh <= 0) { g_field_located = false; return; }

    // The base: terrain plus shroud. Neither changes per frame -- terrain never
    // does, and the shroud only when a unit reveals new ground -- so rebuilding
    // it every frame was pure waste. It cost a 6 MB memcpy, up to 1.5M shroud
    // writes and a 6 MB texture upload EVERY FRAME, which is where the frame
    // rate went.
    static int base_gen = -1;
    lb_field_refresh_shroud(&g_field_info);
    const int gen = lb_field_shroud_generation();
    bool rebuilt = false;
    if (g_wide_terrain.size() != (size_t)mw * (size_t)mh) {
        g_wide_terrain.assign((size_t)mw * (size_t)mh, 0u);
        // WHOLE MAP HERE. The window is a global and lb_hostterrain_prepare
        // sets it to the camera rect every frame; this panel draws the
        // entire map, so it has to clear it or it would inherit a window
        // and render a strip.
        lb_field_set_window(0, 0, 0, 0);
        lb_field_render(&g_field_info, g_wide_terrain.data());
        g_wide_frame.assign((size_t)mw * (size_t)mh, 0u);
        base_gen = -1;
    }
    if (base_gen != gen) {
        base_gen = gen;
        lb_field_apply_shroud(&g_field_info, g_wide_terrain.data());
        std::memcpy(g_wide_frame.data(), g_wide_terrain.data(),
                    g_wide_frame.size() * sizeof(uint32_t));
        rebuilt = true;
    }

    if (!g_wide_tex || g_wide_w != mw || g_wide_h != mh) {
        if (g_wide_tex) SDL_DestroyTexture(g_wide_tex);
        // STATIC, not STREAMING.
        //
        // A streaming texture is internally double-buffered: SDL_UpdateTexture
        // with a rect writes one buffer, the next frame presents the other, and
        // everything outside the updated rect is whatever that buffer last held.
        // With partial updates that renders the map as vertical bands -- the
        // uploaded rectangles in terrain, everything else black -- which is
        // exactly what shipped. STATIC is the access mode for a texture updated
        // by rect and keeps the untouched pixels.
        g_wide_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, mw, mh);
        g_wide_w = mw; g_wide_h = mh;
        if (!g_wide_tex) return;
        SDL_SetTextureBlendMode(g_wide_tex, SDL_BLENDMODE_NONE);
        rebuilt = true;
    }

    // Only the camera rectangle and the entities move, so only those are
    // restored from the base, recomposited and uploaded. Last frame's rectangles
    // are restored too, or a unit would smear a trail across the map.
    struct Rect { int x, y, w, h; };
    static std::vector<Rect> prev_dirty;
    std::vector<Rect> dirty;
    const int kPad = 64;                    // covers the learned-sprite patch
    int dcx = 0, dcy = 0;
    if (lb_field_camera(&dcx, &dcy))
        dirty.push_back({dcx - 2, dcy - 2, 256 + 4, 192 + 4});
    int ow = 0, oc = 0, ex = 0, ey = 0;
    for (int i = 0; i < 64 && lb_field_entity_at(i, &ow, &oc, &ex, &ey); ++i)
        dirty.push_back({ex - kPad / 2, ey - kPad / 2, kPad, kPad});

    auto clamp_rect = [&](Rect r) {
        if (r.x < 0) { r.w += r.x; r.x = 0; }
        if (r.y < 0) { r.h += r.y; r.y = 0; }
        if (r.x + r.w > mw) r.w = mw - r.x;
        if (r.y + r.h > mh) r.h = mh - r.y;
        return r;
    };
    if (!rebuilt) {
        for (Rect r : prev_dirty) {
            r = clamp_rect(r);
            for (int y = 0; y < r.h; ++y)
                std::memcpy(&g_wide_frame[(size_t)(r.y + y) * mw + r.x],
                            &g_wide_terrain[(size_t)(r.y + y) * mw + r.x],
                            (size_t)r.w * sizeof(uint32_t));
        }
    }
    lb_field_composite_guest(&g_field_info, g_wide_frame.data());

    // A full refresh every half second regardless. Partial texture updates are
    // backend-dependent, and the last attempt shipped a map drawn as vertical
    // bands because one backend did not keep the untouched pixels. A periodic
    // full upload costs little at this rate and means a backend that behaves
    // differently degrades to a visible seam for a few frames rather than to an
    // unusable picture.
    static int full_refresh = 0;
    if (--full_refresh <= 0) {
        full_refresh = 30;
        rebuilt = true;
    }

    if (rebuilt) {
        SDL_UpdateTexture(g_wide_tex, nullptr, g_wide_frame.data(),
                          mw * (int)sizeof(uint32_t));
    } else {
        for (Rect r : prev_dirty) dirty.push_back(r);
        for (Rect r : dirty) {
            r = clamp_rect(r);
            if (r.w <= 0 || r.h <= 0) continue;
            const SDL_Rect sr{r.x, r.y, r.w, r.h};
            SDL_UpdateTexture(g_wide_tex, &sr,
                              &g_wide_frame[(size_t)r.y * mw + r.x],
                              mw * (int)sizeof(uint32_t));
        }
    }
    prev_dirty.clear();
    for (Rect r : dirty) prev_dirty.push_back(r);

    // Keep the map's aspect -- never stretch -- but choose WHICH axis binds.
    //
    // Fitting the whole map inside the window (the smaller scale) letterboxes,
    // and the battlefield is 1536x1008, about 3:2. In a 21:9 window that leaves
    // very wide pillars, and those are the black bars at issue. The goal is
    // the whole battlefield OR AS MUCH OF IT AS FITS SCALEWISE, which is the
    // larger scale: cover the window and crop the overflow. There
    // are no bars because the picture is always at least as large as the window.
    //
    // The camera-follow below already clamps whichever axis overflows, so the
    // crop tracks what the player is looking at rather than the map's centre.
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // The scale comes from a VIEW REGION, not from the whole map.
    //
    // Deriving it from the map meant the picture was always exactly as big as
    // the window, so there was nothing to scroll: at a 1600x885 window against a
    // 1536x1008 map the cover scale is 1.04 and the map fills horizontally to
    // within four percent. The camera moved, the units moved, and the map could
    // not -- which is precisely the fault: a static picture of the map with the
    // unit layer panning over it. It was not a bug in the panning code; there
    // was no room to pan.
    //
    // `zoom` is now how many DS SCREENS WIDE the player sees. At 3 the view is
    // 768x576 map pixels -- three times the field of view the cartridge gives,
    // still far smaller than the 1536x1008 map, so both axes have room to move
    // and the view tracks the camera the way a scrolling RTS view should.
    // LB_WIDE_SPAN and LB_WIDE_FIT, beside the LB_WIDE_FIELD that turns this
    // whole path on. These were Settings fields with no widget and no accessor,
    // which made them look like options in settings.json while being reachable
    // only by hand-editing it; env vars say plainly that they belong to a
    // diagnostic.
    static float span = -1.0f;
    static bool fit = false;
    if (span < 0.0f) {
        const char* sv = std::getenv("LB_WIDE_SPAN");
        span = sv ? (float)std::atof(sv) : 3.0f;
        if (span < 0.25f || span > 8.0f) span = 3.0f;
        fit = std::getenv("LB_WIDE_FIT") != nullptr;
    }
    const float view_w = 256.0f * span, view_h = 192.0f * span;
    const float kx = vp->WorkSize.x / view_w, ky = vp->WorkSize.y / view_h;
    float k = !fit ? (kx > ky ? kx : ky) : (kx < ky ? kx : ky);

    // Never smaller than covering the whole map, or a large span would shrink
    // the picture below the window and put the black bars back.
    const float k_cover_map = (vp->WorkSize.x / (float)mw > vp->WorkSize.y / (float)mh)
                                  ? vp->WorkSize.x / (float)mw
                                  : vp->WorkSize.y / (float)mh;
    if (!fit && k < k_cover_map) k = k_cover_map;
    const float w = mw * k, h = mh * k;
    int camx = 0, camy = 0;
    const bool have_cam = lb_field_camera(&camx, &camy);
    float cxp = vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f;
    float cyp = vp->WorkPos.y + (vp->WorkSize.y - h) * 0.5f;
    if (have_cam && w > vp->WorkSize.x) {
        cxp = vp->WorkPos.x + vp->WorkSize.x * 0.5f - (camx + 128) * k;
        const float lo = vp->WorkPos.x + vp->WorkSize.x - w, hi = vp->WorkPos.x;
        if (cxp < lo) cxp = lo;
        if (cxp > hi) cxp = hi;
    }
    if (have_cam && h > vp->WorkSize.y) {
        cyp = vp->WorkPos.y + vp->WorkSize.y * 0.5f - (camy + 96) * k;
        const float lo = vp->WorkPos.y + vp->WorkSize.y - h, hi = vp->WorkPos.y;
        if (cyp < lo) cyp = lo;
        if (cyp > hi) cyp = hi;
    }
    const ImVec2 p0(cxp, cyp);

    // What the window actually covers, in map pixels. The minimap's camera
    // square should match the window's aspect and what it covers, and
    // the cartridge's own indicator cannot: it is drawn for a 256x192 view, so
    // in an ultrawide window showing most of the battlefield it reports a small
    // box in the middle of ground the player can already see.
    g_view_x0 = (vp->WorkPos.x - p0.x) / k;
    g_view_y0 = (vp->WorkPos.y - p0.y) / k;
    g_view_x1 = g_view_x0 + vp->WorkSize.x / k;
    g_view_y1 = g_view_y0 + vp->WorkSize.y / k;
    if (g_view_x0 < 0.0f) g_view_x0 = 0.0f;
    if (g_view_y0 < 0.0f) g_view_y0 = 0.0f;
    if (g_view_x1 > (float)mw) g_view_x1 = (float)mw;
    if (g_view_y1 > (float)mh) g_view_y1 = (float)mh;
    g_view_valid = true;

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(vp->WorkPos,
                      ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                             vp->WorkPos.y + vp->WorkSize.y),
                      IM_COL32(16, 16, 20, 255));
    bg->AddImage((ImTextureID)(intptr_t)g_wide_tex, p0,
                 ImVec2(p0.x + w, p0.y + h));
}

// LB_MAPDUMP, LIFTED OUT OF THE BATTLEFIELD PANEL (docs/291).
//
// This dump used to live inside draw_battlefield, after its own AddRect, which
// made it need the panel OPEN -- and opening the panel makes ImGui take the
// mouse, so lb_overlay_wants_mouse() suppresses guest touch delivery and the
// harness route never reaches a battle. Measured: with LB_FIELDPANEL=1 the run
// produced zero dumps of any kind and logged `map=0x0px geom=0` with
// `peak 0/2048 polygons`. The audit needed a battle, reaching a battle needed
// the panel shut, and the dump needed it open.
//
// Nothing here actually wanted ImGui. mw/mh come from lb_field_size, the view
// rect from lb_field_camera_rect and lb_overlay_field_rect, and the pixels from
// lb_field_render -- all plain calls. So it runs per frame from
// lb_overlay_draw instead, with the panel shut and the route intact, and
// tools/lb_bandaudit.py can pair lb_map with lb_comp and lb_layers again.
//
// COST WHEN OFF IS ONE getenv, CACHED, AND ONE INTEGER COMPARE. The expensive
// parts -- lb_field_locate scanning main RAM, and lb_field_render decoding the
// whole level -- happen only on a frame that is actually dumping, which is one
// frame in LB_MAPDUMP.
// PHASE 2 OF docs/325: THE FIELD'S BASE LAYER, DRAWN FROM THE LEVEL GRID.
//
// Phase 1 measured the cost and it is 1.256 ms for the WHOLE map per frame, so
// there is no caching to design and this can simply run.
//
// STAGED DELIBERATELY. This supplies the compositor's BASE pixel -- what it
// would otherwise take from the palette backdrop -- and does NOT yet suppress
// BG1. Inside the guest's own 256 columns BG1 still draws over it, so the
// native picture is untouched and only the bands change. That makes the first
// step verifiable against the current build by screenshot: identical in the
// middle, real terrain at the sides. Suppressing BG1 is the next step, once
// this one is seen to be right.
//
// LB_HOSTTERRAIN=1. Off by default: it is a render path change.
bool g_ht_ready = false;
// Set when the shroud applied to the host field came from the located map
// record, which is the precondition for letting lb_widemap stop
// reconstructing fog for the band columns.
bool g_ht_fogged = false;
int g_ht_w = 0, g_ht_h = 0, g_ht_cx = 0, g_ht_cy = 0;
unsigned g_ht_band_seen = 0;      // band pixels where the host had terrain
unsigned g_ht_band_kept = 0;      // ...and the final pixel still was it
int g_ht_note_row = -1;           // which output row those two describe
unsigned g_ht_native_seen = 0;    // same inside the guest's own 256 columns
unsigned g_ht_native_kept = 0;
unsigned g_ht_calls = 0, g_ht_hits = 0;
unsigned g_ht_rej_notready = 0, g_ht_rej_range = 0;
int g_ht_first_dy = 0;   // dy of the first row asked for that was off the map

extern "C" int lb_hostterrain_prepare(void) {
    static int on = -1;
    if (on < 0) {
        // OFF BY DEFAULT since 2026-09-20. It was on from docs/325 phase 2,
        // "verified by a composited frame with terrain continuous through both
        // bands and no shroud wall at the native boundary" -- and that
        // verification was on a level where lb_field_locate SUCCEEDED. On
        // another level locate failed until 5263b07b, so this path had never
        // actually run there, and the day it did a screenshot showed a
        // battlefield that is 96% shroud hatch at 10.6 FPS with the crystal
        // fields tiled wrong. docs/334.
        //
        // THREE SEPARATE FAULTS, ALL DOWNSTREAM OF THIS SWITCH:
        //
        //   * the shroud. 9af03fa2 sized the mask from the map instead of a
        //     constant 64x63, which is correct, and it means the game's own
        //     record -- 8,856 of 9,216 cells hidden -- is applied to the whole
        //     map for the first time. Faithful, and a grey wall.
        //   * the metatiles. 5263b07b relocated the tables by pointer chain
        //     (split at 0214B9D0+2, secondary at map+0x33C). A wrong split
        //     resolves every index above it to the wrong entry, which draws as
        //     one tile repeated across a region -- the crystal fields.
        //   * the cost. A whole-map render and shroud pass, measured at
        //     2.587 ms/frame whole-map and 0.430 ms clipped on that route.
        //
        // And it buys almost nothing even when it works: measured contributing
        // ONE band pixel in 672 on the middle output row, because lb_widemap's
        // band synthesis overdraws it. The port was paying for two terrain
        // renderers and showing one.
        //
        // LB_HOSTTERRAIN=1 turns it back on for whoever fixes the above.
        const char* v = std::getenv("LB_HOSTTERRAIN");
        on = (v && v[0] == '1') ? 1 : 0;
    }
    g_ht_ready = false;
    if (!on) return 0;
    if (!g_field_located) {
        // RATE-LIMITED, and the first version of this was not: lb_field_locate
        // copies and scans 4 MB of main RAM, and it FAILS on every frame the
        // player is not in a battle. Retrying per frame measured **29.3 fps
        // against 57** -- the whole cost of enabling this was paid in menus,
        // where it cannot even do anything. The sibling panel above carries
        // the same rate limit and the same warning; I read it after writing
        // the bug.
        static int retry = 0;
        if (--retry > 0) return 0;
        retry = 90;
        g_field_located = lb_field_locate(&g_field_info);
        if (!g_field_located) return 0;
        // Cache the chain for lb_field_map_chain(), which lb_widemap_fill
        // consults from the vblank hook where the map pointer reads 0.
        g_chain_map = g_field_info.map_obj;
        g_chain_w = g_field_info.grid_w;
        g_chain_h = g_field_info.grid_h;
    }
    lb_field_size(&g_field_info, &g_ht_w, &g_ht_h);
    if (g_ht_w <= 0 || g_ht_h <= 0) { g_field_located = false; return 0; }
    // The camera the same way the map dump refreshes it: the game's own state
    // (docs/174), which keeps working over unexplored ground where terrain
    // matching has nothing to match.
    int cam_dummy = 0;
    (void)(lb_field_camera(&cam_dummy, &cam_dummy) ||
           lb_field_track_camera(&g_field_info));
    g_field_px.resize((size_t)g_ht_w * (size_t)g_ht_h);

    // ONLY THE WINDOW THE COMPOSITOR SAMPLES.
    //
    // lb_hostterrain_row maps surface x to `g_ht_cx + (x - extra)`, so nothing
    // outside the camera rect plus the bands is ever read. Rendering and
    // shrouding the whole 1536x1024 map every frame -- which is what this did
    // -- is 1.57M pixel writes for the render, up to 1.5M more for the shroud,
    // and 4,096 fog-cell reads, nearly all of it thrown away.
    //
    // docs/325 phase 1 called the visible-rect design unnecessary on a 1.256 ms
    // measurement of the render ALONE at a 512x192 guest. Making locate succeed
    // turned this path on for that level for the first time, and it
    // measured 26.9 fps. One generous margin of a metatile each side covers the
    // half-open edges and any sub-tile scroll.
    {
        int sw = (int)nds_gpu3d_output_width();
        int sh = (int)nds_gpu3d_output_height();
        if (sw < 256) sw = 256;
        if (sh < 192) sh = 192;
        const int extra = (sw - 256) / 2;
        const int extra_y = (sh - 192) / 2;
        int cx = 0, cy = 0;
        lb_field_camera_rect(&g_field_info, &cx, &cy);
        lb_field_set_window(cx - extra - 24, cy - extra_y - 16,
                            cx - extra + sw + 24, cy - extra_y + sh + 16);
    }
    lb_field_render(&g_field_info, g_field_px.data());

    // THE FIELD IS RENDERED FULLY REVEALED, so the shroud has to go on before
    // anyone samples it. Phase 2 shipped this path without it: the host terrain
    // supplies the compositor's base pixel for the band columns, so every band
    // pixel was showing unexplored ground. In the native columns the guest's own
    // BG1/BG2 overdraw it and the omission is invisible, which is why a frame
    // that looked correct was not -- the bands were the only place it showed,
    // and there it reads as terrain the player has not earned.
    //
    // Applying it over the WHOLE map, not just the bands, is deliberate: the
    // native columns are overdrawn by the guest anyway, so there is nothing to
    // gain from masking the work and a seam to lose if the mask is off by a
    // pixel.
    //
    // Source is the located map record (docs/247) via lb_field's shroud, not
    // the minimap readback -- see refresh_from_record in lb_field.cpp.
    lb_field_refresh_shroud(&g_field_info);
    lb_field_apply_shroud(&g_field_info, g_field_px.data());
    g_ht_fogged = lb_field_shroud_from_record() != 0;

    // LB_FIELDDUMP=1 writes the host field ITSELF, once, after the shroud has
    // been applied to it.
    //
    // This separates two things a composited frame cannot: "the shroud never
    // went on" and "the shroud went on and the compositor did not use this
    // buffer". A frame showing 1536 px of revealed terrain while the record
    // reports 3619 of 4032 cells hidden is consistent with both, and counting
    // colours cannot tell them apart either -- the same trap phase 2 hit.
    {
        static int on = -1;
        if (on < 0) {
            const char* v = std::getenv("LB_FIELDDUMP");
            on = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        static int warm = 0;
        if (on && ++warm == 600) {
            if (std::FILE* f = std::fopen("lb_field_fogged.ppm", "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", g_ht_w, g_ht_h);
                const size_t n = (size_t)g_ht_w * (size_t)g_ht_h;
                for (size_t i = 0; i < n; ++i) {
                    const uint32_t p = g_field_px[i];
                    const unsigned char rgb[3] = {
                        (unsigned char)((p >> 16) & 0xFF),
                        (unsigned char)((p >> 8) & 0xFF),
                        (unsigned char)(p & 0xFF)};
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
                std::fprintf(stderr,
                             "[lb_fielddump] wrote lb_field_fogged.ppm "
                             "%dx%d\n", g_ht_w, g_ht_h);
                std::fflush(stderr);
            }
        }
    }

    lb_field_camera_rect(&g_field_info, &g_ht_cx, &g_ht_cy);
    lb_itcm_watch();   /* LB_ITCMWATCH=1; see lb_field.cpp */

    // The SAME fields lb_widemap's gate reads, from the MAIN thread, at the
    // same cadence. The gate reports map=00000000 while lb_field's chain reads
    // 022BD750 as a valid 64x64 map; printing both from known-different threads
    // is what tells a threading problem from a timing one.
    {
        static int on = -1;
        if (on < 0) {
            const char* v = std::getenv("LB_BANDWHY");
            on = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (on) {
            static unsigned n = 0;
            if ((n++ % 60u) == 0u) lb_field_gate_probe();
        }
    }

    // LB_CAMTRACE=1: one line per frame, the guest's camera beside the host's.
    //
    // WHY NOT MEASURE THE PICTURE. Consecutive-frame pixel deltas cannot decide
    // this: both the terrain and the fog are 8 px tiled patterns, so a shift by
    // a whole tile produces a BYTE-IDENTICAL region. Measured on one recording,
    // the band region held 6 to 48 distinct colours with the top four covering
    // 81-90% of it. A delta of 0.0 there means "moved by a multiple of the tile
    // period" exactly as much as it means "did not move", and reading it as the
    // latter nearly produced a fix for a bug that was never demonstrated.
    //
    // The camera numbers have no such ambiguity. If the host's rect equals the
    // guest's camera every frame, the two halves of the picture are positioned
    // identically and there is no relative shake to find, whatever the pixels
    // happen to look like.
    {
        static int on = -1;
        if (on < 0) {
            const char* v = std::getenv("LB_CAMTRACE");
            on = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (on) {
            static unsigned n = 0;
            int gx = 0, gy = 0;
            const int have = lb_field_camera(&gx, &gy) ? 1 : 0;
            // The band too, and the picture's own top-left in the world
            // (camera minus band): that is what the player sees move. The
            // camera alone moves through docs/373's dead zone at a map edge
            // while the picture stands still.
            const int bx = lb_shown_band_x(), by = lb_shown_band_y();
            std::fprintf(stderr,
                         "[lb_camtrace] f=%u guest=%d,%d(%d) host=%d,%d "
                         "hofs=%u vofs=%u band=%d,%d picture=%d,%d\n",
                         n++, gx, gy, have, g_ht_cx, g_ht_cy,
                         nds_io_debug_read(9, 0x04000014u, 16),
                         nds_io_debug_read(9, 0x04000016u, 16),
                         bx, by, gx - bx, gy - by);
            std::fflush(stderr);
        }
    }
    g_ht_ready = true;

    // Report the PREVIOUS frame's row and reset, so the counters are always a
    // whole row from one frame rather than a smear across several.
    // PRINT EVEN WHEN THE COUNTS ARE ZERO. The first version required a
    // non-zero count and printed nothing at all, which cannot be told apart
    // from "never ran" -- a blank readout is a question, and this project has
    // lost measurements to exactly that before.
    //
    // `calls` is here because the frame rate says this is running more than
    // once a frame: 22.4 fps against a 57 baseline is far more than the
    // 1.256 ms Phase 1 measured for one whole-map render. If calls climbs
    // faster than frames, the composite is entered more than once per frame
    // and the render needs a per-frame guard rather than a per-call one.
    static unsigned n = 0;
    static unsigned calls = 0;
    ++calls;
    if ((++n % 60u) == 0u) {
        std::fprintf(stderr,
                     "[lb_hostterrain] row %d: band %u/%u kept, native %u/%u "
                     "kept  (cam %d,%d  map %dx%d  prepares %u)\n",
                     g_ht_note_row,
                     g_ht_band_kept, g_ht_band_seen,
                     g_ht_native_kept, g_ht_native_seen,
                     g_ht_cx, g_ht_cy, g_ht_w, g_ht_h, calls);
        std::fprintf(stderr,
                     "[lb_hostterrain]   rows: %u asked, %u served, "
                     "%u not-ready, %u off-map (first dy %d)\n",
                     g_ht_calls, g_ht_hits, g_ht_rej_notready,
                     g_ht_rej_range, g_ht_first_dy);
        std::fflush(stderr);
        g_ht_calls = g_ht_hits = 0;
        g_ht_rej_notready = g_ht_rej_range = 0;
    }
    g_ht_band_seen = g_ht_band_kept = 0;
    g_ht_native_seen = g_ht_native_kept = 0;
    return 1;
}

// dx, dy are offsets from the guest window's top-left in MAP PIXELS, so the
// caller passes (x - extra, y - extra_y) and the bands come out negative or
// past the width -- which is the whole point.
//
// lb_field_render writes 0xFFRRGGBB and the compositor works in packed 6-bit
// (r | g<<8 | b<<16). The conversion back is exact: to_rgb32 builds each
// channel as (c6 << 2) | (c6 >> 4), and >> 2 recovers c6 for every c6 < 64.
// The VALIDATED map chain, as located on the main render path.
//
// lb_widemap_fill() runs in the VBLANK hook, and at that moment the pointer at
// 0x02156CB0 reads 0 -- measured, same thread, same run, while a probe at
// composite time reads 022BD750 with w=64 h=64. So the fill cannot read the
// map for itself where it is called, and every gate applied to that read fails:
// `fill ok=0 bail2=1711` over ~1800 frames, which left g_map_w at 0 and made
// band_compute() take its span/2 fallback forever -- the 4 px band alternation
// seen as shake.
//
// This hands the fill the values captured where they ARE valid. Returns 0 until
// a level is located, which is the honest answer during menus.
// CACHED SEPARATELY FROM g_field_located, which churns. That flag is cleared
// by the overlay panel's per-frame lb_field_still_valid() check and only
// restored when prepare re-locates, which is rate-limited to once every 90
// frames -- so at any given vblank it is usually false even though the map has
// been located and host terrain is drawing. Reading it here returned chain=0 on
// every sample while [lb_hostterrain] was logging happily.
//
// These are set whenever a locate SUCCEEDS and are not cleared by that churn.
// The map object address is stable for a battle; docs/247 records that it moves
// BETWEEN battles, which a fresh locate updates. Declared beside g_field_info.
extern "C" int lb_field_map_chain(uint32_t* map, int* w, int* h) {
    if (!g_chain_map || g_chain_w <= 0 || g_chain_h <= 0) return 0;
    if (map) *map = g_chain_map;
    if (w) *w = g_chain_w;
    if (h) *h = g_chain_h;
    return 1;
}

extern "C" int lb_hostterrain_active(void) { return g_ht_ready ? 1 : 0; }

// True only when the host field is both ready AND carries the record-sourced
// shroud. A caller that stops drawing its own fog on the strength of this
// must not be left with no fog at all on a frame where the record did not
// validate, which is the state between battles and mid-load.
extern "C" int lb_hostterrain_fogged(void) {
    return (g_ht_ready && g_ht_fogged) ? 1 : 0;
}

// ONE FETCH PER ROW INSTEAD OF ONE CALL PER PIXEL.
//
// lb_hostterrain_pixel was called 98,304 times a frame -- every pixel, through
// a non-inlined extern "C" boundary, to do two bounds checks and one array
// read. The row is contiguous in the map buffer, so the caller can have a
// pointer and the x range once and index it directly: 450 calls a frame
// instead of 98,304.
//
// Returns 0 when this output row has no map behind it at all, in which case
// the caller leaves the whole row on the palette backdrop.
// THE COUNTERS LIVE HERE, not in lb_hostterrain_pixel, which is dead.
//
// They were written for the per-pixel API and left behind when this row API
// replaced it. For two days every log read `sampler: 0 calls, 0 hits` -- which
// says "the compositor never asks for host terrain", the exact failure this
// subsystem has had before -- while the compositor was in fact asking once per
// output row and being served. docs/330 nearly filed it as a regression.
//
// Rows, not pixels: `calls` is rows asked for, `hits` is rows served.
extern "C" int lb_hostterrain_row(int dy, const uint32_t** row,
                                  int* dx_lo, int* dx_hi) {
    ++g_ht_calls;
    if (!g_ht_ready) { ++g_ht_rej_notready; return 0; }
    if (!row || !dx_lo || !dx_hi) return 0;
    const int wy = g_ht_cy + dy;
    if (wy < 0 || wy >= g_ht_h) {
        ++g_ht_rej_range;
        if (g_ht_rej_range == 1u) g_ht_first_dy = dy;
        return 0;
    }
    ++g_ht_hits;
    *row = g_field_px.data() + (size_t)wy * (size_t)g_ht_w;
    // The x range, in the caller's dx space, for which g_ht_cx + dx is on the
    // map. Half-open: [dx_lo, dx_hi).
    *dx_lo = -g_ht_cx;
    *dx_hi = g_ht_w - g_ht_cx;
    return 1;
}

// ONE COMPOSITED FRAME, ONCE. The counters cannot answer the last question.
//
// `band 80/256 kept` is consistent with two very different worlds: the band is
// mostly unexplored and BG2 is correctly drawing shroud over the terrain, or
// something is wrongly painting over it. Only looking settles that, and every
// other route to a picture failed for reasons unrelated to this change -- the
// harness's end-of-run captures are all the "Defeated!" screen because the
// route never issues orders, and LB_DUMP_EVERY starved the frontend until the
// debug server refused input.
//
// A single frame costs one write and nothing per frame afterwards. Written
// raw as PPM beside the run's other output, which lb_exp already converts.
extern "C" void lb_hostterrain_dump_once(const uint32_t* px, int w, int h) {
    static bool done = false;
    if (done || !px || w <= 0 || h <= 0) return;
    // Wait for the field to have been up a while, so the frame is a battle in
    // progress rather than the first frame it happened to locate on.
    static int warm = 0;
    if (!g_ht_ready) return;
    if (++warm < 600) return;
    done = true;
    std::FILE* f = std::fopen("lb_hostterrain.ppm", "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        const uint32_t c = px[i];
        const unsigned char rgb[3] = {
            (unsigned char)((c >> 16) & 0xFFu),
            (unsigned char)((c >> 8) & 0xFFu),
            (unsigned char)(c & 0xFFu)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[lb_hostterrain] dumped one %dx%d frame\n", w, h);
    std::fflush(stderr);
}

// DEAD SINCE THE ROW API REPLACED IT, and kept only as the readable statement
// of the mapping that lb_hostterrain_row does in bulk. Nothing calls it: the
// compositor asked per pixel -- 98,304 crossings of an extern "C" boundary per
// frame for two bounds checks and an array read -- and that was hoisted to one
// call per output row. Its counters moved with it; leaving them here is what
// made every log say `sampler: 0 calls` (docs/330).
extern "C" int lb_hostterrain_pixel(int dx, int dy, uint32_t* packed6) {
    if (!g_ht_ready) return 0;
    if (!packed6) return 0;
    const int wx = g_ht_cx + dx;
    const int wy = g_ht_cy + dy;
    if (wx < 0 || wy < 0 || wx >= g_ht_w || wy >= g_ht_h) return 0;
    const uint32_t c = g_field_px[(size_t)wy * (size_t)g_ht_w + (size_t)wx];
    *packed6 = ((c >> 18) & 0x3Fu)
             | (((c >> 10) & 0x3Fu) << 8)
             | (((c >> 2) & 0x3Fu) << 16);
    return 1;
}

// DID IT ACTUALLY REACH THE SCREEN? Counted, because screenshots would not.
//
// Six attempts to verify Phase 2 by picture failed on harness mechanics, none
// of them to do with this change: the route never issues orders, so the AI
// wins and every end-of-run capture is the "Defeated!" screen; the mid-battle
// captures only happen if the route gets far enough, which it often does not;
// and turning on the frame dumper made the frontend too slow for the harness's
// own touch delivery ("debug server refused touch: frontend busy or frozen").
//
// The question is not visual anyway. It is: does a band pixel end up carrying
// the colour the host map supplied, or does something overdraw it? That is a
// comparison, and a comparison can be counted.
//
// ONE ROW, THE MIDDLE ONE. A whole frame would need atomics -- the row body is
// fanned out across workers -- and one row is ~1,700 compares, which is
// nothing. One row is also enough: the question is whether the path works at
// all.
//
// IT WAS `y == 96`, A CONSTANT, and that is not comparable across window
// sizes. At the 1280x720 this was written against, row 96 sat in the field and
// read `band 109/384 kept`. At an 1855x988 window the same constant lands
// near the top of the picture and reads `band 0/672 kept` -- which looks like
// the subsystem failing at that size and is not evidence of it (docs/330).
// output_height/2 is in the field at every size, so two runs can be compared.
// The caller passes the row it chose so the log can say which one it was.
extern "C" void lb_hostterrain_note(int row, int in_band, uint32_t out_rgb32,
                                    uint32_t ht_rgb32) {
    g_ht_note_row = row;
    if (in_band) {
        ++g_ht_band_seen;
        if (out_rgb32 == ht_rgb32) ++g_ht_band_kept;
    } else {
        ++g_ht_native_seen;
        if (out_rgb32 == ht_rgb32) ++g_ht_native_kept;
    }
}

// PHASE 1 OF docs/325: WHAT DOES HOST-SIDE TERRAIN COST PER FRAME?
//
// The native-renderer plan turns on one number nobody has: lb_field_render
// decodes the whole level from the grid and today runs on LB_MAPDUMP frames
// only, which is one frame in 240. Per frame at 3440x1440 it is either
// affordable -- in which case the rest of the plan is wiring -- or it is not,
// and a visible-rect-plus-dirty-cache design has to come first. The plan says
// explicitly: do not design the cache before measuring.
//
// This measures the render alone, not the locate: lb_field_locate scans 4 MB
// and is a once-per-level cost that the plan never proposed paying per frame.
//
// LB_FIELDBENCH=1. Off by default and free when off: one cached getenv and an
// integer compare, on a path that already runs per frame.
void lb_field_bench_tick() {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_FIELDBENCH");
        on = (v && v[0] == '1') ? 1 : 0;
    }
    if (!on) return;
    if (!g_field_located) {
        g_field_located = lb_field_locate(&g_field_info);
        if (!g_field_located) return;
    }
    int mw = 0, mh = 0;
    lb_field_size(&g_field_info, &mw, &mh);
    if (mw <= 0 || mh <= 0) { g_field_located = false; return; }
    g_field_px.resize((size_t)mw * (size_t)mh);

    // BOTH WINDOWS, IN THE SAME FRAME.
    //
    // Every end-to-end fps comparison today was confounded by machine load --
    // another project's processes on this box cost ~30% at one point, and
    // single-run fps swung 43-50 with no code change at all. Timing the
    // whole-map render and the clipped render back to back inside one frame
    // removes that: whatever else the machine is doing affects both equally.
    //
    // The window lb_hostterrain_prepare set for this frame is saved and
    // restored, so the "clipped" case measures exactly what the game renders.
    int sw0 = 0, sw1 = 0, sh0 = 0, sh1 = 0;
    lb_field_get_window(&sw0, &sh0, &sw1, &sh1);

    lb_field_set_window(0, 0, 0, 0);
    const auto tf0 = std::chrono::steady_clock::now();
    lb_field_render(&g_field_info, g_field_px.data());
    const auto tf1 = std::chrono::steady_clock::now();

    lb_field_set_window(sw0, sh0, sw1, sh1);
    const auto t0 = std::chrono::steady_clock::now();
    lb_field_render(&g_field_info, g_field_px.data());
    const auto t1 = std::chrono::steady_clock::now();

    static double full_acc = 0.0;
    full_acc += std::chrono::duration<double, std::milli>(tf1 - tf0).count();

    static double acc = 0.0;
    static double worst = 0.0;
    static unsigned n = 0;
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    acc += ms;
    if (ms > worst) worst = ms;
    if ((++n % 120u) == 0u) {
        // A 60 fps budget is 16.7 ms for EVERYTHING, so the mean matters and
        // the worst matters more -- a spike that blows the budget once every
        // two seconds is what a player sees as a stutter, and a mean hides it.
        std::fprintf(stderr,
                     "[lb_fieldbench] map %dx%d  WHOLE-MAP mean %.3f ms | "
                     "CLIPPED mean %.3f ms  worst %.3f ms  over %u frames "
                     "(60fps budget 16.7 ms/frame TOTAL)\n",
                     mw, mh, full_acc / (double)n, acc / (double)n, worst, n);
        std::fflush(stderr);
    }
}

void lb_map_dump_tick() {
    static int mevery = -1;
    if (mevery < 0) {
        const char* v = std::getenv("LB_MAPDUMP");
        mevery = v ? std::atoi(v) : 0;
        if (mevery == 1) mevery = 240;
    }
    if (mevery <= 0) return;

    // The SHARED counter, so lb_map pairs with lb_comp and lb_layers by frame.
    const int f = (int)lb_dump_frame_get();
    static int last_dumped = -1;
    if (f == last_dumped || (f % mevery) != 0) return;

    // Locating scans main RAM, so it is attempted only on a dump frame and
    // only while still unlocated -- the same once-per-level cost the panel
    // pays, not a per-frame one.
    if (!g_field_located) {
        g_field_located = lb_field_locate(&g_field_info);
        if (!g_field_located) return;
    }
    int mw = 0, mh = 0;
    lb_field_size(&g_field_info, &mw, &mh);
    if (mw <= 0 || mh <= 0) { g_field_located = false; return; }

    // Refresh the camera the same way the panel does. The camera comes from
    // the game's own state (docs/174), so this keeps working over unexplored
    // ground where terrain matching has nothing to match.
    int cam_dummy = 0;
    (void)(lb_field_camera(&cam_dummy, &cam_dummy) ||
           lb_field_track_camera(&g_field_info));

    g_field_px.resize((size_t)mw * (size_t)mh);
    lb_field_render(&g_field_info, g_field_px.data());

    int cx = 0, cy = 0;
    lb_field_camera_rect(&g_field_info, &cx, &cy);
    int vw = 256, vh = 192, dummy = 0;
    if (!lb_overlay_field_rect(&dummy, &dummy, &dummy, &dummy, &vw, &vh)) {
        vw = 256;
        vh = 192;
    }

    last_dumped = f;
    const char* dir = std::getenv("LB_DUMP_ADAPTIVE");
    char path[300];
    std::snprintf(path, sizeof path, "%s%slb_map-%dx%d-%d.ppm",
                  dir ? dir : ".", dir ? "/" : "", mw, mh, f);
    if (std::FILE* mf = std::fopen(path, "wb")) {
        std::fprintf(mf, "P6\n%d %d\n255\n", mw, mh);
        std::vector<uint8_t> row((size_t)mw * 3u);
        for (int y = 0; y < mh; ++y) {
            for (int x = 0; x < mw; ++x) {
                const uint32_t v = g_field_px[(size_t)y * mw + x];
                row[(size_t)x * 3u + 0u] = (uint8_t)(v >> 16);
                row[(size_t)x * 3u + 1u] = (uint8_t)(v >> 8);
                row[(size_t)x * 3u + 2u] = (uint8_t)v;
            }
            std::fwrite(row.data(), 1, row.size(), mf);
        }
        std::fclose(mf);
    }
    std::snprintf(path, sizeof path, "%s%slb_map-%dx%d-%d.txt",
                  dir ? dir : ".", dir ? "/" : "", mw, mh, f);
    if (std::FILE* tf = std::fopen(path, "w")) {
        std::fprintf(tf, "cx %d\ncy %d\nvw %d\nvh %d\nmw %d\nmh %d\n",
                     cx, cy, vw, vh, mw, mh);
        std::fclose(tf);
    }
    std::fprintf(stderr,
                 "[lb_mapdump] frame %d: map %dx%d, surface %dx%d at "
                 "map (%d,%d)\n", f, mw, mh, vw, vh, cx, cy);
    std::fflush(stderr);
}

void draw_battlefield(SDL_Renderer* renderer) {
    if (!g_field_visible || !renderer) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.6f, vp->WorkSize.y * 0.6f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Battlefield", &g_field_visible)) { ImGui::End(); return; }
    keep_on_screen();

    if (!g_field_located) {
        ImGui::TextDisabled("Locating the level grid...");
        if (ImGui::Button("Locate now") || ImGui::GetFrameCount() % 120 == 0)
            g_field_located = lb_field_locate(&g_field_info);
        ImGui::TextDisabled("Scans main RAM, so this runs once per level "
                            "rather than per frame.");
        ImGui::End();
        return;
    }

    // The located cell goes stale the moment the player scrolls, so the camera
    // box is re-matched each frame.
    //
    // Tracking CAN legitimately fail, and dropping back to a full re-locate on
    // every such frame would be ruinous -- lb_field_locate() scans main RAM.
    // The failure is not a bug in the matcher: terrain matching has nothing to
    // match against when the view is over unexplored ground, because the shroud
    // replaces the level's own tiles. So the box is kept and flagged stale
    // rather than thrown away.
    // The camera comes from the game's own state now (docs/174), so this no
    // longer depends on terrain matching and keeps working over unexplored
    // ground. The terrain tracker is kept only to refresh the located cell for
    // the fallback path.
    int cam_dummy = 0;
    const bool tracked = lb_field_camera(&cam_dummy, &cam_dummy) ||
                         lb_field_track_camera(&g_field_info);

    int mw = 0, mh = 0;
    lb_field_size(&g_field_info, &mw, &mh);
    if (mw <= 0 || mh <= 0) { g_field_located = false; ImGui::End(); return; }

    if (!g_field_tex || g_field_tex_w != mw || g_field_tex_h != mh) {
        if (g_field_tex) SDL_DestroyTexture(g_field_tex);
        g_field_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, mw, mh);
        g_field_tex_w = mw; g_field_tex_h = mh;
        if (!g_field_tex) { ImGui::End(); return; }
        SDL_SetTextureBlendMode(g_field_tex, SDL_BLENDMODE_NONE);
        g_field_redraw = 0;                 // a new texture holds nothing yet
    }

    // NOT EVERY FRAME. This decodes the WHOLE level out of VRAM and uploads it:
    // on the Free Play map that is 1536x1024 pixels, 1.5M of them, and doing it
    // sixty times a second cost five frames per second whenever the panel was
    // open -- measured, 44.9 -> 39.95. It is an overview of terrain that
    // changes when a building goes up or the shroud lifts, so twice a second
    // is more than the picture can actually show. The camera box below is
    // drawn every frame, and that is the part that has to track.
    if (--g_field_redraw <= 0) {
        g_field_redraw = 30;
        g_field_px.resize((size_t)mw * (size_t)mh);
        lb_field_render(&g_field_info, g_field_px.data());
        SDL_UpdateTexture(g_field_tex, nullptr, g_field_px.data(),
                          mw * (int)sizeof(uint32_t));
    }

    ImGui::Text("%d x %d px  (%d x %d metatiles)", mw, mh,
                g_field_info.grid_w, g_field_info.grid_h);
    ImGui::SameLine();
    if (ImGui::SmallButton("Re-locate")) {
        g_field_located = false;
        g_field_redraw = 0;                 // and decode it again immediately
    }
    if (!tracked) {
        ImGui::SameLine();
        ImGui::TextDisabled("(camera box stale: view is over unmatched ground)");
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float sx = avail.x / (float)mw, sy = avail.y / (float)mh;
    const float sc = sx < sy ? sx : sy;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)g_field_tex,
                 ImVec2((float)mw * sc, (float)mh * sc));

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The camera rectangle, in map pixels. The guest viewport is the adaptive
    // width by the DS height, so a widened window really does cover more map
    // and the box grows with it -- which is the point of drawing it from the
    // located camera rather than from a fixed-size box.
    int cx = 0, cy = 0;
    lb_field_camera_rect(&g_field_info, &cx, &cy);
    int vw = 256, vh = 192, dummy = 0;
    if (!lb_overlay_field_rect(&dummy, &dummy, &dummy, &dummy, &vw, &vh)) {
        vw = 256;
        vh = 192;
    }
    dl->AddRect(ImVec2(origin.x + cx * sc, origin.y + cy * sc),
                ImVec2(origin.x + (cx + vw) * sc, origin.y + (cy + vh) * sc),
                IM_COL32(255, 255, 255, 230), 0.0f, 0, 2.0f);

    // The LB_MAPDUMP block that used to sit here is now
    // lb_map_dump_tick(), called per frame from lb_overlay_draw. It
    // needed this panel OPEN, and an open panel takes the mouse, so the
    // harness route could never reach a battle to dump one. docs/291.

    // Live entities. lb_field_entity_at() reports positions in the
    // simulation's world space; docs/169 measured the constant offset between
    // that and the level array's frame.
    int owner = 0, cat = 0, ex = 0, ey = 0;
    for (int i = 0; lb_field_entity_at(i, &owner, &cat, &ex, &ey); ++i) {
        static const ImU32 col[4] = {IM_COL32(255, 70, 70, 255),
                                     IM_COL32(70, 160, 255, 255),
                                     IM_COL32(80, 220, 80, 255),
                                     IM_COL32(240, 220, 60, 255)};
        const float r = (cat >= 7 ? 7.0f : 4.0f) * (sc < 1.0f ? 1.0f : sc);
        const ImVec2 c(origin.x + ex * sc, origin.y + ey * sc);
        dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
                    col[owner & 3], 0.0f, 0, 2.0f);
    }
    ImGui::End();
}

// ---- docs/396: THE MENU, laid out as Ship of Harkinian's ------------------
//
// One floating window of ten tabs became SoH's modern menu (soh/SohGui/
// Menu.cpp, Menu::DrawElement): the whole window dimmed behind it, a block
// centred in it (90% of a large window, never wider than 16:9), a header row
// of categories with Quit, Reset and Close at its right, a white rule, a
// sidebar of the selected category's sections, a second rule, and the
// selected section's controls. Esc or F1 opens and closes it, as before;
// Ctrl+R resets the game, as SoH's does.
//
// The sections are the old tabs' own drawing functions, unchanged: in menu
// mode tab_begin() answers "is this the section on show" instead of drawing
// a tab, and tab_end() closes nothing. LB_PANEL_TAB still names one (a tab's
// label, as before).
struct MenuSection { const char* label; const char* id; void (*draw)(); };
struct MenuHeader { const char* label; std::vector<MenuSection> sections; };

void section_windows() {
    if (!tab_begin("Windows")) return;
    ImGui::Checkbox("Show the top screen overlay", &g_top_visible);
        badge(WIRED);   // g_top_visible gates draw_top_screen
        // THE "WIDE FIELD" VIEWER NO LONGER HAS A CONTROL HERE.
        //
        // Two rules settle it: adaptive window size is ONE toggle, and no
        // ImGui setting or button remains unwired or incomplete. This was
        // three more toggles for the same wish -- a
        // whole-map view drawn by the port, plus "fill the window (crop)" and
        // "view width (DS screens)" to shape it -- and it carried its own
        // label saying NOT PLAYABLE, because clicks landed in the wrong place,
        // the in-field HUD stayed at native screen positions, scrolling slid
        // units across a fixed picture, and it cost ~1.5M pixel writes and a
        // 6 MB upload per frame.
        //
        // Enhanced now delivers what it was reaching for and delivers it
        // properly: the field is rendered at the window's own size, so the
        // terrain, fog, units, effects and HUD all extend to the real edges,
        // clicks map through the same rect the picture is drawn into, and the
        // HUD anchors to the window's own corners (docs/212, docs/215). A
        // second, worse route to the same place is not a setting.
        //
        // The code is still there and still reachable with LB_WIDE_FIELD=1,
        // because it is the only thing that draws the WHOLE level at once and
        // that is useful when investigating the map. It is a diagnostic now,
        // not an option, which is why it is behind a variable and not a box.
        // LB_FIELDPANEL=1 opens it at startup, so the panel can be exercised
        // headlessly. It only executes when it is DRAWN, so a run that never
        // opens it is no evidence at all about whether it works -- which is
        // how "map viewer doesn't fetch properly" went unnoticed.
        ImGui::Checkbox("Show the whole battlefield (panel)", &g_field_visible);
        badge(WIRED);   // g_field_visible gates draw_battlefield
        help("Draws the entire level from the grid in RAM, not just the\n"
             "256x192 the DS can show. The white box is what the game\n"
             "window covers, so it grows as the window widens.");
    tab_end();
}

// ── docs/404: THE BALANCE MENU ─────────────────────────────────────────────
//
// Every unit's and building's values, by faction, read from the game's own
// blueprints and edited in place (lb_balance.cpp's top layer, over the pack
// and Same-builders); their attacks' damage; and the packs and files. A value
// the player set is drawn yellow, and a right-click puts the one beneath it
// back. Online the cells are locked: both machines must run one rule set.
const char* const kBalFactions[7] = {"King", "Wizard", "Pirate", "Imperial",
                                     "Earth", "Alien", "Neutral and story"};
const char* const kBalCategory[20] = {
    "Hero", "Builder", "Melee", "Ranged", "Mounted", "Transport", "Special",
    "Base", "Mill", "Mine", "Farm", "Barracks", "Vehicle Center", "Tower",
    "Tower 2", "Tower 3", "Shipyard", "Bridge", "Gate", "Wall"};
// Short column labels, in lb_balance.cpp's field order, and what each means.
const char* const kBalUnitCols[10] = {"Cost", "Build", "Health", "Mana",
                                      "Speed", "Melee", "Spread", "Interval",
                                      "Range", "Yield"};
const char* const kBalUnitTips[10] = {
    "Bricks to build it.",
    "Build time (the construction's divisor; larger is slower).",
    "Hit points.",
    "A hero's mana.",
    "Tiles per second (with \"Speed as stored value\", the Entities Editor's number).",
    "Melee damage, the low end.",
    "Melee damage spread: the high end is damage + spread.",
    "Ticks between attacks (30 a second).",
    "Attack range.",
    "Resources a mine, quarry or well cap yields."};
int g_bal_faction = 0;
int g_bal_filter = 0;   // 0 all, 1 units, 2 buildings
bool g_bal_speed_raw = false;   // docs/425 item 3: Speed as the stored value
char g_bal_path[512] = "balance.json";
std::string g_bal_msg;
bool g_bal_msg_bad = false;

int bal_faction_of(int rec) {
    char name[48];
    if (lb_balance_record_name(rec, name, (int)sizeof name) < 2 || name[1] != '_')
        return 6;
    static const char kPrefix[6] = {'K', 'W', 'P', 'I', 'E', 'A'};
    for (int f = 0; f < 6; ++f)
        if (name[0] == kPrefix[f]) return f;
    return 6;
}

// docs/408: the bridges come in pairs, the horizontal and vertical piece of
// each size (121/122 small, 123/124 medium, 125/126 large) with the same
// values, so one row shows the pair and a change writes both. The gates
// (127/128) are never built (docs/405) and are not listed.
int bal_twin(int rec) {
    return (rec == 121 || rec == 123 || rec == 125) ? rec + 1 : -1;
}
bool bal_unlisted(int rec) {
    // And two placeholders with no values at all: 137 MarsFalvour ("REMOVE
    // THIS") and 139 SpaceCriminalShipColor (no name).
    return rec == 122 || rec == 124 || rec == 126 || rec == 127 || rec == 128 ||
           rec == 137 || rec == 139;
}
const char* bal_row_name(int rec) {
    if (rec == 121) return "Bridge (small)";
    if (rec == 123) return "Bridge (medium)";
    if (rec == 125) return "Bridge (large)";
    return lb_teams_unit_name(rec);
}
void bal_set(int rec, int field, int v) {
    lb_balance_custom_set(rec, field, v);
    const int twin = bal_twin(rec);
    if (twin >= 0) lb_balance_custom_set(twin, field, v);
}

// One editable value. Enter commits it; a right-click on a yellow one puts
// the value beneath it back.
void bal_cell(int kind, int rec, int field, bool locked) {
    const int live = lb_balance_value(rec, field);
    ImGui::PushID(rec * 32 + field);
    if (live < 0) {
        ImGui::TextDisabled("-");
        ImGui::PopID();
        return;
    }
    const int custom = lb_balance_custom_get(rec, field);
    const bool speed = std::strcmp(lb_balance_field_name(kind, field), "Speed") == 0;
    if (custom >= 0)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.25f, 1.0f));
    ImGui::SetNextItemWidth(speed ? 64.0f : 58.0f);
    ImGui::BeginDisabled(locked);
    if (speed && g_bal_speed_raw) {
        int v = live;   // the Entities Editor's own number
        if (ImGui::InputInt("##v", &v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
            bal_set(rec, field, v < 1 ? 1 : v);
    } else if (speed) {
        float tps = (float)live * 30.0f / 4096.0f;
        if (ImGui::InputFloat("##v", &tps, 0.0f, 0.0f, "%.2f",
                              ImGuiInputTextFlags_EnterReturnsTrue)) {
            long raw = std::lround(tps * 4096.0f / 30.0f);
            if (raw < 1) raw = 1;
            bal_set(rec, field, (int)raw);
        }
    } else {
        int v = live;
        if (ImGui::InputInt("##v", &v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
            const int mx = lb_balance_field_max(kind, field);
            bal_set(rec, field, v < 0 ? 0 : (v > mx ? mx : v));
        }
    }
    ImGui::EndDisabled();
    // docs/425 item 3: a speed says both of its numbers.
    if (speed && custom < 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%.2f tiles per second; stored value %d.",
                          (float)live * 30.0f / 4096.0f, live);
    if (custom >= 0) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            const int b = lb_balance_base(rec, field);
            if (speed)
                ImGui::SetTooltip("Yours: %.2f tiles per second, stored value %d. Beneath "
                                  "it: %.2f (%d). Right-click to put that back.",
                                  (float)live * 30.0f / 4096.0f, live,
                                  (float)b * 30.0f / 4096.0f, b);
            else
                ImGui::SetTooltip("Yours. Beneath it: %d. Right-click to put "
                                  "that back.", b);
        }
        if (!locked && ImGui::IsItemClicked(ImGuiMouseButton_Right))
            bal_set(rec, field, -1);
    }
    ImGui::PopID();
}

// docs/408: an on/off field (a projectile's flags), as a tick box.
void bal_flag(int rec, int field, bool locked) {
    const int live = lb_balance_value(rec, field);
    const int custom = lb_balance_custom_get(rec, field);
    ImGui::PushID(rec * 32 + field);
    bool on = live > 0;
    if (custom >= 0)
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 0.82f, 0.25f, 1.0f));
    ImGui::BeginDisabled(locked || live < 0);
    if (ImGui::Checkbox("##f", &on)) bal_set(rec, field, on ? 1 : 0);
    ImGui::EndDisabled();
    if (custom >= 0) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Yours. Beneath it: %s. Right-click to put that "
                              "back.", lb_balance_base(rec, field) ? "on" : "off");
        if (!locked && ImGui::IsItemClicked(ImGuiMouseButton_Right))
            bal_set(rec, field, -1);
    }
    ImGui::PopID();
}

// The lock and the hint, above each table.
bool bal_header(bool picks = false) {
    if (!lb_patches_blueprint_base()) {
        ImGui::TextDisabled("The values are read from the game's own "
                            "blueprints, which load at its title screen.");
        return false;
    }
    if (const char* why = lb_balance_custom_blocked()) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f), "Locked: %s.", why);
        // docs/418: online, whose numbers are in force, named by the seed
        // both machines compare.
        if (lb_balance_host_on()) {
            ImGui::TextDisabled("The host's balance is in force here: %s, %d value(s) "
                                "(seed %016llX)%s.",
                                lb_balance_name(lb_balance_host_slot()),
                                lb_balance_host_size(), lb_balance_host_seed(),
                                lb_balance_host_set_current() ? "" : ", being applied");
        } else {
            const int s = lb_balance_effective_slot(), b = lb_balance_effective_builders();
            ImGui::TextDisabled("Yours go to the players who join: %s, %d value(s) "
                                "(seed %016llX).", lb_balance_name(s),
                                lb_balance_custom_size(), lb_balance_local_seed(s, b));
        }
        return true;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(picks ? "Pick a spell. It applies at once, in a battle "
                                "too; recordings carry it. Yellow choices are "
                                "yours: right-click one to put it back."
                              : "Type a value and press Enter. It applies at once, "
                                "in a battle too; recordings carry it. Yellow "
                                "values are yours: right-click one to put it back.");
    ImGui::PopTextWrapPos();
    return true;
}

void tab_balance_units() {
    if (!tab_begin("Balance Units")) return;
    guide("Change a unit's cost, health, damage and speed. Click a value to edit it; "
          "yellow means you changed it, and right-click puts it back. Online, the "
          "host's balance applies to everyone.");
    static bool faction_env = false;   // LB_BAL_FACTION=0..7, a test aid
    if (!faction_env) {
        faction_env = true;
        if (const char* v = std::getenv("LB_BAL_FACTION")) g_bal_faction = std::atoi(v);
        if (std::getenv("LB_BAL_SPEED_RAW")) g_bal_speed_raw = true;   // test aid
    }
    if (bal_header()) {
        const bool locked = lb_balance_custom_blocked() != nullptr;
        ImGui::SetNextItemWidth(170.0f);
        ImGui::Combo("Faction", &g_bal_faction, kBalFactions, 7);
        ImGui::SameLine();
        const char* filters[] = {"Everything", "Units", "Buildings"};
        ImGui::SetNextItemWidth(130.0f);
        ImGui::Combo("Show", &g_bal_filter, filters, 3);
        ImGui::SameLine();
        ImGui::Checkbox("Speed as stored value", &g_bal_speed_raw);   // docs/425 item 3
        help("Shows and edits Speed as the number the Entities Editor stores (a\n"
             "distance per tick, x4096), so values can be typed across. Off:\n"
             "tiles per second. Hover a speed to see both.");
        // The stat columns; the spell fields after them have their own
        // section (tab_balance_heroes).
        const int nf = LB_BAL_SPELL_FIELD0;
        SDL_Texture* atlas = lb_teams_hud_atlas(g_team_renderer)   /* docs/408 */;
        if (ImGui::BeginTable("balunits", nf + 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit,
                              ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableSetupColumn("##icon");
            ImGui::TableSetupColumn("Name");
            for (int f = 0; f < nf && f < 10; ++f)
                ImGui::TableSetupColumn(kBalUnitCols[f]);
            ImGui::TableSetupColumn("Attack");
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int c = 0; c < nf + 3; ++c) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TableHeader(ImGui::TableGetColumnName(c));
                if (c >= 2 && c < nf + 2 && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", kBalUnitTips[c - 2]);
            }
            const int n = lb_balance_record_count(LB_BAL_UNITS);
            for (int rec = 0; rec < n; ++rec) {
                if (bal_faction_of(rec) != g_bal_faction) continue;
                if (bal_unlisted(rec)) continue;   // docs/408
                const int cat = lb_balance_record_category(rec);
                const bool building = cat >= 7;
                if ((g_bal_filter == 1 && building) || (g_bal_filter == 2 && !building))
                    continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                float u0, v0, u1, v1;
                if (atlas && lb_teams_hud_icon_uv(rec, &u0, &v0, &u1, &v1))
                    ImGui::Image((ImTextureID)(intptr_t)atlas, ImVec2(24.0f, 24.0f),
                                 ImVec2(u0, v0), ImVec2(u1, v1));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(bal_row_name(rec));
                ImGui::SameLine();
                ImGui::TextDisabled("%s", cat >= 0 && cat < 20 ? kBalCategory[cat] : "");
                for (int f = 0; f < nf; ++f) {
                    ImGui::TableNextColumn();
                    bal_cell(LB_BAL_UNITS, rec, f, locked);
                }
                ImGui::TableNextColumn();
                const int p = lb_balance_record_projectile(rec);
                char pname[48] = "";
                if (p >= 0) lb_balance_record_name(p, pname, (int)sizeof pname);
                ImGui::TextDisabled("%s", pname);
            }
            ImGui::EndTable();
        }
    }
    tab_end();
}

// docs/426: the Entities Editor's other unit fields, in their own section.
void tab_balance_traits() {
    if (!tab_begin("Balance Traits")) return;
    guide("Change where a unit can go, its size, its shot and its sight. Click a value "
          "to edit it. Online, the host's balance applies to everyone.");
    if (bal_header()) {
        const bool locked = lb_balance_custom_blocked() != nullptr;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Where a unit can go, how big it is, what shot it uses "
                            "and how far it sees. Yellow = yours; right-click to put back.");
        ImGui::PopTextWrapPos();
        SDL_Texture* atlas = lb_teams_hud_atlas(g_team_renderer);
        static const char* const kCols[] = {
            "Land", "Water", "Trees", "Bld collision", "Land<->water", "Not water-only",
            "Hitbox", "Uninteract.", "Speed: none", "Projectile", "AI priority",
            "Defogged"};
        static const char* const kTips[] = {
            "The unit can walk on land. (Read by FUN_0205079C.)",
            "The unit can enter water. Ticking Land or Water also sets Building "
            "collision, Land<->water and Not water-only the way the Entities Editor does.",
            "The unit can walk through trees. (Read by FUN_02001510.)",
            "Collides with buildings: the editor sets 0 for a unit that has Land and "
            "Water, else 1. (FUN_0205079C.)",
            "Crosses between land and water: the editor sets 1 with Land and Water.",
            "The editor's BoatAntiFlag: ticked for every unit except water-only ones "
            "(ships) and Shipyards. Ticking Water without Land clears it.",
            "Hitbox size. (FUN_02001170.)",
            "Uninteractable. UNCONFIRMED: the reader at +0x21 was not traced to the "
            "blueprint, so this may do nothing.",
            "No speed: writes 0x0000FFFF, as the Entities Editor's NONE does.",
            "The projectile record a ranged unit fires, or Melee. A unit that "
            "fires one has Melee damage 0.",
            "AI priority: accumulated in the combat path (docs/25).",
            "Defogged area: how far the unit clears the fog; FF clears the map. "
            "A unit auto-attacks only within it, so raise it with Range "
            "(Range above it is not used for auto-attack)."};
        const int nc = 12;
        if (ImGui::BeginTable("baltraits", 2 + nc,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableSetupColumn("##icon");
            ImGui::TableSetupColumn("Name");
            for (int c = 0; c < nc; ++c) ImGui::TableSetupColumn(kCols[c]);
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int c = 0; c < nc + 2; ++c) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TableHeader(ImGui::TableGetColumnName(c));
                if (c >= 2 && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kTips[c - 2]);
            }
            // Fields: F+0 Land, 1 Water, 2 Trees, 3 Bld collision, 4 Land<->water,
            // 5 Hitbox, 6 Uninteractable, 7 Projectile, 8 AI priority, 9 Defogged,
            // 10 Water-only.
            const int F = LB_BAL_TRAIT_FIELD0;
            const int n = lb_balance_record_count(LB_BAL_UNITS);
            for (int rec = 0; rec < n; ++rec) {
                if (bal_unlisted(rec)) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                float u0, v0, u1, v1;
                if (atlas && lb_teams_hud_icon_uv(rec, &u0, &v0, &u1, &v1))
                    ImGui::Image((ImTextureID)(intptr_t)atlas, ImVec2(24.0f, 24.0f),
                                 ImVec2(u0, v0), ImVec2(u1, v1));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(bal_row_name(rec));
                // Land, Water: a change also sets the dependent flags, as the editor does.
                for (int k = 0; k < 2; ++k) {
                    ImGui::TableNextColumn();
                    const int was = lb_balance_custom_get(rec, F + k);
                    const int wasv = lb_balance_value(rec, F + k);
                    bal_flag(rec, F + k, locked);
                    const int now = lb_balance_custom_get(rec, F + k);
                    if (now >= 0 && (now != was) && wasv >= 0) {
                        const bool l = lb_balance_value(rec, F) > 0;
                        const bool w = lb_balance_value(rec, F + 1) > 0;
                        bal_set(rec, F + 3, (l && w) ? 0 : 1);
                        bal_set(rec, F + 4, (l && w) ? 1 : 0);
                        // BoatAntiFlag is 0 for a water-only unit, 1 otherwise.
                        bal_set(rec, F + 10, (w && !l) ? 0 : 1);
                    }
                }
                ImGui::TableNextColumn(); bal_flag(rec, F + 2, locked);
                ImGui::TableNextColumn(); bal_flag(rec, F + 3, locked);
                ImGui::TableNextColumn(); bal_flag(rec, F + 4, locked);
                ImGui::TableNextColumn(); bal_flag(rec, F + 10, locked);
                ImGui::TableNextColumn(); bal_cell(LB_BAL_UNITS, rec, F + 5, locked);
                ImGui::TableNextColumn(); bal_flag(rec, F + 6, locked);
                ImGui::TableNextColumn();   // Speed: none
                {
                    const int sp = LB_BAL_SPEED_FIELD;
                    const int live = lb_balance_value(rec, sp);
                    const int custom = lb_balance_custom_get(rec, sp);
                    ImGui::PushID(rec * 32 + 30);
                    bool none = live == 0xFFFF;
                    ImGui::BeginDisabled(locked || (live < 0 && custom < 0));
                    if (ImGui::Checkbox("##none", &none))
                        bal_set(rec, sp, none ? 0xFFFF : -1);
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::TableNextColumn();   // Projectile
                {
                    const int fld = F + 7;
                    const int live = lb_balance_value(rec, fld);
                    const int custom = lb_balance_custom_get(rec, fld);
                    ImGui::PushID(rec * 32 + fld);
                    if (custom >= 0)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.25f, 1.0f));
                    char cur[48] = "Melee";
                    const int first = lb_balance_record_first(LB_BAL_ATTACKS);
                    const int cnt = lb_balance_record_count(LB_BAL_ATTACKS);
                    if (live >= first && live < first + cnt)
                        lb_balance_record_name(live, cur, (int)sizeof cur);
                    else if (live >= 0 && live != 0xFFFF)
                        std::snprintf(cur, sizeof cur, "#%d", live);
                    ImGui::SetNextItemWidth(130.0f);
                    ImGui::BeginDisabled(locked || live < 0);
                    if (ImGui::BeginCombo("##proj", cur)) {
                        if (ImGui::Selectable("Melee", live == 0xFFFF))
                            bal_set(rec, fld, 0xFFFF);
                        for (int p = first; p < first + cnt; ++p) {
                            char nm[48];
                            lb_balance_record_name(p, nm, (int)sizeof nm);
                            char item[64];
                            std::snprintf(item, sizeof item, "%s##%d", nm, p);
                            if (ImGui::Selectable(item, p == live)) bal_set(rec, fld, p);
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::EndDisabled();
                    if (custom >= 0) {
                        ImGui::PopStyleColor();
                        if (!locked && ImGui::IsItemClicked(ImGuiMouseButton_Right))
                            bal_set(rec, fld, -1);
                    }
                    ImGui::PopID();
                }
                ImGui::TableNextColumn(); bal_cell(LB_BAL_UNITS, rec, F + 8, locked);
                ImGui::TableNextColumn(); bal_cell(LB_BAL_UNITS, rec, F + 9, locked);
            }
            ImGui::EndTable();
        }
    }
    tab_end();
}

void tab_balance_attacks() {
    if (!tab_begin("Balance Attacks")) return;
    if (bal_header()) {
        const bool locked = lb_balance_custom_blocked() != nullptr;
        ImGui::TextDisabled("A ranged unit's or a special's attack. Damage is "
                            "a range: each hit deals between the two. The "
                            "ticks are on/off.");
        // docs/408: what each field does, from the code that reads it.
        static const char* const kTips[] = {
            "Damage to what it touches on the way or hits (FUN_0206E408).",
            "",
            "Damage where it lands: the target, and the blast if it explodes\n"
            "(FUN_0206E450, FUN_0206E60C).",
            "",
            "Flight speed, tiles per second. Applies from the next shot.",
            "Its contact damage reaches buildings.",
            "It can touch flying units.",
            "It can touch ground units, bridges and gates.",
            "Its contact damage reaches walls.",
            "Its contact damage hurts its own side too.",
            "It damages enemies it passes over, not only its target. With\n"
            "Stopped by enemies off, it pierces.",
            "Impassable ground (cliffs, deep water) stops it.",
            "Buildings stop it; off, it flies over them.",
            "Its own side's units stop it.",
            "The first enemy unit it touches stops it.",
            "Walls stop it.",
            "It explodes where it lands, hitting what is near."};
        const int nf = lb_balance_field_count(LB_BAL_ATTACKS);
        if (ImGui::BeginTable("balattacks", nf + 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("Attack");
            for (int f = 0; f < nf; ++f)
                ImGui::TableSetupColumn(lb_balance_field_name(LB_BAL_ATTACKS, f));
            ImGui::TableSetupColumn("Used by");
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int c = 0; c < nf + 2; ++c) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TableHeader(ImGui::TableGetColumnName(c));
                const int f = c - 1;
                if (f >= 0 && f < nf && f < (int)IM_ARRAYSIZE(kTips) &&
                    kTips[f][0] && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", kTips[f]);
            }
            const int first = lb_balance_record_first(LB_BAL_ATTACKS);
            const int n = lb_balance_record_count(LB_BAL_ATTACKS);
            const int units = lb_balance_record_count(LB_BAL_UNITS);
            for (int rec = first; rec < first + n; ++rec) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char name[48] = "";
                lb_balance_record_name(rec, name, (int)sizeof name);
                ImGui::TextUnformatted(name);
                for (int f = 0; f < nf; ++f) {
                    ImGui::TableNextColumn();
                    if (lb_balance_field_max(LB_BAL_ATTACKS, f) == 1)
                        bal_flag(rec, f, locked);
                    else
                        bal_cell(LB_BAL_ATTACKS, rec, f, locked);
                }
                ImGui::TableNextColumn();
                int shown = 0;
                for (int u = 0; u < units && shown < 4; ++u) {
                    if (lb_balance_record_projectile(u) != rec) continue;
                    if (shown++) ImGui::SameLine();
                    ImGui::TextDisabled("%s", lb_teams_unit_name(u));
                }
            }
            ImGui::EndTable();
        }
    }
    tab_end();
}

// docs/408: which hero casts what. A hero's five slots are spell ids at
// blueprint +0x72 (docs/106); the yellow brick lists them in slot order and
// an empty slot (0) shows nothing. The names are the game's own
// (lb_teams_spell_name); these English ones stand in before its text loads.
// Id 3 (Area Heal) is skipped by the yellow brick's filler (FUN_020DC004's
// `cmp r0,#3`), and 0x21/0x22 are impact payloads, so none is offered.
const char* lb_spell_name(int id) {
    if (id == 0) return "(empty)";
    if (const char* s = lb_teams_spell_name(id)) return s;
    static const char* const kEn[0x21] = {
        "(empty)", "Unit Heal", "Unit Heal", "Area Heal", "Unit Speed Boost",
        "Area Speed Boost", "Unit Damage Boost", "Area Damage Boost",
        "Unit Armour Boost", "Area Armour Boost", "Forest Spawn",
        "Crystal Cache", "Jungle Growth", "Earthquake", "Fireball", "Lightning",
        "Thunder Hammer", "Mining Buff", "Roar", "Logging Buff", "Monkey Swarm",
        "Crab Swarm", "Coconut Storm", "Artillery", "Trade Winds",
        "Arrow Volley", "Teleport", "EMP", "Space Laser", "ESP", "Tracking",
        "Cluster Bomb", "Hot Wire"};
    return id > 0 && id <= 0x20 ? kEn[id] : "?";
}
bool lb_spell_offered(int id) {
    return id == 0 || (id >= 1 && id <= 0x20 && id != 3);
}

void tab_balance_heroes() {
    if (!tab_begin("Balance Heroes")) return;
    if (bal_header(true)) {
        const bool locked = lb_balance_custom_blocked() != nullptr;
        ImGui::TextDisabled("Each hero's five spell slots, in the order the "
                            "yellow brick lists them. Empty slots show nothing.");
        SDL_Texture* atlas = lb_teams_hud_atlas(g_team_renderer)   /* docs/408 */;
        if (ImGui::BeginTable("balheroes", 2 + LB_BAL_SPELLS,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableSetupColumn("##icon");
            ImGui::TableSetupColumn("Hero");
            for (int s = 0; s < LB_BAL_SPELLS; ++s)
                ImGui::TableSetupColumn(lb_balance_field_name(LB_BAL_UNITS,
                                                              LB_BAL_SPELL_FIELD0 + s));
            ImGui::TableHeadersRow();
            const int n = lb_balance_record_count(LB_BAL_UNITS);
            for (int rec = 0; rec < n; ++rec) {
                if (lb_balance_record_category(rec) != 0) continue;   // heroes
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                float u0, v0, u1, v1;
                if (atlas && lb_teams_hud_icon_uv(rec, &u0, &v0, &u1, &v1))
                    ImGui::Image((ImTextureID)(intptr_t)atlas, ImVec2(24.0f, 24.0f),
                                 ImVec2(u0, v0), ImVec2(u1, v1));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(lb_teams_unit_name(rec));
                for (int s = 0; s < LB_BAL_SPELLS; ++s) {
                    ImGui::TableNextColumn();
                    const int field = LB_BAL_SPELL_FIELD0 + s;
                    const int live = lb_balance_value(rec, field);
                    const int custom = lb_balance_custom_get(rec, field);
                    ImGui::PushID(rec * 32 + field);
                    if (custom >= 0)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImVec4(1.0f, 0.82f, 0.25f, 1.0f));
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::BeginDisabled(locked);
                    if (ImGui::BeginCombo("##sp", lb_spell_name(live < 0 ? 0 : live))) {
                        for (int id = 0; id <= 0x22; ++id) {
                            if (!lb_spell_offered(id)) continue;
                            char item[64];
                            // Ids 1 and 2 are both "Unit Heal"; 2's record
                            // casts at once (target mask 0x40, FUN_020A92D4).
                            std::snprintf(item, sizeof item, "%s%s##%d",
                                          lb_spell_name(id),
                                          id == 2 ? " (instant)" : "", id);
                            if (ImGui::Selectable(item, id == live))
                                lb_balance_custom_set(rec, field, id);
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::EndDisabled();
                    if (custom >= 0) {
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Yours. Beneath it: %s. Right-click "
                                              "to put that back.",
                                              lb_spell_name(lb_balance_base(rec, field)));
                        if (!locked && ImGui::IsItemClicked(ImGuiMouseButton_Right))
                            lb_balance_custom_set(rec, field, -1);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    tab_end();
}

void tab_balance_presets() {
    if (!tab_begin("Balance Presets")) return;
    guide("Pick a mod's balance below, or use Import from ROM to take it from a mod's "
          ".nds. Your own changes from the Balance pages are kept on top. Online, "
          "the host's balance applies to everyone.");
    draw_balance_pack();
    ImGui::Separator();
    ImGui::Text("Your values: %d", lb_balance_custom_size());
    help("Kept in settings.json, on top of the pack above. They apply at\n"
         "once, in a battle too, except online; recordings carry them.");
    ImGui::SameLine();
    ImGui::BeginDisabled(lb_balance_custom_size() == 0 ||
                         lb_balance_custom_blocked() != nullptr);
    if (ImGui::Button("Clear all")) lb_balance_custom_clear_all();
    ImGui::EndDisabled();
    // docs/416: values in the game's code rather than a blueprint.
    if (lb_patches_blueprint_base()) {
        const int rec = lb_balance_record_first(LB_BAL_GLOBALS);
        const bool locked = lb_balance_custom_blocked() != nullptr;
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Gold Brick value");
        ImGui::SameLine();
        bal_cell(LB_BAL_GLOBALS, rec, 0, locked);
        help("Bricks a hero's team gets for a Gold Brick: 1000 in the game\n"
             "(150 where the game gives 150, which takes the same share).\n"
             "Offline only, like every value here; recordings carry it.");
    }
    ImGui::Spacing();
    ImGui::InputText("File", g_bal_path, sizeof g_bal_path);
    if (ImGui::Button("Export my balance (.json)...")) {
        if (lb_pick_json_file(g_bal_path, (int)sizeof g_bal_path, 1)) {
            static char text[32768];
            lb_balance_custom_text(text, (int)sizeof text);
            std::string o = "{\n  \"lb_balance\": 1,\n";
            char b[64];
            std::snprintf(b, sizeof b, "  \"pack\": %d,\n", g_set.balance_patch);
            o += b;
            json_put_str(o, "values", text, false);
            o += "}\n";
            FILE* f = std::fopen(g_bal_path, "wb");
            g_bal_msg_bad = !f || std::fwrite(o.data(), 1, o.size(), f) != o.size();
            if (f) std::fclose(f);
            g_bal_msg = g_bal_msg_bad ? "Could not write that file."
                                      : "Exported " +
                                            std::to_string(lb_balance_custom_size()) +
                                            " value(s).";
        }
    }
    ImGui::SetItemTooltip("Saves the chosen preset and every value you changed.");
    ImGui::SameLine();
    ImGui::BeginDisabled(lb_balance_custom_blocked() != nullptr);
    if (ImGui::Button("Import my balance (.json)...")) {
        if (lb_pick_json_file(g_bal_path, (int)sizeof g_bal_path, 0)) {
            std::string s;
            if (FILE* f = std::fopen(g_bal_path, "rb")) {
                char chunk[4096];
                size_t got;
                while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0 &&
                       s.size() < (1u << 20))
                    s.append(chunk, got);
                std::fclose(f);
            }
            if (s.find("\"lb_balance\"") == std::string::npos) {
                g_bal_msg = "That is not a balance file.";
                g_bal_msg_bad = true;
            } else {
                static char text[32768];
                text[0] = 0;
                json_get_str(s, "values", text, sizeof text);
                int pack = g_set.balance_patch;
                json_get_int(s, "pack", &pack);
                if (pack >= 0 && pack <= lb_balance_pack_count()) g_set.balance_patch = pack;
                const int n = lb_balance_custom_parse(text);
                g_bal_msg = "Imported " + std::to_string(n) + " value(s) and the " +
                            lb_balance_name(g_set.balance_patch) + " pack.";
                g_bal_msg_bad = false;
            }
        }
    }
    ImGui::SetItemTooltip("Loads a file made by Export: the preset and the values in it.");
    ImGui::SameLine();
    // docs/426: every byte of a ROM's or Entities.ebp's blueprint that differs
    // from the game's own and falls in a menu field becomes your value, on Vanilla.
    if (ImGui::Button("Import from ROM...")) {
        if (lb_pick_balance_rom_file(g_bal_path, (int)sizeof g_bal_path)) {
            char msg[320];
            const int n = lb_balance_import_file(g_bal_path, msg, (int)sizeof msg);
            if (n >= 0) g_set.balance_patch = 0;
            g_bal_msg = msg;
            g_bal_msg_bad = n < 0;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reads a mod's balance from its .nds or an Entities.ebp; it "
                          "replaces your changed values.\n"
                          "A .nds or an Entities.ebp (raw or compressed): its numbers "
                          "that differ from the game's own become your values,\n"
                          "with the pack set to Vanilla. Changes outside the menu's "
                          "fields are counted, not applied.");
    ImGui::EndDisabled();
    if (!g_bal_msg.empty())
        ImGui::TextColored(g_bal_msg_bad ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f)
                                         : ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                           "%s", g_bal_msg.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("Credits");   // docs/426
    ImGui::TextUnformatted("FactionWars by Alpha47");
    ImGui::TextUnformatted("Vanil+ by RynCat");
    ImGui::TextUnformatted("Rebalance by RynCat");
    ImGui::TextUnformatted("FunLite by Dream-Foxtrot");
    ImGui::TextUnformatted("Fusion by NinjaGoat");
    tab_end();
}

// ---- docs/430, SG-0013: UPDATE CHECK ---------------------------------------------
//
// GitHub's releases/latest of the public repository, over WinHTTP
// (lb_https_request), on a thread of its own. The result is only ever shown;
// nothing is downloaded or installed. At startup (when allowed) the check is
// quiet unless a different, newer-looking release exists.
struct UpdateInfo {
    int state = 0;   // 0 not checked, 1 checking, 2 done, 3 failed
    std::string tag, name, notes, url, error;
    std::string asset;    // docs/435: the release's game zip (not the source zip)
    bool newer = false;   // a release that differs from this build and is not older
};
std::mutex g_upd_mu;
UpdateInfo g_upd;
std::atomic<bool> g_upd_busy{false};
constexpr const char* kReleasesUrl = "https://github.com/AgitationSkeleton/legobattles-pc/releases";

// The string value of the first "key": in a JSON text, unescaped.
std::string upd_json_str(const std::string& s, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    while (p != std::string::npos) {
        size_t q = p + pat.size();
        while (q < s.size() && (s[q] == ' ' || s[q] == ':')) ++q;
        if (q < s.size() && s[q] == '"') {
            std::string out;
            for (++q; q < s.size() && s[q] != '"'; ++q) {
                if (s[q] == '\\' && q + 1 < s.size()) {
                    const char c = s[++q];
                    if (c == 'n') out += '\n';
                    else if (c == 'r') {}
                    else if (c == 't') out += ' ';
                    else if (c == 'u' && q + 4 < s.size()) {
                        const unsigned cp = (unsigned)std::strtoul(s.substr(q + 1, 4).c_str(), nullptr, 16);
                        out += cp < 0x80 ? std::string(1, (char)cp) : std::string("?");
                        q += 4;
                    } else out += c;
                } else out += s[q];
            }
            return out;
        }
        p = s.find(pat, p + 1);
    }
    return std::string();
}

// "playtest-2026-10-23 (abc123)" -> 20261023; 0 when there is no date.
unsigned upd_date_of(const std::string& v) {
    for (size_t i = 0; i + 10 <= v.size(); ++i)
        if (std::isdigit((unsigned char)v[i]) && v.compare(i + 4, 1, "-") == 0 &&
            v.compare(i + 7, 1, "-") == 0 && std::isdigit((unsigned char)v[i + 9]))
            return (unsigned)(std::atoi(v.substr(i, 4).c_str()) * 10000 +
                              std::atoi(v.substr(i + 5, 2).c_str()) * 100 +
                              std::atoi(v.substr(i + 8, 2).c_str()));
    return 0;
}

void upd_run(bool quiet) {
    std::string reply, error;
    const bool ok = lb_https_request("api.github.com", 443, "GET",
                                     // The list, newest first: releases/latest
                                     // skips pre-releases, and every early
                                     // build was one.
                                     "/repos/AgitationSkeleton/legobattles-pc/releases?per_page=1",
                                     "", &reply, &error);
    UpdateInfo r;
    if (!ok) {
        r.state = 3;
        r.error = "Could not reach GitHub (" + error + ").";
    } else if (reply.find("\"tag_name\"") == std::string::npos) {
        r.state = 2;   // a 404: no release has been published yet
        r.error = "No release has been published yet.";
    } else {
        r.state = 2;
        r.tag = upd_json_str(reply, "tag_name");
        r.name = upd_json_str(reply, "name");
        r.notes = upd_json_str(reply, "body");
        r.url = upd_json_str(reply, "html_url");
        if (r.url.compare(0, 8, "https://") != 0) r.url = kReleasesUrl;
        if (r.notes.size() > 1500) r.notes = r.notes.substr(0, 1500) + "...";
        const std::string mine = lb_port_version();
        const unsigned dm = upd_date_of(mine), dr = upd_date_of(r.tag + " " + r.name);
        // Dates when both builds have one; otherwise any difference counts.
        // A tag is "v0.1.0-beta.8"; version.txt holds "0.1.0-beta.8".
        const std::string bare = (!r.tag.empty() && (r.tag[0] == 'v' || r.tag[0] == 'V'))
                                     ? r.tag.substr(1) : r.tag;
        r.newer = (dm && dr) ? dr > dm : (!bare.empty() && mine.find(bare) == std::string::npos);
        // docs/435: the game zip among the assets (the other is the source).
        const std::string key = "\"browser_download_url\":";
        for (size_t p = reply.find(key); p != std::string::npos; p = reply.find(key, p + 1)) {
            const size_t q0 = reply.find('"', p + key.size());
            const size_t q1 = q0 == std::string::npos ? q0 : reply.find('"', q0 + 1);
            if (q1 == std::string::npos) break;
            const std::string u = reply.substr(q0 + 1, q1 - q0 - 1);
            if (u.size() > 4 && u.compare(u.size() - 4, 4, ".zip") == 0 &&
                u.find("runner-source") == std::string::npos &&
                u.find("/LegoBattles-PC-") != std::string::npos)
                r.asset = u;
        }
    }
    std::fprintf(stderr, "[lb_update] %s: %s tag \"%s\" newer=%d (this build \"%s\")%s%s\n",
                 quiet ? "startup check" : "check", r.state == 3 ? "failed" : "ok",
                 r.tag.c_str(), r.newer ? 1 : 0, lb_port_version(),
                 r.error.empty() ? "" : " - ", r.error.c_str());
    std::fflush(stderr);
    {
        std::lock_guard<std::mutex> lk(g_upd_mu);
        g_upd = r;
    }
    if (quiet && r.newer) {
        // docs/435: Install updates automatically downloads and installs it;
        // the new version runs from the next start.
        if (g_set.auto_update && !r.asset.empty() &&
            lb_update_start(r.asset.c_str(), r.tag.c_str())) {
            const std::string msg = "Installing " + r.tag + " (you run " + lb_port_version() +
                                    "). Settings > General says when it is ready.";
            lb_overlay_notice(msg.c_str());
        } else {
            const std::string msg = "A newer version is available: " + r.tag +
                                    " (you run " + lb_port_version() +
                                    "). Settings > General can install it.";
            lb_overlay_notice(msg.c_str());
        }
    }
    g_upd_busy = false;
}

void upd_start(bool quiet) {
    if (g_upd_busy.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(g_upd_mu);
        g_upd.state = 1;
    }
    std::thread([quiet]() { upd_run(quiet); }).detach();
}

// Once, a few seconds in, when the setting is on (LB_UPDATE_CHECK=0 skips it).
void upd_startup_tick() {
    static unsigned long long at = 0;
    static bool done = false;
    if (done || !g_ready) return;
    if (!at) at = SDL_GetTicks64() + 6000ull;
    if (SDL_GetTicks64() < at) return;
    done = true;
    const char* e = std::getenv("LB_UPDATE_CHECK");
    if (g_set.update_check && !(e && e[0] == '0')) upd_start(true);
}

void tab_general() {
    if (!tab_begin("General")) return;
    guide("Version, updates and the Discord status. These stay on this machine and do "
          "not affect online play.");
    ImGui::Text("Version: %s", lb_port_version());
    ImGui::Spacing();
    UpdateInfo u;
    {
        std::lock_guard<std::mutex> lk(g_upd_mu);
        u = g_upd;
    }
    ImGui::BeginDisabled(u.state == 1);
    if (ImGui::Button("Check for updates")) upd_start(false);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Asks GitHub for the newest release. Nothing is downloaded or "
                          "installed.");
    if (u.state == 1) ImGui::TextUnformatted("Checking...");
    else if (u.state == 3) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", u.error.c_str());
    else if (u.state == 2 && u.tag.empty()) ImGui::TextUnformatted(u.error.c_str());
    else if (u.state == 2) {
        ImGui::Text("Newest release: %s%s%s", u.tag.c_str(), u.name.empty() ? "" : "  -  ",
                    u.name.c_str());
        ImGui::TextColored(u.newer ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                           "%s", u.newer ? "A newer version is available." :
                                           "You have the newest version (or newer).");
        if (!u.notes.empty()) {
            ImGui::BeginChild("lb_upd_notes", ImVec2(0, 150), ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(u.notes.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        }
        if (ImGui::Button("Open download page")) SDL_OpenURL(u.url.c_str());
        // docs/435: or download and install it here.
        if (u.newer && !u.asset.empty()) {
            ImGui::SameLine();
            const bool can = lb_update_supported() != 0;
            char umsg[256];
            const int us = lb_update_state(umsg, (int)sizeof umsg);
            ImGui::BeginDisabled(!can || us == 1 || us == 2 || us == 3);
            if (ImGui::Button("Install update")) lb_update_start(u.asset.c_str(), u.tag.c_str());
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Downloads the new version from GitHub and installs it over\n"
                                  "this one. Your settings, saves, ROM and mods are kept.");
            if (!can)
                ImGui::TextDisabled("Installing here works in the downloaded game only.");
        }
    }
    {
        char umsg[256];
        const int us = lb_update_state(umsg, (int)sizeof umsg);
        if (us == 1 || us == 2) ImGui::TextUnformatted(umsg);
        if (us == 4) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", umsg);
        if (us == 3) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", umsg);
            if (ImGui::Button("Restart now")) lb_update_restart();
        }
    }
    ImGui::Checkbox("Check for updates at startup", &g_set.update_check);
    help("Looks for a newer release a few seconds after the game starts, and says so\n"
         "only when there is one.");
    ImGui::BeginDisabled(!g_set.update_check);
    ImGui::Checkbox("Install updates automatically", &g_set.auto_update);
    help("With the check at startup on: a newer release is downloaded and installed\n"
         "by itself, and runs from the next start. Your settings, saves, ROM and mods\n"
         "are kept.");
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Discord");
    ImGui::Checkbox("Discord status", &g_set.discord_status);
    help("Shows what you are doing (menus, a mission, a lobby, a match) in the Discord\n"
         "desktop app, if it is running. Nothing happens when it is not.");
    ImGui::BeginDisabled(!g_set.discord_status);
    ImGui::Checkbox("Show my hero in Discord status", &g_set.discord_hero);
    help("Adds the hero you play as a small picture on your Discord status.");
    ImGui::EndDisabled();
    tab_end();
}

// ---- SG-0018: Network > Scores ---------------------------------------------------
// The master server's score tables (services/master-server-cloudflare/src/scores.js):
// one table per balance ruleset, filtered by bans, fetched on a worker thread.

struct ScoreRow {
    std::string name;
    int hero = -1, matches = 0, wins = 0, losses = 0;
    int units_built = 0, units_lost = 0, units_killed = 0;
    int buildings_built = 0, buildings_lost = 0, buildings_killed = 0, bricks = 0;
};
struct ScoreState {
    int state = 0;                  // 0 not asked, 1 asking, 2 shown, 3 failed
    std::string error;
    std::vector<std::string> rulesets;
    std::vector<ScoreRow> rows;
};
std::mutex g_sc_mu;
ScoreState g_sc;
std::atomic<bool> g_sc_busy{false};

// A number or a string field of one flat JSON object.
int sc_num(const std::string& o, const char* key) {
    const std::string pat = std::string("\"") + key + "\":";
    const size_t p = o.find(pat);
    return p == std::string::npos ? 0 : std::atoi(o.c_str() + p + pat.size());
}

std::string sc_url(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// Each {...} in the array that follows `key`.
std::vector<std::string> sc_objects(const std::string& s, const char* key) {
    std::vector<std::string> out;
    size_t p = s.find(std::string("\"") + key + "\":[");
    if (p == std::string::npos) return out;
    p = s.find('[', p);
    int depth = 0;
    size_t start = 0;
    bool str = false;
    for (size_t i = p + 1; i < s.size(); ++i) {
        const char c = s[i];
        if (str) {
            if (c == '\\') ++i;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        else if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}') { if (--depth == 0) out.push_back(s.substr(start, i - start + 1)); }
        else if (c == ']' && depth == 0) break;
    }
    return out;
}

// A GET to the master server; the reply, or an empty string with `error`.
std::string sc_get(const std::string& host, int port, const std::string& path,
                   std::string* error) {
    std::vector<char> buf(256 * 1024);
    const int n = lb_net_http_get(host.c_str(), port, path.c_str(), buf.data(), (int)buf.size());
    if (n < 0) {
        *error = buf.data()[0] ? buf.data() : "no answer";
        return std::string();
    }
    return std::string(buf.data(), (size_t)n);
}

void sc_run(std::string host, int port, std::string query) {
    ScoreState r;
    std::string error;
    const std::string list = sc_get(host, port, "/scores/rulesets", &error);
    const std::string table = error.empty() ? sc_get(host, port, "/scores" + query, &error)
                                            : std::string();
    if (!error.empty()) {
        r.state = 3;
        r.error = "Could not reach the master server (" + error + ").";
    } else if (table.find("\"rows\"") == std::string::npos) {
        r.state = 3;
        r.error = "This master server keeps no score tables.";
    } else {
        r.state = 2;
        for (const std::string& o : sc_objects(list, "rulesets")) {
            const std::string v = upd_json_str(o, "ruleset");
            if (!v.empty()) r.rulesets.push_back(v);
        }
        for (const std::string& o : sc_objects(table, "rows")) {
            ScoreRow row;
            row.name = upd_json_str(o, "name");
            row.hero = o.find("\"hero\":-1") != std::string::npos ? -1 : sc_num(o, "hero");
            row.matches = sc_num(o, "matches");
            row.wins = sc_num(o, "wins");
            row.losses = sc_num(o, "losses");
            row.units_built = sc_num(o, "units_built");
            row.units_lost = sc_num(o, "units_lost");
            row.units_killed = sc_num(o, "units_killed");
            row.buildings_built = sc_num(o, "buildings_built");
            row.buildings_lost = sc_num(o, "buildings_lost");
            row.buildings_killed = sc_num(o, "buildings_killed");
            row.bricks = sc_num(o, "bricks");
            r.rows.push_back(row);
        }
    }
    std::fprintf(stderr, "[lb_scores] table %s: %s, %zu row(s), %zu ruleset(s) at %llu ms%s%s\n",
                 query.c_str(), r.state == 2 ? "ok" : "failed", r.rows.size(),
                 r.rulesets.size(), (unsigned long long)SDL_GetTicks64(),
                 r.error.empty() ? "" : " - ", r.error.c_str());
    std::fflush(stderr);
    {
        std::lock_guard<std::mutex> lk(g_sc_mu);
        g_sc = r;
    }
    g_sc_busy = false;
}

// A head from the battle HUD's sheet, as a picture (no button).
void hero_image(int bp, float size) {
    SDL_Texture* hud = lb_teams_hud_atlas(g_team_renderer);
    float u0, v0, u1, v1;
    if (bp >= 0 && hud && lb_teams_hud_icon_uv(bp, &u0, &v0, &u1, &v1))
        ImGui::Image((ImTextureID)(intptr_t)hud, ImVec2(size, size), ImVec2(u0, v0),
                     ImVec2(u1, v1));
    else
        ImGui::Dummy(ImVec2(size, size));
}

void tab_scores() {
    if (!tab_begin("Scores")) return;
    guide("The best players of online matches, kept by the master server: one table per "
          "balance ruleset. The host reports each match when it ends. Matches with cheats "
          "on do not count.");
    ImGui::Checkbox("Keep me out of the score tables", &g_set.scores_opt_out);
    help("Your matches are not added to the tables from the next lobby you host or\n"
         "join. Matches already counted stay.");
    ImGui::Spacing();

    static int ruleset = 0;   // 0 every ruleset, else rulesets[ruleset - 1]
    static int bans = 0;      // 0 any, 1 with unit bans, 2 without
    static int unit = -1;     // a unit that was banned, or -1 for any
    static std::string asked = "\x01";
    ScoreState s;
    {
        std::lock_guard<std::mutex> lk(g_sc_mu);
        s = g_sc;
    }
    if (ruleset > (int)s.rulesets.size()) ruleset = 0;
    {
        static int said = -1;
        if (said != s.state) {
            said = s.state;
            std::fprintf(stderr, "[lb_scores] page: state %d, %zu row(s) at %llu ms\n",
                         s.state, s.rows.size(), (unsigned long long)SDL_GetTicks64());
            std::fflush(stderr);
        }
    }

    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("Ruleset", ruleset ? s.rulesets[(size_t)ruleset - 1].c_str()
                                             : "Every ruleset")) {
        if (ImGui::Selectable("Every ruleset", ruleset == 0)) ruleset = 0;
        for (size_t i = 0; i < s.rulesets.size(); ++i)
            if (ImGui::Selectable(s.rulesets[i].c_str(), ruleset == (int)i + 1))
                ruleset = (int)i + 1;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    static const char* const kBans[] = {"Any bans", "With unit bans", "Without unit bans"};
    ImGui::SetNextItemWidth(170.0f);
    ImGui::Combo("##scbans", &bans, kBans, 3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("Banned unit", unit < 0 ? "Any" : lb_teams_unit_name(unit))) {
        if (ImGui::Selectable("Any", unit < 0)) unit = -1;
        for (int bp = 0; bp < 182; ++bp) {
            const char* nm = lb_teams_unit_name(bp);
            if (!nm || !*nm || !std::strcmp(nm, "?") || !std::strncmp(nm, "unit ", 5)) continue;
            char id[96];
            std::snprintf(id, sizeof id, "%s##scu%d", nm, bp);
            if (ImGui::Selectable(id, unit == bp)) unit = bp;
        }
        ImGui::EndCombo();
    }
    help("Only the matches where this unit was banned.");

    std::string query = "?limit=100";
    if (ruleset) query += "&ruleset=" + sc_url(s.rulesets[(size_t)ruleset - 1]);
    if (bans == 1) query += "&bans=on";
    if (bans == 2) query += "&bans=off";
    if (unit >= 0) query += "&banned=" + std::to_string(unit);
    const bool refresh = ImGui::Button("Refresh");
    if ((refresh || query != asked) && !g_sc_busy.exchange(true)) {
        asked = query;
        {
            std::lock_guard<std::mutex> lk(g_sc_mu);
            g_sc.state = 1;
        }
        const std::string host = g_set.master_host;
        const int port = g_set.master_port;
        std::thread([host, port, query]() { sc_run(host, port, query); }).detach();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_set.master_host);

    if (s.state == 1 || s.state == 0) ImGui::TextDisabled("Asking the master server...");
    if (s.state == 3) ImGui::TextWrapped("%s", s.error.c_str());
    if (s.state == 2 && s.rows.empty()) ImGui::TextDisabled("No matches recorded here yet.");
    if (s.state == 2 && !s.rows.empty() &&
        ImGui::BeginTable("scores", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0.0f, ImGui::GetContentRegionAvail().y - 4.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        for (const char* c : {"Wins", "Losses", "Win %", "Units destroyed", "Units lost",
                              "Buildings destroyed", "Buildings lost", "Bricks"})
            ImGui::TableSetupColumn(c);
        ImGui::TableHeadersRow();
        const float icon = ImGui::GetTextLineHeight() * 1.4f;
        for (size_t i = 0; i < s.rows.size(); ++i) {
            const ScoreRow& r = s.rows[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            hero_image(r.hero, icon);
            if (ImGui::IsItemHovered() && r.hero >= 0)
                ImGui::SetTooltip("Most played: %s", lb_teams_unit_name(r.hero));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", r.wins);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.losses);
            ImGui::TableNextColumn();
            ImGui::Text("%d%%", r.matches ? (int)std::lround(100.0 * r.wins / r.matches) : 0);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.units_killed);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.units_lost);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.buildings_killed);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.buildings_lost);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.bricks);
        }
        ImGui::EndTable();
    }
    tab_end();
}

// docs/431 (SG-0010): the community map archive, one map at a time.
SDL_Renderer* g_maps_ren = nullptr;   // set by lb_overlay_draw, for the thumbnails

void tab_map_archive() {
    if (!tab_begin("Maps")) return;
    guide("Free Play maps from the community map archive and the port's own (beta and "
          "prototype maps). Pick a map to see its picture and facts, Download it into "
          "your mods/maps folder, then Load new maps now. In a lobby, a map you lack "
          "is fetched for you.");
    lb_mapbrowser_draw(g_maps_ren);
    tab_end();
}

const std::vector<MenuHeader>& menu_headers() {
    static const std::vector<MenuHeader> k = {
        {"Settings", {{"General", "General", tab_general},
                      {"Display", "Display", tab_display},
                      {"Controls", "Input", tab_input},
                      {"Music", "Music", tab_music},
                      {"Saves", "Saves", tab_saves}}},
        {"Enhancements", {{"Gameplay", "Enhancements", tab_enhancements},
                          {"Renames", "Renames", tab_renames},
                          {"Team Builder", "Team Builder", tab_team_builder}}},
        {"Balance", {{"Units", "Balance Units", tab_balance_units},
                     {"Heroes", "Balance Heroes", tab_balance_heroes},
                     {"Traits", "Balance Traits", tab_balance_traits},
                     {"Attacks", "Balance Attacks", tab_balance_attacks},
                     {"Spells", "Balance Spells", tab_balance_spells},
                     {"Unit Caps", "Balance Caps", tab_balance_caps},
                     {"Ban Units", "Ban Units", tab_ban_units},
                     {"Ban Maps", "Ban Maps", tab_ban_maps},
                     {"Presets", "Balance Presets", tab_balance_presets}}},
        {"Cheats", {{"Cheats", "Cheats", tab_cheats}}},
        {"Network", {{"Multiplayer", "Multiplayer", tab_multiplayer},
                     {"Scores", "Scores", tab_scores}}},
        {"Maps", {{"Map archive", "Maps", tab_map_archive}}},
        {"Tools", {{"Recordings", "Demo", tab_demo},
                   {"Windows", "Windows", section_windows}}},
    };
    return k;
}

int g_menu_header = 0;
int g_menu_section[8] = {};
bool g_quit_ask = false, g_reset_ask = false;

// A header or sidebar entry: a full-width button, coloured when selected.
bool menu_entry(const char* label, bool selected, float width) {
    const ImVec4 sel(0.16f, 0.42f, 0.86f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? sel : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          selected ? sel : ImVec4(0.16f, 0.42f, 0.86f, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, sel);
    const bool hit = ImGui::Button(label, ImVec2(width, 0));
    ImGui::PopStyleColor(3);
    return hit;
}

bool menu_red_button(const char* label, const char* tip) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.90f, 0.22f, 0.22f, 1.0f));
    const bool hit = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return hit;
}

// The first frame only: LB_PANEL_TAB names the section to open on.
void menu_apply_env_tab() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* want = std::getenv("LB_PANEL_TAB");
    if (!want) return;
    const auto& hs = menu_headers();
    for (int h = 0; h < (int)hs.size(); ++h)
        for (int s = 0; s < (int)hs[h].sections.size(); ++s)
            if (std::strcmp(hs[h].sections[s].id, want) == 0) {
                g_menu_header = h;
                g_menu_section[h] = s;
            }
}

void draw_panels() {
    menu_apply_env_tab();
    const auto& hs = menu_headers();
    if (g_menu_header < 0 || g_menu_header >= (int)hs.size()) g_menu_header = 0;
    const MenuHeader& head = hs[(size_t)g_menu_header];
    int& sec = g_menu_section[g_menu_header];
    if (sec < 0 || sec >= (int)head.sections.size()) sec = 0;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags full = ImGuiWindowFlags_NoDecoration |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("LEGO Battles", &g_visible, full)) {
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // The block: the whole window below 1280x800, else 90% of it, and never
    // wider than 16:9 (SoH's rule).
    const float ww = vp->WorkSize.x, wh = vp->WorkSize.y;
    ImVec2 size(ww, wh);
    if (ww > 1280.0f) size.x = std::min(ww * 0.9f, wh * 1.77f);
    if (wh > 800.0f) size.y = wh * 0.9f;
    ImGui::SetCursorPos(ImVec2((ww - size.x) * 0.5f, (wh - size.y) * 0.5f));
    ImGui::BeginChild("lb_menu_block", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Header: the categories, then Quit / Reset / Close at the right.
    ImGui::SetWindowFontScale(1.35f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    for (int h = 0; h < (int)hs.size(); ++h) {
        if (h) ImGui::SameLine();
        const float w = ImGui::CalcTextSize(hs[h].label).x + 20.0f;
        if (menu_entry(hs[h].label, h == g_menu_header, w)) g_menu_header = h;
    }
    const float bw = ImGui::CalcTextSize("Reset").x + ImGui::CalcTextSize("Quit").x +
                     ImGui::CalcTextSize("Close").x + 60.0f + st.ItemSpacing.x * 2;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), size.x - bw - st.WindowPadding.x));
    if (menu_red_button("Quit", "Close the game")) g_quit_ask = true;
    ImGui::SameLine();
    if (menu_red_button("Reset", "Restart the game from its title screen (Ctrl+R)"))
        g_reset_ask = true;
    ImGui::SameLine();
    if (ImGui::Button("Close")) g_visible = false;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close the menu (Esc)");
    ImGui::PopStyleVar();

    // The rule under the header.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + size.x - st.WindowPadding.x * 2, p.y + 4.0f),
                          IM_COL32(255, 255, 255, 255), 2.0f);
        ImGui::Dummy(ImVec2(0, 4.0f + st.ItemSpacing.y));
    }

    // Sidebar, rule, section.
    const float sideW = size.x > 1600.0f ? size.x * 0.15f : 200.0f;
    const float bodyH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("lb_menu_side", ImVec2(sideW, bodyH));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    for (int s = 0; s < (int)head.sections.size(); ++s)
        if (menu_entry(head.sections[s].label, s == sec, -1.0f)) sec = s;
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + 4.0f, p.y + bodyH), IM_COL32(255, 255, 255, 255),
                          2.0f);
        ImGui::Dummy(ImVec2(4.0f, bodyH));
    }
    ImGui::SameLine();
    ImGui::BeginChild("lb_menu_body", ImVec2(0, bodyH));
    // A control is a column wide, not the whole body (SoH lays its widgets
    // out in columns): sliders the width of a 3440 window are unreadable.
    ImGui::PushItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.6f, 460.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 7.0f));
    g_menu_id = head.sections[(size_t)sec].id;
    head.sections[(size_t)sec].draw();
    g_menu_id = nullptr;
    ImGui::PopStyleVar(2);
    ImGui::PopItemWidth();
    ImGui::EndChild();

    // Confirmations, as SoH's modals.
    if (g_quit_ask) { ImGui::OpenPopup("Quit LEGO Battles?"); g_quit_ask = false; }
    if (g_reset_ask) { ImGui::OpenPopup("Reset the game?"); g_reset_ask = false; }
    if (ImGui::BeginPopupModal("Quit LEGO Battles?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Are you sure you want to quit?");
        if (menu_red_button("Quit", nullptr)) {
            SDL_Event q{};
            q.type = SDL_QUIT;
            SDL_PushEvent(&q);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Reset the game?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Restart from the title screen? Progress since the\n"
                               "game last saved is lost.");
        if (menu_red_button("Reset", nullptr)) {
            lb_request_game_reset();
            g_visible = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace

// ---------------------------------------------------------------------------

// docs/413: one message for the player, over the game, until OK or 20 s. Set
// from the emulation thread (a joiner missing the host's mod map), so it is
// handed over under a lock; the same text again does not restart it.
namespace {
std::mutex g_notice_mu;
std::string g_notice, g_notice_shown;
unsigned long long g_notice_until = 0;
void draw_notice() {
    {
        std::lock_guard<std::mutex> lk(g_notice_mu);
        if (!g_notice.empty()) {
            if (g_notice != g_notice_shown) {
                g_notice_shown = g_notice;
                g_notice_until = SDL_GetTicks64() + 20000ull;
            }
            g_notice.clear();
        }
    }
    if (g_notice_shown.empty() || SDL_GetTicks64() > g_notice_until) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 40.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("##lb_notice", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(g_notice_shown.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK")) g_notice_until = 0;
    }
    ImGui::End();
}
}  // namespace

extern "C" void lb_overlay_notice(const char* text) {
    if (!text || !*text) return;
    {
        std::lock_guard<std::mutex> lk(g_notice_mu);
        g_notice = text;
    }
    lb_sfx_notify();   // docs/427
}

extern "C" int lb_setting_notify_sound(void) { return g_set.notify_sound; }
extern "C" int lb_setting_notify_volume(void) { return g_set.notify_volume; }

extern "C" void lb_overlay_draw(SDL_Window* window, SDL_Renderer* renderer) {
    if (std::getenv("LB_DIAG")) {
        static int t2 = 0;
        if ((t2++ % 60) == 0)
            std::fprintf(stderr, "[lb_diag] draw win=%p ren=%p enhanced=%d wide=%d top_vis=%d top=%dx%d\n",
                         (void*)window, (void*)renderer, (int)g_set.enhanced,
                         (int)g_wide_field, (int)g_top_visible, g_top_w, g_top_h);
    }
    if (!window || !renderer) return;
    g_maps_ren = renderer;   // docs/431
    // Before any panel logic and independent of every panel's visibility, so
    // the band audit's reference dump lands whether or not anything is open.
    // See the note at its definition and docs/291.
    lb_field_bench_tick();  /* legobattles-decomp: docs/325 phase 1 */
    lb_map_dump_tick();
    lb_sfx_tick();   // docs/427: decode the chosen notification sound
    lb_matchpic_tick();   // docs/428: LB_MATCHPIC_TEST only
    upd_startup_tick();   // docs/430: the quiet update check
    lb_discord_tick();    // docs/430: Discord Rich Presence

    if (g_ready) save_cache_tick();   // docs/388: the save's recent armies

    // Persist when something actually changed, not every frame: this writes
    // a file, and the values only move when a control does.
    {
        static size_t last_hash = 0;
        static int tick = 0;
        if (g_ready && ++tick >= 120) {
            tick = 0;
            size_t h = 1469598103934665603ull;
            const unsigned char* p = (const unsigned char*)&g_set;
            for (size_t i = 0; i < sizeof g_set; ++i) {
                h ^= p[i]; h *= 1099511628211ull;
            }
            // g_wide_field is a LEFTOVER TERM and is kept only so this hash
            // keeps its documented meaning of "everything the autosave
            // covers". It is no longer persisted or loaded -- see the note in
            // Settings -- and after startup it never changes: draw_wide_field
            // sets it once from LB_WIDE_FIELD and the checkbox that used to
            // own it was removed. So it can neither trigger a save nor
            // suppress one; it is inert, not a persisted field this misses.
            h ^= (size_t)(g_wide_field ? 1u : 0u); h *= 1099511628211ull;
            // docs/404: the Balance tab's values live in lb_balance.cpp.
            h ^= (size_t)lb_balance_custom_generation(); h *= 1099511628211ull;
            if (last_hash && h != last_hash) lb_settings_save();
            last_hash = h;
        }
    }

    if (!g_ready) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        // ImGui keeps a pointer, so the string has to outlive this call.
        static std::string ini = settings_path("imgui.ini");
        ImGui::GetIO().IniFilename = ini.c_str();
        lb_settings_load();
        lb_update_cleanup();   // docs/435: the files the last update replaced
        ImGui::StyleColorsDark();
        if (!ImGui_ImplSDL2_InitForSDLRenderer(window, renderer)) {
            std::fprintf(stderr, "[lb_overlay] SDL2 backend init failed\n");
            ImGui::DestroyContext();
            return;
        }
        if (!ImGui_ImplSDLRenderer2_Init(renderer)) {
            std::fprintf(stderr, "[lb_overlay] SDLRenderer2 backend init failed\n");
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        g_ready = true;
        // docs/396: text events, for typing on the game's keyboards (SDL2
        // starts them on the desktop already; this does not depend on it).
        SDL_StartTextInput();
        std::fprintf(stderr, "[lb_overlay] ready (F1 or Esc toggles)\n");

        // Prove the binding table gives the port's default layout. The overlay
        // REPLACES frontend.cpp's key_bit(), so if the table were wrong the
        // keyboard would simply stop working -- and no headless test can
        // press a key, because SDL input needs a real window. Checking the
        // mapping directly is the next best thing. (docs/396: the layout is
        // frontend.cpp's with X/Y/R on C/V/E and W/A/S/D as the D-pad; a
        // player's own saved bindings report as a mismatch, which is fine.)
        bindings_init_once();
        {
            struct { int sc; uint16_t bit; } expect[] = {
                {SDL_SCANCODE_Z, 1u << 0},  {SDL_SCANCODE_X, 1u << 1},
                {SDL_SCANCODE_BACKSPACE, 1u << 2}, {SDL_SCANCODE_RETURN, 1u << 3},
                {SDL_SCANCODE_RIGHT, 1u << 4}, {SDL_SCANCODE_LEFT, 1u << 5},
                {SDL_SCANCODE_UP, 1u << 6}, {SDL_SCANCODE_DOWN, 1u << 7},
                {SDL_SCANCODE_E, 1u << 8},  {SDL_SCANCODE_Q, 1u << 9},
                {SDL_SCANCODE_C, 1u << 10}, {SDL_SCANCODE_V, 1u << 11},
                {SDL_SCANCODE_W, 1u << 6},  {SDL_SCANCODE_A, 1u << 5},
                {SDL_SCANCODE_S, 1u << 7},  {SDL_SCANCODE_D, 1u << 4},
            };
            int bad = 0;
            for (const auto& e : expect)
                if ((uint16_t)lb_binding_key_bit(e.sc) != e.bit) ++bad;
            std::fprintf(stderr,
                         "[lb_overlay] key bindings: %d/%d match the shipped "
                         "layout%s\n",
                         (int)(sizeof(expect) / sizeof(expect[0])) - bad,
                         (int)(sizeof(expect) / sizeof(expect[0])),
                         bad ? "  <-- MISMATCH" : "");
        }

        // docs/424: LB_MASTER=host:port, a test aid -- this run's master
        // server alone (a local `wrangler dev`, say), never the public ones.
        if (const char* x = std::getenv("LB_MASTER")) {
            if (const char* c = std::strchr(x, ':')) {
                std::snprintf(g_set.master_host, sizeof g_set.master_host, "%.*s",
                              (int)(c - x), x);
                g_set.master_port = std::atoi(c + 1);
                g_set.master_both = false;
                g_set.master_https = false;
            }
        }
        // docs/419: LB_EXTRA_SERVER is in place before the self-test runs.
        if (const char* x = std::getenv("LB_EXTRA_SERVER")) {
            if (const char* c = std::strchr(x, ':')) {
                char h[128];
                std::snprintf(h, sizeof h, "%.*s", (int)(c - x), x);
                lb_net_set_extra_server(h, std::atoi(c + 1));
            }
        }
        lb_net_selftest_if_asked();
        lb_net_apply_env_peer();
    }

    // Keep an advertised lobby alive. The server drops it after 90 s, and the
    // overlay is already called every frame, so this is cheaper than a timer
    // thread; the call itself returns immediately unless a beat is due.
    // docs/419: Both lists on and browses king.legobattles.win as well (the
    // Multiplayer tab's second known server), every frame, panel open or not.
    // LB_EXTRA_SERVER=host:port stands in for it (a test aid: two local
    // servers instead of the public ones).
    static const char* extra_env = std::getenv("LB_EXTRA_SERVER");
    if (extra_env && std::strchr(extra_env, ':')) {
        char h[128];
        std::snprintf(h, sizeof h, "%.*s", (int)(std::strchr(extra_env, ':') - extra_env),
                      extra_env);
        lb_net_set_extra_server(h, std::atoi(std::strchr(extra_env, ':') + 1));
    } else if (g_set.master_both) {
        lb_net_set_extra_server("king.legobattles.win", 28555);
    } else {
        lb_net_set_extra_server("", 0);
    }
    lb_net_host_tick((long long)SDL_GetTicks64());
    // The game's own Host and Join screens drive the
    // master server. Every frame, whatever panel is open, for the same reason
    // as the heartbeat above; it returns at once unless the game's network
    // state calls for something.
    lb_net_auto_tick(g_set.master_host, g_set.master_port, mp_player_name(),
                     g_set.auto_advertise ? 1 : 0, g_set.auto_browse ? 1 : 0,
                     (long long)SDL_GetTicks64());

    // Rescale the whole style when asked. ImGui sizes everything in pixels, so
    // a panel authored at 1x is unreadable once the window is scaled up. The
    // style is rebuilt from a pristine copy each time rather than multiplied
    // repeatedly, which would compound rounding on every change.
    if (g_ui_scale_dirty) {
        g_ui_scale_dirty = false;
        static ImGuiStyle base_style;
        static bool base_saved = false;
        if (!base_saved) { base_style = ImGui::GetStyle(); base_saved = true; }
        ImGuiStyle scaled = base_style;
        scaled.ScaleAllSizes(g_ui_scale);
        ImGui::GetStyle() = scaled;
        ImGui::GetIO().FontGlobalScale = g_ui_scale;
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    draw_wide_field(renderer);
    draw_top_screen(renderer);
    draw_battlefield(renderer);
    draw_notice();   // docs/413
    // SG-0016: while Skip the intro screens fast-forwards the boot logos (they
    // ignore input and run on the game's own timers, so the boot is only run
    // faster), cover the window, so the player sees a short loading screen
    // and then the title, not the logos racing past.
    if (lb_boot_skip_wanted()) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 sz = ImGui::GetIO().DisplaySize;
        fg->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(0, 0, 0, 255));
        const char* text = "Loading...";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        fg->AddText(ImVec2((sz.x - ts.x) * 0.5f, (sz.y - ts.y) * 0.5f),
                    IM_COL32(200, 200, 200, 255), text);
    }
    lb_swear_selftest_if_asked();   // docs/415
    bans_roundtrip_if_asked();      // docs/419
    lb_chat_draw(renderer);         // docs/415
    // docs/384: the tabs draw heads with this renderer, and the enemy team
    // and the ban list reach lb_teams.cpp whether or not the panel is open.
    g_team_renderer = renderer;
    {
        // docs/410: LB_ENEMY_TEAM=<9 blueprints, the slots in order> sets the
        // Custom opponent's army once, for a headless test. Test aid only.
        static bool enemy_env = false;
        if (!enemy_env) {
            enemy_env = true;
            if (const char* p = std::getenv("LB_ENEMY_TEAM")) {
                int v[LB_TEAM_SLOTS], k = 0, used = 0;
                while (k < LB_TEAM_SLOTS && std::sscanf(p, "%d%n", &v[k], &used) == 1) {
                    p += used;
                    if (*p == ',') ++p;
                    ++k;
                }
                if (k == LB_TEAM_SLOTS)
                    for (int i = 0; i < LB_TEAM_SLOTS; ++i) g_set.enemy_team[i] = v[i];
            }
        }
        const LbTeam enemy = team_of(g_set.enemy_team);
        lb_teams_set_enemy(&enemy);
        unsigned char bits[24];
        bans_bits(bits);
        lb_teams_set_bans(bits, 24);
        maps_sync_local();   // docs/429
    }
    if (g_visible) draw_panels();
    ImGui::Render();
    ImDrawData* draw = ImGui::GetDrawData();

    // Prove once, in the log, that geometry actually reached the renderer.
    // "The context initialised" and "the panel is on screen" are different
    // claims, and only the second one matters to a player.
    if (!g_logged_draw && draw && draw->TotalVtxCount > 0) {
        g_logged_draw = true;
        std::fprintf(stderr,
                     "[lb_overlay] drawing: %d vertices, %d indices, %d list(s)"
                     " at %.0fx%.0f\n",
                     draw->TotalVtxCount, draw->TotalIdxCount, draw->CmdListsCount,
                     draw->DisplaySize.x, draw->DisplaySize.y);
    }

    // docs/396: IN WINDOW PIXELS, WHATEVER LOGICAL SIZE THE FRONTEND SET.
    // DS Vanilla presents through SDL_RenderSetLogicalSize, which also scaled
    // this draw: ImGui laid itself out for the whole window and was then drawn
    // at the logical scale (2x on a 1920x1080 screen), so the panel ran off
    // the window and the control to turn Enhanced back on could be out of
    // reach -- a player had to delete settings.json. The logical size is
    // lifted for the overlay and put back after it.
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(renderer, &lw, &lh);
    if (lw > 0 && lh > 0) SDL_RenderSetLogicalSize(renderer, 0, 0);
    g_overlay_renderer = renderer;
    ImGui_ImplSDLRenderer2_RenderDrawData(draw, renderer);
    if (lw > 0 && lh > 0) SDL_RenderSetLogicalSize(renderer, lw, lh);
}

// ---- docs/383: BATTLE SHORTCUTS ------------------------------------------
//
// Space and right-click are bound to quick deselect by default, Mouse 5 opens
// the blue brick menu and Mouse 4 the red/yellow/white one. The first version
// pressed the button on the touchscreen; a shortcut must instead invoke the
// game's own function, not tap the screen.
//
// So a shortcut is only a REQUEST here. lb_patches.cpp runs it in the guest
// (view_update_override): the cartridge's own callback for that button, with
// the arguments its widget manager passes for a click, gated the way its pen
// path gates a click. The touchscreen is not involved: a shortcut works while
// the player is holding a press or dragging a box, and it cannot fall through
// to the field. A button that is not shown (nothing selected, no brick of that
// kind) makes it do nothing.
//
// docs/385: with the Space Criminal Leader selected, 4 on the number row or
// keypad arms Hotwire; 2 with a builder selected picks from its list -- the
// second brick's list, whichever colour the game shows. 1-9 ask for item N of
// that list (kQuickItem + N);
// lb_patches.cpp opens the list with the brick's callback if it is closed and
// then runs the item's, as a player's two taps would.
extern "C" void lb_quick_request(int action);   /* src/lb_patches.cpp */

namespace {
enum QuickAction { kQuickNone = -1, kQuickDeselect = 0, kQuickBlue = 1,
                   kQuickSecond = 2, kQuickItem = 10 };

// 1..9 for the number row and the keypad's digits, else 0.
int quick_digit(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return 1 + (int)(sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return 1 + (int)(sc - SDL_SCANCODE_KP_1);
    return 0;
}

bool quick_in_battle() {
    int cx = 0, cy = 0;
    return lb_field_camera(&cx, &cy) != 0;
}

// Returns true when the event was a shortcut (and so is consumed).
bool quick_event(const SDL_Event* event, const ImGuiIO& io) {
    if (!g_set.quick_keys || !lb_setting_enhanced()) return false;
    int action = kQuickNone;
    if (event->type == SDL_KEYDOWN && event->key.repeat == 0 &&
        event->key.keysym.scancode == SDL_SCANCODE_SPACE &&
        !io.WantCaptureKeyboard &&
        lb_binding_key_bit(SDL_SCANCODE_SPACE) == 0) {   // not a DS button
        action = kQuickDeselect;
    } else if (event->type == SDL_KEYDOWN && event->key.repeat == 0 &&
               quick_digit(event->key.keysym.scancode) &&
               !io.WantCaptureKeyboard &&
               lb_binding_key_bit(event->key.keysym.scancode) == 0) {
        action = kQuickItem + quick_digit(event->key.keysym.scancode);
    } else if ((event->type == SDL_MOUSEBUTTONDOWN ||
                event->type == SDL_MOUSEBUTTONUP) && !io.WantCaptureMouse) {
        const int b = event->button.button;
        if (b == SDL_BUTTON_RIGHT) action = kQuickDeselect;
        else if (b == SDL_BUTTON_X2) action = kQuickBlue;     // Mouse 5
        else if (b == SDL_BUTTON_X1) action = kQuickSecond;   // Mouse 4
        if (action != kQuickNone && event->type == SDL_MOUSEBUTTONUP)
            return true;   // the release of a shortcut button: nothing more
    }
    if (action == kQuickNone) return false;
    if (quick_in_battle()) lb_quick_request(action);
    return true;
}
}  // namespace

extern "C" bool lb_overlay_event(const SDL_Event* event) {
    if (!g_ready || !event) return false;

    // docs/415: the text event of the key that opened the chat comes after its
    // key event; it is not part of the message.
    if (event->type == SDL_TEXTINPUT && g_chat_eat_text) {
        g_chat_eat_text = false;
        return true;
    }
    if (event->type == SDL_KEYDOWN && g_chat_eat_text &&
        (int)event->key.keysym.scancode != g_set.chat_key)
        g_chat_eat_text = false;

    if (event->type == SDL_KEYDOWN && event->key.repeat == 0) {
        const SDL_Keycode k = event->key.keysym.sym;

        // A rebind in progress swallows the next key, INCLUDING F1, so that
        // every key on the keyboard can actually be bound. Esc cancels rather
        // than binding, because a player needs a way out that is not itself a
        // binding.
        if (g_rebinding >= 0 && g_rebinding < kActionCount) {
            if (k != SDLK_ESCAPE) {
                const int sc = (int)event->key.keysym.scancode;
                for (int i = 0; i < kActionCount; ++i)
                    if (i != g_rebinding && g_binding[i] == sc)
                        g_binding[i] = 0;   // one key cannot drive two buttons
                g_binding[g_rebinding] = sc;
            }
            g_rebinding = -1;
            return true;
        }
        // SG-0012: the swap key's rebind, the same way.
        if (g_rebinding_swap) {
            if (k != SDLK_ESCAPE) {
                const int sc = (int)event->key.keysym.scancode;
                for (int i = 0; i < kActionCount; ++i)
                    if (g_binding[i] == sc) g_binding[i] = 0;   // one key, one job
                g_set.swap_key = sc;
            }
            g_rebinding_swap = false;
            return true;
        }
        // SG-0012: the swap key calls the port's own swap, unless a text field
        // has the keyboard.
        if (g_set.swap_key && (int)event->key.keysym.scancode == g_set.swap_key &&
            !ImGui::GetIO().WantTextInput) {
            lb_request_swap_screens();
            return true;
        }
        // docs/415: the chat key's rebind, the same way.
        if (g_rebinding_chat) {
            if (k != SDLK_ESCAPE) g_set.chat_key = (int)event->key.keysym.scancode;
            g_rebinding_chat = false;
            return true;
        }
        // docs/415: Esc closes an open chat box before it opens the menu.
        if (k == SDLK_ESCAPE && lb_chat_is_open()) {
            lb_chat_close();
            return true;
        }
        // docs/415: the chat key opens the chat box, online only; offline, or
        // while a text field has the keyboard, it is an ordinary key.
        if ((int)event->key.keysym.scancode == g_set.chat_key && lb_chat_online() &&
            !lb_chat_is_open() && !ImGui::GetIO().WantTextInput) {
            lb_chat_open();
            g_chat_eat_text = true;   // the "t" this key types is not the message
            return true;
        }

        if (k == SDLK_F1 || k == SDLK_ESCAPE) {
            g_visible = !g_visible;
            return true;
        }
        // docs/396: Ctrl+R resets the game, as SoH's does.
        if (k == SDLK_r && (event->key.keysym.mod & KMOD_CTRL)) {
            lb_request_game_reset();
            return true;
        }
    }

    if (event->type == SDL_CONTROLLERBUTTONDOWN &&
        g_rebinding_pad >= 0 && g_rebinding_pad < kActionCount) {
        const int b = (int)event->cbutton.button;
        for (int i = 0; i < kActionCount; ++i)
            if (i != g_rebinding_pad && g_pad_binding[i] == b)
                g_pad_binding[i] = -1;   // one button cannot drive two DS buttons
        g_pad_binding[g_rebinding_pad] = b;
        g_rebinding_pad = -1;
        return true;
    }

    // docs/396: TYPING ON THE GAME'S OWN KEYBOARDS. While the profile-name or
    // cheat-code keyboard is the screen, typed characters press its keys --
    // through the keys' own callback, in lb_patches.cpp -- and the letter,
    // digit, Space, Backspace and Enter keys are not DS buttons meanwhile, so
    // typing "wasd" does not also walk the cursor. Not while a panel field
    // has the keyboard.
    if (lb_keyboard_screen_active() &&
        !(g_visible && ImGui::GetIO().WantCaptureKeyboard)) {
        if (event->type == SDL_TEXTINPUT) {
            for (const char* p = event->text.text; *p; ++p)
                if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7F)
                    lb_keyboard_type(*p);
            return true;
        }
        if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
            const SDL_Scancode sc = event->key.keysym.scancode;
            const bool typing =
                (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_0) ||
                (sc >= SDL_SCANCODE_MINUS && sc <= SDL_SCANCODE_SLASH) ||
                sc == SDL_SCANCODE_SPACE || sc == SDL_SCANCODE_BACKSPACE ||
                sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER;
            if (typing) {
                if (event->type == SDL_KEYDOWN) {
                    if (sc == SDL_SCANCODE_BACKSPACE) lb_keyboard_type('\b');
                    else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER)
                        lb_keyboard_type('\r');
                }
                return true;
            }
        }
    }

    // docs/396: the overlay is drawn in window pixels (lb_overlay_draw), but
    // under a logical size (DS Vanilla) SDL hands mouse events over in
    // LOGICAL coordinates -- half the window's on a 1920x1080 screen -- so
    // ImGui's idea of the cursor was elsewhere. Its copy of the event goes back
    // to window pixels; the game's is untouched.
    SDL_Event lb_ev = *event;
    if (g_overlay_renderer) {
        int lw = 0, lh = 0;
        SDL_RenderGetLogicalSize(g_overlay_renderer, &lw, &lh);
        if (lw > 0 && lh > 0) {
            auto to_window = [](int* x, int* y) {
                int wx = *x, wy = *y;
                SDL_RenderLogicalToWindow(g_overlay_renderer, (float)*x, (float)*y,
                                          &wx, &wy);
                *x = wx;
                *y = wy;
            };
            if (lb_ev.type == SDL_MOUSEMOTION)
                to_window(&lb_ev.motion.x, &lb_ev.motion.y);
            else if (lb_ev.type == SDL_MOUSEBUTTONDOWN || lb_ev.type == SDL_MOUSEBUTTONUP)
                to_window(&lb_ev.button.x, &lb_ev.button.y);
        }
    }
    ImGui_ImplSDL2_ProcessEvent(&lb_ev);

    const ImGuiIO& io = ImGui::GetIO();
    if (quick_event(event, io)) return true;   // docs/383
    switch (event->type) {
        case SDL_MOUSEWHEEL:
            // THE WHEEL ZOOMS, which is what a PC RTS does and what this port
            // exists to become. It is bound to the SAME `pixel_scale` the
            // Magnification slider writes, so the two cannot disagree and the
            // value persists with the rest of the settings.
            //
            // Nothing else had to be built for this: frontend.cpp already
            // derives the guest surface from the scale every frame
            // (`win_w / scale`), and the 3D frustum follows from RenderWidth on
            // both axes. Zooming out is bounded by the world, not by this
            // clamp -- the pixel-scale floor beside lb_world_extent() stops it
            // once the whole map is in shot.
            //
            // The panel gets the wheel first: scrolling a settings list must
            // not also zoom the battlefield behind it.
            if (io.WantCaptureMouse) return true;
            if (event->wheel.y != 0) {
                const float step = event->wheel.y > 0 ? 0.25f : -0.25f;
                float s = g_set.pixel_scale + step;
                if (s < 1.0f) s = 1.0f;
                if (s > 8.0f) s = 8.0f;   // the slider's own range
                g_set.pixel_scale = s;
                return true;
            }
            return false;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION:
            return io.WantCaptureMouse;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_TEXTINPUT:
            return io.WantCaptureKeyboard;
        default:
            return false;
    }
}

extern "C" bool lb_overlay_wants_mouse(void) {
    return g_ready && ImGui::GetIO().WantCaptureMouse;
}

// DELIBERATELY UNUSED, and unlike lb_overlay_wants_mouse above it is not a
// second gate anybody needs to add. Written down because "an exported entry
// point with zero call sites" is exactly the shape that hid the missing
// lb_overlay_shutdown caller, so the next sweep that finds this one should be
// able to stop here instead of re-deriving it.
//
// The keyboard is already fully captured, by event consumption rather than by
// a query:
//
//   * lb_overlay_event returns io.WantCaptureKeyboard for SDL_KEYDOWN,
//     SDL_KEYUP and SDL_TEXTINPUT (the switch above), and
//   * apply_hooks.py inserts `if (lb_overlay_event(&event)) continue;` as the
//     FIRST statement inside the frontend's `while (SDL_PollEvent(&event))`
//     (port/runner/tree/runner/src/frontend.cpp:2034), so a captured key never
//     reaches any of the runner's own handling.
//
// What makes that sufficient for the keyboard and NOT for the mouse is that
// the guest's key feed is purely event-driven, with no polled second route:
//
//   * frontend.cpp's key_bit() -- which this file replaces via
//     lb_binding_key_bit -- has exactly two call sites, frontend.cpp:2063 and
//     :2078, both reading event.key.keysym.scancode inside that same loop, and
//   * there is no SDL_GetKeyboardState or SDL_GetModState anywhere in
//     port/runner/tree/runner/src (grepped, zero hits).
//
// The mouse needed the extra gate because set_touch_from_mouse is a FUNCTION
// three call sites and the self-test paths reach, which is why apply_hooks.py
// calls it "belt and braces". There is no keyboard equivalent of that
// function, so there is nothing for this accessor to guard.
//
// Kept rather than deleted only because it is declared in lb_overlay.h next to
// lb_overlay_wants_mouse and costs nothing; if the header is ever trimmed,
// this is a safe thing to take with it.
extern "C" bool lb_overlay_wants_keyboard(void) {
    return g_ready && ImGui::GetIO().WantCaptureKeyboard;
}

extern "C" void lb_overlay_set_top_pixels(const void* pixels, int width,
                                          int height) {
    g_top_pixels = pixels;
    // LB_DIAG=1 reports what the frontend actually hands over, because the
    // debug server and the window disagreed about whether the top screen
    // was black and only one of them can be right.
    if (std::getenv("LB_DIAG")) {
        static int t = 0;
        if ((t++ % 60) == 0) {
            unsigned long long sum = 0;
            const uint32_t* p = (const uint32_t*)pixels;
            const int n = width * height;
            if (p) for (int i = 0; i < n; i += 7)
                sum += (p[i] & 0xFF) + ((p[i] >> 8) & 0xFF) + ((p[i] >> 16) & 0xFF);
            std::fprintf(stderr, "[lb_diag] top_pixels=%p %dx%d mean=%.1f\n",
                         pixels, width, height,
                         p && n ? (double)sum / (n / 7 * 3) : -1.0);
        }
    }
    g_top_w = width;
    g_top_h = height;
}

namespace {
int g_field_x = 0, g_field_y = 0, g_field_w = 0, g_field_h = 0;
int g_field_guest_w = 256;
int g_field_guest_h = 192;
}  // namespace

extern "C" void lb_overlay_set_field_rect(int x, int y, int w, int h,
                                          int guest_width,
                                          int guest_height) {
    g_field_x = x;
    g_field_y = y;
    g_field_w = w;
    g_field_h = h;
    g_field_guest_w = guest_width > 0 ? guest_width : 256;
    g_field_guest_h = guest_height > 0 ? guest_height : 192;
}

extern "C" int lb_overlay_field_rect(int* x, int* y, int* w, int* h,
                                     int* guest_width, int* guest_height) {
    if (g_field_w <= 0 || g_field_h <= 0) return 0;
    if (x) *x = g_field_x;
    if (y) *y = g_field_y;
    if (w) *w = g_field_w;
    if (h) *h = g_field_h;
    if (guest_width) *guest_width = g_field_guest_w;
    if (guest_height) *guest_height = g_field_guest_h;
    return 1;
}

namespace {
int g_tr_x = 0, g_tr_y = 0, g_tr_w = 0, g_tr_h = 0;   // DS Vanilla's touch screen
}  // namespace

extern "C" void lb_overlay_vanilla_layout(int win_w, int win_h, int w0, int w1,
                                          int* top, int* bot) {
    auto clampf = [](float v) { return v < 0.25f ? 0.25f : v > 2.0f ? 2.0f : v; };
    const float ts = clampf(g_set.top_scale), bs = clampf(g_set.bottom_scale);
    if (w0 < 1) w0 = 256;
    if (w1 < 1) w1 = 256;
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;
    const float tw = (float)w0 * ts, bw = (float)w1 * bs;
    const float th = 192.0f * ts, bh = 192.0f * bs;
    float f = std::min((float)win_w / std::max(tw, bw), (float)win_h / (th + bh));
    const bool equal = std::fabs(ts - 1.0f) < 0.001f && std::fabs(bs - 1.0f) < 0.001f;
    if (equal && f >= 1.0f) f = std::floor(f);   // the DS's own pixels, crisp
    const int tW = std::max(1, (int)(tw * f + 0.5f)), tH = std::max(1, (int)(th * f + 0.5f));
    const int bW = std::max(1, (int)(bw * f + 0.5f)), bH = std::max(1, (int)(bh * f + 0.5f));
    const int y0 = (win_h - (tH + bH)) / 2;
    top[0] = (win_w - tW) / 2; top[1] = y0;      top[2] = tW; top[3] = tH;
    bot[0] = (win_w - bW) / 2; bot[1] = y0 + tH; bot[2] = bW; bot[3] = bH;
    // The touch panel is the screen showing the cartridge's bottom engine: the
    // physical top one while swapped.
    const int* t = lb_screens_swapped() ? top : bot;
    g_tr_x = t[0]; g_tr_y = t[1]; g_tr_w = t[2]; g_tr_h = t[3];
    {   // say it when the layout changes, so a run can be graded by numbers
        static int last[10] = {};
        const int now[10] = {win_w, win_h, top[0], top[1], top[2], top[3], bot[0],
                             bot[1], bot[2], bot[3]};
        if (std::memcmp(last, now, sizeof now) != 0) {
            std::memcpy(last, now, sizeof now);
            std::fprintf(stderr, "[lb_vanilla] window %dx%d: top %d,%d %dx%d, bottom %d,%d "
                         "%dx%d, touch on the %s\n", win_w, win_h, top[0], top[1], top[2],
                         top[3], bot[0], bot[1], bot[2], bot[3],
                         lb_screens_swapped() ? "top" : "bottom");
            std::fflush(stderr);
        }
    }
}

extern "C" int lb_overlay_touch_rect(int* x, int* y, int* w, int* h) {
    if (g_tr_w <= 0 || g_tr_h <= 0) return 0;
    if (x) *x = g_tr_x;
    if (y) *y = g_tr_y;
    if (w) *w = g_tr_w;
    if (h) *h = g_tr_h;
    return 1;
}

extern "C" int lb_overlay_mouse_in_touch_area(int x, int y, int band_left,
                                              int separate) {
    // 256x192 is the DS panel; named here rather than including the frontend's
    // constants, which would drag SDL headers into every user of this one.
    const int kW = 256, kH = 192;
    if (lb_setting_enhanced() && g_field_w > 0 && g_field_h > 0) {
        return x >= g_field_x && x < g_field_x + g_field_w &&
               y >= g_field_y && y < g_field_y + g_field_h;
    }
    if (!lb_setting_enhanced() && !separate && g_tr_w > 0 && g_tr_h > 0)   // BR-0010
        return x >= g_tr_x && x < g_tr_x + g_tr_w && y >= g_tr_y && y < g_tr_y + g_tr_h;
    return x >= band_left && x < band_left + kW && (separate || y >= kH);
}

extern "C" int lb_binding_key_bit(int scancode) {
    bindings_init_once();
    if (scancode == 0) return 0;
    for (int i = 0; i < kActionCount; ++i)
        if (g_binding[i] == scancode) return (int)kActions[i].bit;
    // docs/396: W/A/S/D are the D-pad too -- the camera keys, as the arrows --
    // unless the player bound one of them to a button above.
    switch (scancode) {
        case SDL_SCANCODE_W: return 1 << 6;   // Up
        case SDL_SCANCODE_A: return 1 << 5;   // Left
        case SDL_SCANCODE_S: return 1 << 7;   // Down
        case SDL_SCANCODE_D: return 1 << 4;   // Right
        default: return 0;
    }
}

// SG-0012: the port's own screen swap. It only flips a flag that the engine/
// screen routing (gpu2d.cpp, apply_hooks 0e2) reads when it files a scanline
// and when the compositor maps a screen back to its engine.
extern "C" void lb_request_swap_screens(void) {
    if (g_set.enhanced) {   // SG-0012: Enhanced has no second screen to swap with
        std::fprintf(stderr, "[lb_swap] not in Enhanced; use DS Vanilla\n");
        std::fflush(stderr);
        return;
    }
    const int now = g_screens_swapped.load() ? 0 : 1;
    g_screens_swapped.store(now);
    std::fprintf(stderr, "[lb_swap] screens %s\n", now ? "swapped" : "back to normal");
    std::fflush(stderr);
}
extern "C" int lb_screens_swapped(void) {
    static const bool env_once = [] {   // LB_SWAP=1: start swapped (a test aid)
        const char* v = std::getenv("LB_SWAP");
        if (v && std::atoi(v)) g_screens_swapped.store(1);
        return true;
    }();
    (void)env_once;
    return (!g_set.enhanced && g_screens_swapped.load()) ? 1 : 0;
}

// SG-0016: this machine's "Skip the intro screens" (LB_SKIP_INTRO=0|1 forces it).
extern "C" int lb_setting_skip_intro(void) {
    static int forced = -1;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* v = std::getenv("LB_SKIP_INTRO")) forced = std::atoi(v) ? 1 : 0;
    }
    return forced >= 0 ? forced : (g_set.skip_intro ? 1 : 0);
}

// docs/396: the menu's Reset. A demo being replayed owns the game, so it is
// refused then, as the battle shortcuts are.
static std::atomic<int> g_reset_req{0};
extern "C" void lb_request_game_reset(void) {
    if (lb_demo_playing()) return;
    g_reset_req.store(1);
}
extern "C" int lb_take_game_reset(void) {
    // LB_TEST_RESET_AT=<seconds>: the menu's Reset, pressed once at that
    // many seconds after start. Test aid only.
    static const char* at = std::getenv("LB_TEST_RESET_AT");
    static bool fired = false;
    if (at && !fired && SDL_GetTicks64() >= (unsigned long long)std::atoi(at) * 1000ull) {
        fired = true;
        std::fprintf(stderr, "[lb_reset] test aid: Reset pressed\n");
        lb_request_game_reset();
    }
    // LB_TEST_RESET_JOINED=<seconds>: the same Reset, that long after this
    // machine has joined a lobby (BR-0014's repro: a joiner that resets and
    // joins again). Test aid only.
    static const char* rj = std::getenv("LB_TEST_RESET_JOINED");
    static unsigned long long joined_at = 0;
    static bool rj_fired = false;
    if (rj && !rj_fired) {
        const int ns = lb_net_guest_state();
        const bool joined = ns == 5;   // a client in the host's lobby
        if (!joined) {
            joined_at = 0;
        } else {
            if (!joined_at) joined_at = SDL_GetTicks64();
            if (SDL_GetTicks64() - joined_at >= (unsigned long long)std::atoi(rj) * 1000ull) {
                rj_fired = true;
                std::fprintf(stderr, "[lb_reset] test aid: Reset pressed in the lobby\n");
                std::fflush(stderr);
                lb_request_game_reset();
            }
        }
    }
    // LB_TEST_QUIT_AT=<seconds>: this process ends then (a host that goes
    // away, for a joiner's leave path). Test aid only.
    static const char* quit_at = std::getenv("LB_TEST_QUIT_AT");
    if (quit_at && SDL_GetTicks64() >= (unsigned long long)std::atoi(quit_at) * 1000ull) {
        std::fprintf(stderr, "[lb_test] quitting now (LB_TEST_QUIT_AT)\n");
        std::fflush(stderr);
        std::_Exit(0);
    }
    return g_reset_req.exchange(0);
}

// docs/396: the multiplayer name lb_patches.cpp writes into the game's
// lobby copies -- the typed one, when "Use the save's profile name" is off;
// 0 (nothing to write) when it is on, so the profile's own name stands.
extern "C" int lb_setting_mp_name(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (g_set.name_from_save || !g_set.player_name[0]) return 0;
    std::snprintf(out, (size_t)cap, "%s", g_set.player_name);
    return (int)std::strlen(out);
}

extern "C" int lb_binding_pad_bit(int button) {
    bindings_init_once();
    if (button < 0) return 0;
    for (int i = 0; i < kActionCount; ++i)
        if (g_pad_binding[i] == button) return (int)kActions[i].bit;
    return 0;
}

extern "C" int lb_setting_balance_patch(void) {
    return g_set.balance_patch;
}

// lb_setting_fit_all and its LB_FIT environment variable are REMOVED.
//
// It survived as an accessor no code called, carrying a comment that said it
// was "kept only so LB_FIT can still force the old behaviour for a comparison
// capture". With no caller, LB_FIT forced nothing: the comment described a
// capability that did not exist, which is worse than no knob at all -- a reader
// setting LB_FIT would have seen no change and had no way to tell why.
//
// Adaptive window size is ONE toggle by design, and the
// reason the choice went away is still true: the surface is sized to the
// window's own aspect (docs/215), so fitting and filling produce the same
// rectangle. lb_setting_enhanced is that one toggle.

extern "C" int lb_setting_enhanced(void) {
    return g_set.enhanced ? 1 : 0;
}

// docs/405: UPnP when hosting.
extern "C" int lb_setting_upnp(void) {
    return g_set.upnp ? 1 : 0;
}

// docs/398: number keys pick from the blue brick's list while it is open.
extern "C" int lb_setting_quick_blue(void) {
    return g_set.quick_keys_blue ? 1 : 0;
}

// docs/351: steady 30 Hz game pacing. Read every VBlank by apply_cadence
// through lb_cadence_effective, which lets an env override and a playing demo
// take precedence.
// docs/387. LB_PALETTE / LB_TILESET force them without the UI (test aids).
// docs/415. LB_SWEAR_FILTER=0|1 forces the filter (a test aid).
extern "C" int lb_setting_swear_filter(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_SWEAR_FILTER");
        forced = v ? (std::atoi(v) ? 1 : 0) : -1;
    }
    return forced >= 0 ? forced : (g_set.swear_filter ? 1 : 0);
}
extern "C" int lb_setting_chat_toasts(void) { return g_set.chat_toasts ? 1 : 0; }
extern "C" int lb_setting_fetch_maps(void) { return g_set.fetch_maps ? 1 : 0; }   // docs/420
extern "C" int lb_setting_fast_link(void) { return g_set.fast_link ? 1 : 0; }     // docs/423
extern "C" int lb_setting_match_picture(void) { return g_set.match_picture ? 1 : 0; }   // docs/428
// SG-0018: LB_SCORES_OPT_OUT=0|1 forces it (a test aid).
extern "C" int lb_setting_scores_opt_out(void) {
    static const char* v = std::getenv("LB_SCORES_OPT_OUT");
    if (v && *v) return std::atoi(v) ? 1 : 0;
    return g_set.scores_opt_out ? 1 : 0;
}
extern "C" int lb_setting_discord_status(void) { return g_set.discord_status ? 1 : 0; }   // docs/430
extern "C" int lb_setting_discord_hero(void) { return g_set.discord_hero ? 1 : 0; }
// docs/419. LB_MASTER_HTTPS=0|1 forces it (a test aid).
extern "C" int lb_setting_master_https(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_MASTER_HTTPS");
        forced = v ? (std::atoi(v) ? 1 : 0) : -1;
    }
    return forced >= 0 ? forced : (g_set.master_https ? 1 : 0);
}
extern "C" const char* lb_overlay_player_name(void) { return mp_player_name(); }

extern "C" int lb_setting_smooth_models(void) {   // docs/411; LB_SMOOTH_MODELS
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_SMOOTH_MODELS");
        forced = v ? (std::atoi(v) ? 1 : 0) : -1;
    }
    return forced >= 0 ? forced : (g_set.smooth_models ? 1 : 0);
}

extern "C" int lb_setting_res3d(void) {   // docs/434; LB_RES3D=1..4, a test aid
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_RES3D");
        forced = v ? std::atoi(v) : -1;
        if (forced > 4) forced = 4;
    }
    if (forced >= 1) return forced;
    if (forced == 0) return 1;
    return 1 + (g_set.res3d < 0 ? 0 : g_set.res3d > 3 ? 3 : g_set.res3d);
}

extern "C" int lb_setting_mod_palette(void) {   // docs/410
    static int forced = -2;   // LB_MOD_PALETTE=0..4, a test aid
    if (forced == -2) {
        const char* v = std::getenv("LB_MOD_PALETTE");
        forced = v ? std::atoi(v) : -1;
    }
    const int m = forced >= 0 ? forced : g_set.mod_palette;
    return (m < 0 || m > 4) ? 0 : m;
}

extern "C" int lb_setting_level_palette(void) {
    static const int forced = [] {
        const char* v = std::getenv("LB_PALETTE");
        return v ? std::atoi(v) : -1;
    }();
    const int m = forced >= 0 ? forced : g_set.level_palette;
    return (m < 0 || m > 3) ? 0 : m;
}
extern "C" int lb_setting_force_tileset(void) {
    static const int forced = [] {
        const char* v = std::getenv("LB_TILESET");
        return v ? std::atoi(v) : -1;
    }();
    const int m = forced >= 0 ? forced : g_set.force_tileset;
    return (m < 0 || m > 4) ? 0 : m;
}

extern "C" int lb_setting_steady_cadence(void) {
    return g_set.steady_cadence ? 1 : 0;
}

static int hotwire_mode_effective(void);

extern "C" int lb_setting_patch_hotwire_glitch(void) {
    return hotwire_mode_effective() == 1 ? 1 : 0;
}

// BR-0011: while joined these follow the host's 'S' page (lb_teams.cpp).
extern "C" int lb_setting_attack_boost_ranged(void) {
    { LbHostSettings h; if (lb_rules_host_settings(&h) >= 0) return h.attack_boost; }   // BR-0011
    // LB_ATTACKBOOST=1 forces this on without the UI, the same test aid
    // LB_CHEATS, LB_BALANCE and LB_UNLOCKS have. It was the only enhancement
    // without one, which is why it is also the only one whose badge could not
    // be settled by a headless run.
    static int forced = -1;
    if (forced < 0) {
        const char* v = std::getenv("LB_ATTACKBOOST");
        forced = v ? std::atoi(v) : -1;
    }
    const int m = forced >= 0 ? forced : g_set.attack_boost_ranged;
    return m < 0 ? 0 : (m > 2 ? 2 : m);   // docs/433: 2 = not the blast
}

// LB_HOTWIRE_TRANSPORT=<0|1|2> forces the combo without the UI, so mode 1 --
// which is newly implemented and issues guest calls -- can be exercised
// headlessly. Same idiom as LB_HOTWIRE_MODE, LB_FORCE_CAP and LB_MUSIC_MODE.
extern "C" int lb_setting_hotwire_transport(void) {
    { LbHostSettings h; if (lb_rules_host_settings(&h) >= 0) return h.hotwire_transport; }   // BR-0011
    static int forced = -1;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* v = std::getenv("LB_HOTWIRE_TRANSPORT")) {
            const int n = std::atoi(v);
            if (n >= 0 && n <= 2) forced = n;
        }
    }
    return forced >= 0 ? forced : g_set.hotwire_transport;
}

// docs/406: this machine's EMP-on-transports choice (LB_EMP_TRANSPORTS=0|1
// forces it). The patch runs with lb_patches.cpp's effective value: a
// replay's, else the host's online, else this.
extern "C" int lb_setting_emp_transports(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_EMP_TRANSPORTS");
        forced = v ? (std::atoi(v) ? 1 : 0) : -1;
    }
    return forced >= 0 ? forced : (g_set.emp_transports ? 1 : 0);
}

// docs/408: LB_TRANSPORT_RELEASE=0|1 forces it.
extern "C" int lb_setting_transport_release(void) {
    { LbHostSettings h; if (lb_rules_host_settings(&h) >= 0) return h.transport_release; }   // BR-0011
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_TRANSPORT_RELEASE");
        forced = v ? (std::atoi(v) ? 1 : 0) : -1;
    }
    return forced >= 0 ? forced : (g_set.transport_release ? 1 : 0);
}

// docs/406: LB_AI_HERO_REBUILD=0|1 forces it.
extern "C" int lb_setting_ai_hero_rebuild(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_AI_HERO_REBUILD");
        forced = v ? (std::atoi(v) ? 1 : 0) : -1;
    }
    return forced >= 0 ? forced : (g_set.ai_hero_rebuild ? 1 : 0);
}

// docs/406: the rule flags a host sends its players (lb_teams.cpp).
extern "C" int lb_rules_local_flags(void) {
    return (lb_setting_emp_transports() ? LB_RULE_EMP_TRANSPORTS : 0) |
           (lb_setting_creatures() ? LB_RULE_CREATURES : 0);   // docs/410
}

// docs/410: LB_CREATURES=0|1 forces it.
extern "C" int lb_setting_creatures(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_CREATURES");
        forced = v ? (std::atoi(v) ? 1 : 0) : -1;
    }
    return forced >= 0 ? forced : (g_set.creatures ? 1 : 0);
}

// The "Fix Hotwired Healing" checkbox had NO getter: nothing in the port could
// read it. lb_setting_patch_hotwire_glitch is a different control -- it is
// hotwire_mode == 1, "Patch the glitch (specials only)", which filters what
// hotwire may TARGET and says nothing about healing.
extern "C" int lb_setting_fix_monkey_cage(void) {   // docs/385
    return g_set.fix_monkey_cage ? 1 : 0;
}

extern "C" int lb_setting_own_hero_spells(void) {   // docs/385
    return g_set.own_hero_spells ? 1 : 0;
}

extern "C" int lb_setting_fix_hotwired_healing(void) {
    { LbHostSettings h; if (lb_rules_host_settings(&h) >= 0) return h.fix_healing; }   // BR-0011
    return g_set.fix_hotwired_healing ? 1 : 0;
}

// LB_HOTWIRE_MODE=<0|1|2> forces the combo without the UI, so the cheat can be
// measured headlessly and without writing the player's settings.json -- the same
// idiom as LB_FORCE_CAP, LB_PIXEL_SCALE and LB_HUDSCALE.
// This machine's Hotwire targets (LB_HOTWIRE_MODE forces it), as recorded.
extern "C" int lb_setting_hotwire_mode(void) {
    { LbHostSettings h; if (lb_rules_host_settings(&h) >= 0) return h.hotwire_mode; }   // BR-0011
    static int forced = -1;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* v = std::getenv("LB_HOTWIRE_MODE")) {
            const int n = std::atoi(v);
            if (n >= 0 && n <= 2) forced = n;
        }
    }
    return forced >= 0 ? forced : g_set.hotwire_mode;
}

// docs/392: the one the game runs with -- a replay's own, else this machine's.
// It changes what the simulation allows, so a recording carries it.
extern "C" int lb_demo_play_hotwire_mode(void);   /* src/lb_demo.cpp */
static int hotwire_mode_effective(void) {
    const int played = lb_demo_play_hotwire_mode();
    return played >= 0 ? played : lb_setting_hotwire_mode();
}

extern "C" int lb_setting_hotwire_anything(void) {
    return hotwire_mode_effective() == 2 ? 1 : 0;
}

extern "C" int lb_setting_cap(int build_limit_class) {
    if (build_limit_class < 0 || build_limit_class >= 10) return 0;
    { LbHostSettings h; if (lb_rules_host_settings(&h) >= 0) return h.caps[build_limit_class]; }   // BR-0011

    // LB_FORCE_CAP=<class>:<value> sets one class without the UI, so a headless
    // run can verify the patch. Test aid only, like LB_AUTOPLAY.
    static int forced_class = -2;
    static int forced_value = 0;
    if (forced_class == -2) {
        forced_class = -1;
        if (const char* spec = std::getenv("LB_FORCE_CAP")) {
            int c = 0, v = 0;
            if (std::sscanf(spec, "%d:%d", &c, &v) == 2 && c >= 0 && c < 10) {
                forced_class = c;
                forced_value = v;
            }
        }
    }
    if (build_limit_class == forced_class) return forced_value;

    return g_set.cap[build_limit_class];
}

extern "C" int lb_setting_cheat(int index) {
    if (index < 0 || index >= kCheatCount) return 0;
    // LB_CHEATS=0,3,17 forces cheats on without the UI, so the wiring can be
    // verified headlessly. Test aid only, exactly like LB_BALANCE above it.
    static unsigned forced = 0u;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* spec = std::getenv("LB_CHEATS")) {
            const char* p = spec;
            while (*p) {
                if (*p >= '0' && *p <= '9') {
                    int v = 0;
                    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
                    if (v >= 0 && v < 32) forced |= (1u << v);
                } else {
                    ++p;
                }
            }
        }
    }
    // docs/406: a replay runs on the recording's cheats (from sets=3).
    const int played = lb_demo_play_cheats();
    if (played >= 0) return (played >> index) & 1;
    { LbHostSettings h; if (lb_rules_host_settings(&h) >= 0) return (int)((h.cheats >> index) & 1u); }   // BR-0011
    if (forced & (1u << index)) return 1;
    return g_set.cheat[index] ? 1 : 0;
}

extern "C" int lb_setting_cheat_count(void) { return kCheatCount; }
// BR-0011: joined, the host's cheats apply and the cartridge's own are held off.
extern "C" int lb_setting_cheats_locked(void) {
    return lb_rules_host_settings(nullptr) >= 0 ? 1 : 0;
}

// docs/406: the cheats as a mask, bit i = flags[i], as recordings carry them.
extern "C" int lb_cheat_mask_live(void) {
    int m = 0;
    for (int i = 0; i < kCheatCount; ++i)
        if (lb_setting_cheat(i)) m |= 1 << i;
    return m;
}

// docs/406: the AI gets the red-brick cheats too (lb_patches.cpp, kAiCheats).
// LB_AI_CHEATS=0|1 forces it. Offline only; a replay's own when replaying.
extern "C" int lb_setting_ai_cheats(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_AI_CHEATS");
        // docs/433: 0 me, 1 me and the AI, 2 the AI only.
        forced = v ? std::min(std::max(std::atoi(v), 0), 2) : -1;
    }
    return forced >= 0 ? forced
                       : (g_set.ai_cheats ? (g_set.ai_cheats_only ? 2 : 1) : 0);
}

// SG-0017: the AI casts single-target spells. LB_AI_TAP_SPELLS=0|1 forces it.
// Offline only (lb_patches.cpp applies it only with no network). The single
// accessor for the host-rules page to carry.
extern "C" int lb_setting_ai_tap_spells(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_AI_TAP_SPELLS");
        forced = v ? (std::atoi(v) ? 1 : 0) : -1;
    }
    return forced >= 0 ? forced : (g_set.ai_tap_spells ? 1 : 0);
}

// SG-0002: the computer plays the player's side (Free Play, offline).
// LB_AI_SPECTATE=0|1 forces it. lb_patches.cpp applies it only with no network.
extern "C" int lb_setting_ai_spectate(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* v = std::getenv("LB_AI_SPECTATE");
        forced = v ? (std::atoi(v) ? 1 : 0) : -1;
    }
    return forced >= 0 ? forced : (g_set.ai_spectate ? 1 : 0);
}

// Which faction's music a match plays. 0 vanilla (the game untouched),
// 1 random, 2 faction-aligned by builder, 3 map-specific, 4..9 one side
// always (docs/387). Consumed by
// src/lb_music.cpp, which is where the reasoning lives.
extern "C" int lb_setting_music_mode(void) {
    static int forced = -1;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* v = std::getenv("LB_MUSIC_MODE"))
            forced = std::atoi(v);
    }
    const int m = forced >= 0 ? forced : g_set.music_mode;
    return (m < 0 || m > 9) ? 0 : m;   // docs/387: 4..9 one faction always
}

// docs/356: what this machine's text calls the Space Criminal Leader, 0 the
// cartridge's own words, 1 Kranxx, 2 Plisken. Read every VBlank by
// apply_leader_name in lb_patches.cpp. LB_LEADER_NAME=<0|1|2> forces it
// without the UI, the same test aid as LB_HOTWIRE_TRANSPORT and LB_MUSIC_MODE.
extern "C" int lb_setting_criminal_leader_name(void) {
    static int forced = -1;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* v = std::getenv("LB_LEADER_NAME")) {
            const int n = std::atoi(v);
            if (n >= 0 && n <= 2) forced = n;
        }
    }
    const int m = forced >= 0 ? forced : g_set.criminal_leader_name;
    return (m < 0 || m > 2) ? 0 : m;
}

// docs/392: the Pirate Plantation's name, 0 shipped, 1 Hide-Away, 2
// Storehouse. LB_PLANTATION_NAME=<0|1|2> forces it, like LB_LEADER_NAME.
extern "C" int lb_setting_plantation_name(void) {
    static int forced = -1;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* v = std::getenv("LB_PLANTATION_NAME")) {
            const int n = std::atoi(v);
            if (n >= 0 && n <= 2) forced = n;
        }
    }
    const int m = forced >= 0 ? forced : g_set.plantation_name;
    return (m < 0 || m > 2) ? 0 : m;
}

// docs/425: the cosmetic renames Claw Tank (0), Gemma (1), Broadside (2):
// 0 the cartridge's word, 1 the new one. LB_RENAME_CLAW_TANK / LB_RENAME_GEMMA
// / LB_RENAME_BROADSIDE=<0|1> force them, like LB_LEADER_NAME.
extern "C" int lb_setting_rename(int which) {
    // LB_RENAME_ALL=1 forces every rename on (test aid, SG-0003/SG-0014).
    static const char* const kEnv[11] = {"LB_RENAME_CLAW_TANK", "LB_RENAME_GEMMA",
                                         "LB_RENAME_BROADSIDE", "LB_RENAME_ASSAULT_SHIP",
                                         "LB_RENAME_ATTACK_SHIP", "LB_RENAME_EARTH_TRANSPORT",
                                         "LB_RENAME_ALIEN_TRANSPORT", "LB_RENAME_STARFIGHTER",
                                         "LB_RENAME_STARSHIP1", "LB_RENAME_PIRATE_BUILDINGS",
                                         "LB_RENAME_BOUNTY"};
    static int forced[11] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        const char* all = std::getenv("LB_RENAME_ALL");
        for (int i = 0; i < 11; ++i) {
            if (all && std::atoi(all) == 1) forced[i] = 1;
            if (const char* v = std::getenv(kEnv[i])) {
                const int n = std::atoi(v);
                if (n >= 0 && n <= 1) forced[i] = n;
            }
        }
    }
    if (which < 0 || which > 10) return 0;
    const int own[11] = {g_set.rename_claw_tank, g_set.rename_gemma,
                         g_set.rename_broadside, g_set.rename_assault_ship,
                         g_set.rename_attack_ship, g_set.rename_earth_transport,
                         g_set.rename_alien_transport, g_set.rename_starfighter,
                         g_set.rename_starship1, g_set.rename_pirate_buildings,
                         g_set.rename_bounty};
    const int m = forced[which] >= 0 ? forced[which] : own[which];
    return (m < 0 || m > 1) ? 0 : m;
}

// docs/392: Free Play's opening opponent, 0..7 (lb_overlay.cpp's field).
// LB_DEFAULT_OPPONENT=<0..7> forces it.
extern "C" int lb_setting_default_opponent(void) {
    static int forced = -1;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* v = std::getenv("LB_DEFAULT_OPPONENT")) {
            const int n = std::atoi(v);
            if (n >= 0 && n <= 7) forced = n;
        }
    }
    const int m = forced >= 0 ? forced : g_set.default_opponent;
    return (m < 0 || m > 7) ? 1 : m;
}

extern "C" float lb_setting_pixel_scale(void) {
    static float forced = -1.0f;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* v = std::getenv("LB_PIXEL_SCALE"))
            forced = (float)std::atof(v);
    }
    // docs/409: LB_TEST_ZOOM=<call>:<scale>,... changes the zoom as the wheel
    // does, at the given call of this function (one per presented frame), so
    // a recording with zoom in it can be made without input. Test aid only.
    static std::vector<std::pair<unsigned, float>> zooms;
    static bool zoom_env = false;
    static unsigned calls = 0;
    if (!zoom_env) {
        zoom_env = true;
        if (const char* p = std::getenv("LB_TEST_ZOOM")) {
            unsigned at = 0;
            float sc = 0.0f;
            int used = 0;
            while (std::sscanf(p, "%u:%f%n", &at, &sc, &used) == 2) {
                zooms.emplace_back(at, sc);
                p += used;
                if (*p == ',') ++p;
            }
        }
    }
    ++calls;
    for (const auto& z : zooms)
        if (z.first == calls) g_set.pixel_scale = z.second;
    const float s = forced > 0.0f ? forced : g_set.pixel_scale;
    return s < 1.0f ? 1.0f : (s > 16.0f ? 16.0f : s);
}

extern "C" float lb_setting_hud_scale(void) {
    static float forced = -1.0f;
    static bool read_env = false;
    if (!read_env) {
        read_env = true;
        if (const char* v = std::getenv("LB_HUDSCALE"))
            forced = (float)std::atof(v);
    }
    if (forced > 0.0f) return forced;
    return g_set.hud_scale;
}

extern "C" int lb_setting_hud_anchor(void) {
    // LB_HUDANCHOR=0 forces it off headlessly, so a capture can be taken
    // with and without the move and the two compared -- the same test aid as
    // LB_CHEATS and LB_BALANCE.
    static int forced = -1;
    if (forced < 0) {
        const char* v = std::getenv("LB_HUDANCHOR");
        forced = v ? std::atoi(v) : -1;
    }
    if (forced >= 0) return forced ? 1 : 0;
    // NOT a setting of its own. Anchoring the HUD is part of making the
    // window size work, not a separate feature to opt into -- so it follows
    // Enhanced. The env override stays for A/B captures.
    return lb_setting_enhanced();
}

// A DEVELOPER A/B KNOB, DELIBERATELY ENV-ONLY. Not a player setting, and this
// is the evidence for keeping it out of the panel rather than an omission.
//
// THE OLD COMMENT HERE IS RETRACTED. It said LB_VIEW_WIDTH and LB_FORCE_WIDTH
// are two halves of one feature -- "both are needed: a wider view with a 256 px
// raster has nowhere to put the extra columns" -- and that is measurably false.
// docs/183 captured the same point of the same save with the raster ALREADY
// widened (LB_FORCE_WIDTH=448, LB_BG_EXTEND=1), with and without this patch:
//
//     stock descriptor (32 wide): content reaches x=356 of 448
//     patched         (56 wide): content reaches x=356 of 448
//     difference: 171 pixels of 86016, all in the centre where units animate
//
// docs/192 repeated it and got output that is byte-identical, "the same five
// bands, to the column". So the two are not complements; the raster half is the
// whole of what a player sees, and this one is not needed for it.
//
// WHAT IT ACTUALLY DOES. 020333A4 is the width immediate of the cartridge's
// Set_View_Dimensions, and the setter reallocates from it, so patching it does
// work -- it correctly resizes the FOG/VISIBILITY viewport and nothing else.
// Read live with LB_VIEW_WIDTH=448 (docs/192, docs/202): the object at 02151F28
// goes stride 4 -> 7, width 0x20 -> 0x38, row table reallocated 02245258 ->
// 022452A0. And that changes no pixels, because the fog draw loop is still
// hard-coded to tx < 0x20 (docs/202), so a wider mask alone was never going to
// draw wider fog. docs/183's own verdict: "it correctly widens the fog viewport
// and, on its own, does nothing a player would see."
//
// WHAT SUPERSEDES IT for the thing a player wants. The widened picture is the
// adaptive path, and it is host-side: lb_setting_enhanced gates the whole
// feature (docs/239 counts 9 sites across 3 files), lb_widemap.cpp supplies the
// tile entries for the widened columns out of a host array, apply_cull_width
// moves the entity gather rect, and lb_setting_pixel_scale decides how much
// field a given window buys. None of those need this.
//
// AND IT IS NOT HARMLESS, which is what settles it. docs/231 tried it as a
// hypothesis for the out-of-band click refusal, twice (2026-09-05,
// 2026-09-06): LB_VIEW_WIDTH=392 left the left band at 0 of 10 selections
// against 3 of 10 in band, and widening to 512 "leaves the band at 0 of 4 AND
// DEGRADES THE IN-BAND CONTROL to 2 of 4, so it is neither the guard nor
// harmless". A knob that measurably makes selection WORSE inside the native
// columns is not something to hand a player a slider for.
//
// NOT ESTABLISHED, and stated rather than guessed: whether it can desync a
// multiplayer game. It is unlike every other widescreen control in this file --
// it rewrites a GUEST INSTRUCTION (bus_write_u32 at 020333A4, apply_view_width
// in lb_patches.cpp), so docs/85's argument that the port's rendering changes
// are client-local does not cover it, and nothing has tested two clients
// disagreeing about it. Settling that would need a two-instance lockstep run
// with the variable set on one side only. Until then, env-only is also the
// conservative answer.
extern "C" int lb_setting_view_width(void) {
    static int px = -1;
    if (px < 0) {
        const char* v = std::getenv("LB_VIEW_WIDTH");
        px = v ? std::atoi(v) : 0;
    }
    return px;
}

extern "C" int lb_setting_unlock_units(void) {
    // LB_UNLOCKS=units,extras forces these on headlessly, like LB_CHEATS.
    static int forced = -1;
    if (forced < 0) {
        const char* spec = std::getenv("LB_UNLOCKS");
        forced = (spec && std::strstr(spec, "units")) ? 1 : 0;
    }
    return (g_set.unlock_units || forced) ? 1 : 0;
}

extern "C" int lb_setting_unlock_extras(void) {
    static int forced = -1;
    if (forced < 0) {
        const char* spec = std::getenv("LB_UNLOCKS");
        forced = (spec && std::strstr(spec, "extras")) ? 1 : 0;
    }
    return (g_set.unlock_extras || forced) ? 1 : 0;
}

// docs/407: every unlock category by number, LB_UNLOCKS=<words> forcing them
// (units, extras, maps, art, campaigns, or all).
extern "C" int lb_setting_unlock(int kind) {
    static const char* const kWord[LB_UNLOCK_KINDS] = {
        "units", "extras", "maps", "art", "campaigns"};
    if (kind < 0 || kind >= LB_UNLOCK_KINDS) return 0;
    static int forced = -1;
    if (forced < 0) {
        forced = 0;
        if (const char* spec = std::getenv("LB_UNLOCKS"))
            for (int k = 0; k < LB_UNLOCK_KINDS; ++k)
                if (std::strstr(spec, kWord[k]) || std::strstr(spec, "all"))
                    forced |= 1 << k;
    }
    const bool on[LB_UNLOCK_KINDS] = {g_set.unlock_units, g_set.unlock_extras,
                                      g_set.unlock_maps, g_set.unlock_art,
                                      g_set.unlock_campaigns};
    return (on[kind] || ((forced >> kind) & 1)) ? 1 : 0;
}

extern "C" int lb_setting_same_builders(void) {
    // LB_FORCE_SAME_BUILDERS=1 turns this on without the UI, so a headless run
    // can verify the patch. Same role as LB_AUTOPLAY: a test aid, never on by
    // default. Without it the only way to exercise the feature is by hand.
    static int forced = -1;
    // docs/396: =1 still means the Imperial builder; =2..6 nothing new.
    if (forced < 0) forced = std::getenv("LB_FORCE_SAME_BUILDERS") ? 1 : 0;
    if (forced) return 4;

    // This COMPOSES with a balance patch rather than being suppressed by it.
    // The patch is applied first, so equalising to "the Imperial builder"
    // means the patched Imperial, which is what a player selecting both would
    // expect. lb_patches.cpp enforces that order.
    return g_set.same_builders;   // docs/396: 0, or the source builder 1..6
}

extern "C" void lb_overlay_shutdown(void) {
    if (!g_ready) return;
    lb_settings_save();
    // ALL THREE OF THE OVERLAY'S OWN TEXTURES, not two.
    //
    // g_wide_tex was missed here. That was invisible while this function had
    // no caller at all; now that destroy_presentation calls it
    // (port/runner/apply_hooks.py, "the overlay teardown finally has a
    // caller"), the omission has teeth, because destroy_presentation goes on
    // to SDL_DestroyRenderer the renderer all three were created from.
    //
    // Leaving g_wide_tex non-null across that leaves a pointer to a texture
    // whose renderer is gone. draw_wide_field's guard is
    // `if (!g_wide_tex || g_wide_w != mw || g_wide_h != mh)`, so on a
    // recreate at the SAME map size the guard is false and the next
    // SDL_UpdateTexture writes through the freed handle; at a different size
    // the SDL_DestroyTexture inside the guard double-frees it. The one
    // existing teardown-then-recreate -- the gl_top -> SDL fallback at
    // frontend.cpp -- runs BEFORE the first lb_overlay_draw, so g_ready is
    // false there and this is latent rather than live today. It is fixed
    // rather than noted because the widescreen path already recreates
    // textures mid-run (lb_fit_texture) and the next such caller would hit
    // it for real.
    //
    // The sizes are cleared as well as the pointer: the pointer alone is
    // enough for the guard, but a stale 0-size pair is the honest state after
    // the texture it described is gone.
    if (g_top_tex) { SDL_DestroyTexture(g_top_tex); g_top_tex = nullptr; }
    if (g_field_tex) { SDL_DestroyTexture(g_field_tex); g_field_tex = nullptr; }
    if (g_wide_tex) { SDL_DestroyTexture(g_wide_tex); g_wide_tex = nullptr; }
    g_top_tex_w = g_top_tex_h = 0;
    g_field_tex_w = g_field_tex_h = 0;
    g_wide_w = g_wide_h = 0;
    lb_teams_shutdown();          // docs/384: the head sheets, same renderer
    g_team_renderer = nullptr;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

// ── docs/394: what a lobby announces ──────────────────────────────────────
//
// The build, for the master server's log and for telling playtest builds
// apart: version.txt beside the executable (tools/lb_dist.py writes it into a
// release), else when this was compiled.
#ifndef LB_PORT_VERSION
#define LB_PORT_VERSION "dev " __DATE__
#endif
extern "C" const char* lb_port_version(void) {
    static std::string version;
    if (version.empty()) {
        version = LB_PORT_VERSION;
        if (char* base = SDL_GetBasePath()) {
            const std::string path = std::string(base) + "version.txt";
            SDL_free(base);
            if (FILE* f = std::fopen(path.c_str(), "rb")) {
                char line[64] = "";
                if (std::fgets(line, sizeof line, f)) {
                    std::string v(line);
                    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' ||
                                          v.back() == ' '))
                        v.pop_back();
                    if (!v.empty()) version = v;
                }
                std::fclose(f);
            }
        }
        // Test aid (docs/430): LB_VERSION_TEST=<text> stands in for the build, to
        // exercise the lobby's version warning with two copies of one build.
        if (const char* t = std::getenv("LB_VERSION_TEST"))
            if (*t) version = std::string(t).substr(0, 60);
    }
    return version.c_str();
}

// The host's settings that change the simulation, by the panel's own labels,
// as one JSON object. The master server prints them as they are, so it never
// has to know what a number means. Read from this machine's settings: in a
// match every machine must hold the same ones (docs/392), and this is what
// the host is running.
extern "C" const char* lb_net_lobby_options_json(void) {
    static std::string out;
    static const char* kHotwire[] = {"Vanilla", "Specials only", "Anything (cheat)"};
    static const char* kTransport[] = {"Off (vanilla)", "Destroy them", "Hotwire them"};
    static const char* kCapName[10] = {
        "Heroes", "Minifigures", "Specials", "Towers", "Buildings",
        nullptr, "Walls", "Bridges", nullptr, nullptr};   // docs/405: no gates
    auto pick = [](const char* const* names, int n, int v) {
        return (v >= 0 && v < n) ? names[v] : "?";
    };
    auto esc = [](const std::string& s) {
        std::string r = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') { r += '\\'; r += c; }
            else if ((unsigned char)c >= 0x20) r += c;
        }
        return r + "\"";
    };
    std::vector<std::pair<std::string, std::string>> kv;
    kv.push_back({"Balance patch", (g_set.balance_patch >= 0 && g_set.balance_patch <= lb_balance_pack_count())
                      ? lb_balance_name(g_set.balance_patch) : "?"});
    kv.push_back({"Hotwire targets", pick(kHotwire, 3, g_set.hotwire_mode)});
    kv.push_back({"Transport occupants", pick(kTransport, 3, g_set.hotwire_transport)});
    kv.push_back({"EMP disables transports", g_set.emp_transports ? "On" : "Off"});
    kv.push_back({"AI rebuilds its hero", g_set.ai_hero_rebuild ? "On" : "Off"});
    kv.push_back({"Hotwired transports leave", g_set.transport_release ? "On" : "Off"});
    kv.push_back({"Attack boost reaches ranged",
                  g_set.attack_boost_ranged >= 2 ? "On, no blast"
                  : g_set.attack_boost_ranged ? "On" : "Off"});
    kv.push_back({"Fix hotwired healing", g_set.fix_hotwired_healing ? "On" : "Off"});
    {
        static const char* const kFrom[7] = {"Off", "King", "Wizard", "Pirate",
                                             "Imperial", "Earth", "Alien"};
        const int f = g_set.same_builders;
        kv.push_back({"Same builders", kFrom[f >= 0 && f <= 6 ? f : 0]});
    }
    std::string caps;
    for (int i = 0; i < 10; ++i) {
        if (!kCapName[i] || g_set.cap[i] == kShippedCap[i]) continue;
        if (!caps.empty()) caps += ", ";
        caps += std::string(kCapName[i]) + " " + std::to_string(g_set.cap[i]);
    }
    kv.push_back({"Unit caps", caps.empty() ? "Shipped" : caps});
    std::string cheats;
    for (int i = 0; i < kCheatCount; ++i) {
        if (!g_set.cheat[i]) continue;
        if (!cheats.empty()) cheats += ", ";
        cheats += kCheats[i].name;
    }
    kv.push_back({"Cheats", cheats.empty() ? "None" : cheats});
    if (g_set.unlock_units) kv.push_back({"Unlock all units", "On"});
    if (g_set.unlock_maps) kv.push_back({"Unlock all maps", "On"});   // docs/407
    // The lists by name, as the game shows them, never as the settings' ids
    // and tokens ("133,179", "s5,h<hash>").
    auto unit_names = [](const char* ids, const char* sep) {
        std::string o;
        for (const char* p = ids; p && *p;) {
            const int bp = std::atoi(p);
            if (!o.empty()) o += sep;
            o += lb_teams_unit_name(bp);
            while (*p && *p != ',') ++p;
            if (*p) ++p;
        }
        return o;
    };
    auto map_names = [](const char* toks) {
        static char buf[1024];
        lb_maps_names_of(toks, buf, (int)sizeof buf);
        return std::string(buf);
    };
    if (g_set.banned_units[0])
        kv.push_back({"Banned units", unit_names(g_set.banned_units, ", ")});
    if (g_set.banned_maps[0])
        kv.push_back({"Banned maps", map_names(g_set.banned_maps)});   // docs/429
    if (g_set.unit_map_bans[0]) {
        // "bp:tok+tok;bp:tok" -> "Sheriff: The Pond, Avast; ..."
        std::string o;
        const std::string umb = g_set.unit_map_bans;
        size_t at = 0;
        while (at < umb.size()) {
            size_t end = umb.find(';', at);
            if (end == std::string::npos) end = umb.size();
            const std::string one = umb.substr(at, end - at);
            const size_t colon = one.find(':');
            if (colon != std::string::npos) {
                if (!o.empty()) o += "; ";
                o += std::string(lb_teams_unit_name(std::atoi(one.c_str()))) + ": " +
                     map_names(one.substr(colon + 1).c_str());
            }
            at = end + 1;
        }
        kv.push_back({"Units banned on some maps", o});
    }
    if (g_set.ban_preset[0]) kv.push_back({"Ban preset", g_set.ban_preset});
    if (g_set.random_maps) kv.push_back({"Random map entries", "On"});
    if (g_set.tourney_team) kv.push_back({"Random team (tourney)", "On"});
    out = "{";
    for (size_t i = 0; i < kv.size(); ++i) {
        if (i) out += ",";
        out += esc(kv[i].first) + ":" + esc(kv[i].second);
    }
    out += "}";
    return out.c_str();
}
