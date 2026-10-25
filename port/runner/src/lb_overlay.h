// lb_overlay.h - the LEGO Battles ImGui layer.
//
// This file belongs to legobattles-decomp, NOT to ndsrecomp. Nothing in the
// upstream runner is edited in place; port/runner/apply_hooks.py inserts the
// four calls below into a COPY of the upstream tree at build time. See
// port/runner/README.md for the fork policy.
//
// Everything here is presentation and settings state. No control in this layer
// touches the simulation yet, and the project does not
// pretend otherwise, so any toggle whose effect is not yet implemented is
// rendered with an explicit "not wired" marker rather than silently doing
// nothing.

#ifndef LB_OVERLAY_H
#define LB_OVERLAY_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Master-server client (port/runner/src/lb_net.cpp) ──────────────────────
//
// Lists, announces and looks up lobbies against services/master-server.
//
// It does not speak the game's own protocol and does not need to: the
// cartridge does its own discovery, joining and lobby over DS wireless, and
// the runner's local-wireless bridge carries those frames. What this adds is
// the one thing the bridge could not supply -- the address of a host on
// ANOTHER machine. See docs/216.
typedef struct LbLobby {
    char code[8];
    char name[64];
    char host_name[32];
    char map_name[32];
    int  players;
    int  max_players;
    /* Where the host actually is. The directory records the address it saw
       the host announce from, and the UDP port the host said it is bound
       to. Without these a listing is something to look at rather than
       something to join. */
    char address[40];
    int  port;
} LbLobby;

void        lb_net_refresh_lobbies(const char* host, int port);
int         lb_net_lobby_count(void);
int         lb_net_lobby_at(int index, LbLobby* out);
const char* lb_net_status(void);
int         lb_net_busy(void);
void        lb_net_selftest_if_asked(void);
// LB_MP_PEER=host:port / LB_MP_LISTEN=1, the command-line form of the
// Multiplayer tab's join and advertise buttons.
void        lb_net_apply_env_peer(void);

// -- Demo record / replay (port/runner/src/lb_demo.cpp) -------------------
//
// Records what the CONSOLE received -- nds_set_touch, nds_set_key_mask and
// this port's extended x -- one line per change, indexed by vblank. A
// player plays; the port replays it as often as an investigation needs.
void        lb_demo_start_record(const char* path);
void        lb_demo_start_play(const char* path);
void        lb_demo_stop(void);
int         lb_demo_state(void);        // 0 idle, 1 recording, 2 playing
const char* lb_demo_status(void);
unsigned    lb_demo_frame(void);
unsigned    lb_demo_event_count(void);
// The game cadence a PLAYING demo was recorded at (20 = the cartridge's 20 ms
// threshold, 30 = steady 30 Hz), or 0 when no demo is playing. A demo with no
// cadence in its header predates the steady default and replays at 20, so an
// old recording still reproduces the game its player saw (docs/351).
int         lb_demo_play_cadence(void);
// Whether a PLAYING demo's touches are true coordinates (1; recorded while the
// widget hit test was corrected, docs/371) or were rewritten by the frontend's
// HUD unmap (0; every older recording). -1 when no demo is playing.
int         lb_demo_play_hudhit(void);
// Likewise for the camera's range (docs/373): 1 when the playing demo was
// recorded with the camera stopping where the picture stops, 0 for an older
// recording (its taps were aimed with the stock range), -1 when not playing.
int         lb_demo_play_camedge(void);
// 1 while that camera range is in force (lb_patches.cpp).
int         lb_camera_edge_live(void);
// And for the audibility window of unit and building sounds (docs/376).
int         lb_demo_play_snd(void);
// 1 while that window is widened to the picture (lb_patches.cpp).
int         lb_snd_cull_live(void);
// And for sprite depth across the vertical bands (docs/377).
int         lb_demo_play_depth(void);
// 1 while the depth is 11 + feet row everywhere (lb_patches.cpp; the words
// themselves are written only while the surface also has a vertical band).
int         lb_depth_unwrap_live(void);
// docs/383: the HUD-hit fallback and the two-axis footprint harvest, likewise
// (0 in an older recording, -1 when not playing).
int         lb_demo_play_hudmemo(void);
int         lb_demo_play_fp2d(void);

// Hosting advertises a lobby AND starts listening for a peer: until then the
// bridge's socket binds loopback, deliberately, so that merely enabling local
// wireless does not open a port to the network.
// Look a room code up on the master server and point the local-wireless
// bridge at the host it names. The GAME then does its own joining -- see
// the note at the top of lb_net.cpp and docs/216.
void        lb_net_join_by_code(const char* server, int port,
                                const char* code);
// The UDP port this instance listens on for wireless frames, or 0 when
// local wireless is off. What a host must advertise for anyone to reach it.
unsigned short lb_net_wireless_port(void);
// The peer currently set, as a printable "a.b.c.d:port", or "" for none.
const char* lb_net_peer_text(void);
// docs/400: this machine's public UDP mapping from STUN, "a.b.c.d:port"
// (0 and "" until known), and the bridge's link counters: frames sent, and
// datagrams received from anything that is not a sibling on this machine.
int         lb_net_public_text(char* out, int cap);
// docs/405: what the router said to the UPnP request, or "" before one.
const char* lb_net_upnp_text(void);
void        lb_net_link_stats(unsigned* sent, unsigned* recvd);
// Point the bridge at lobby `index` of the last list. What the per-row
// Join button does, and what LB_MP_JOIN=1 does at startup.
int         lb_net_join_index(int index);

// Refuses (and says so in lb_net_host_status) when game_port is 0: a lobby
// advertising a port nothing is bound to cannot be joined.
void        lb_net_announce(const char* server, int port, const char* name,
                            const char* host_name, const char* map_name,
                            int game_port, int players, int max_players);
void        lb_net_host_tick(long long now_ms);   // heartbeat, from the frame loop
void        lb_net_stop_hosting(void);
int         lb_net_is_hosting(void);
const char* lb_net_host_status(void);
const char* lb_net_host_code(void);

// What the cartridge's own network layer is doing, reported every VBlank by
// lb_patches.cpp: the state word its network task keeps (0x0215B7BC) and the
// loaded profile's name (0x02154E2D). See report_net_state there.
void        lb_net_guest_update(int net_state, const char* profile_name);
int         lb_net_guest_state(void);
const char* lb_net_guest_state_text(void);
// The loaded save profile's name, the one the game shows in its own lobby.
// Returns its length; 0 (and "") before a profile is loaded.
int         lb_net_profile_name(char* out, int cap);
// docs/396: rename the loaded profile on the next VBlank (lb_patches.cpp);
// the game writes it into the save at its next save.
void        lb_request_profile_rename(const char* name);
// docs/396: the menu's Reset. lb_request_game_reset asks; the frontend takes
// the request at its between-frames safe point (lb_take_game_reset) and runs
// the power-on boot, then lb_port_on_guest_reset (lb_patches.cpp).
void        lb_request_game_reset(void);
void        lb_request_swap_screens(void);   // SG-0012
int         lb_screens_swapped(void);
int         lb_setting_skip_intro(void);   // SG-0016
int         lb_boot_skip_wanted(void);     // SG-0016, lb_patches.cpp
int         lb_take_game_reset(void);
void        lb_port_on_guest_reset(void);
// docs/396: typing on the game's keyboards (lb_patches.cpp). ch is a
// printable character, '\b' to erase, '\r' for OK.
void        lb_keyboard_type(int ch);
int         lb_keyboard_screen_active(void);
// Once per frame, from the overlay: advertise the
// lobby the game opens when auto_host, and give the game's own Join screen the
// master server's lobbies when auto_join.
void        lb_net_auto_tick(const char* server, int port,
                             const char* player_name, int auto_host,
                             int auto_join, long long now_ms);

// docs/394: what a lobby announces besides its address. The host's
// simulation settings as a JSON object of panel label -> value
// (lb_overlay.cpp), the build's version (lb_overlay.cpp), and the last map the
// cartridge loaded with a counter that moves on each load (lb_patches.cpp,
// map_load_observer).
const char* lb_net_lobby_options_json(void);
const char* lb_port_version(void);
const char* lb_level_map_name(void);
unsigned    lb_level_map_gen(void);

// Draw the overlay for this frame and submit it to `renderer`. Initialises the
// ImGui context on first call, so there is no separate init hook to keep in
// sync with the runner's window setup. Safe to call with a null renderer.
void lb_overlay_draw(SDL_Window* window, SDL_Renderer* renderer);

// Feed one SDL event to ImGui. Call this BEFORE the runner's own handling.
// Returns true when the overlay consumed the event and the guest must not see
// it.
bool lb_overlay_event(const SDL_Event* event);

// True while a panel has the mouse. The runner's guest-touch path consults
// this so a click on a panel is not also delivered as a stylus tap.
bool lb_overlay_wants_mouse(void);

// True while a panel has the keyboard (a text field is focused, etc.).
bool lb_overlay_wants_keyboard(void);

// Release the ImGui context. Safe to call when never initialised.
void lb_overlay_shutdown(void);

// ── Settings read by the gameplay patches (port/runner/src/lb_patches.cpp) ──
//
// These are the only settings that currently reach the simulation. Everything
// else in the overlay is still presentation-only and is badged as such.
//
// Two kinds live here and the difference matters.
//
// CLIENT-LOCAL settings only change what this machine draws. docs/85 found
// zero Sim->User calls across 555 simulation virtuals, so a widescreen host
// and a Vanilla client stay in lockstep.
int lb_setting_balance_patch(void);   // 0 vanilla, 1-3 a bundled patch
int lb_setting_enhanced(void);   // Vanilla / Enhanced field of view
// docs/351: steady 30 Hz game pacing (default on). Local:
// it moves WHEN each simulation step runs, not what it does, and lockstep
// turns fix the step sequence.
int lb_setting_steady_cadence(void);
// The cadence actually in force, 20 or 30: LB_CADENCE=20/30 if set, else a
// playing demo's own, else the setting. lb_patches.cpp; lb_demo.cpp records it.
int lb_cadence_effective(void);
// docs/356: what this machine's text calls the Space Criminal Leader -- 0 the
// cartridge's own words, 1 Kranxx, 2 Plisken. Cosmetic: it rewrites the loaded
// LOC table in guest RAM, which only screens read and no peer ever receives.
// LB_LEADER_NAME=<0|1|2> forces it without the UI.
int lb_setting_criminal_leader_name(void);
// docs/392: the Pirate Plantation's name (0 shipped, 1 Hide-Away, 2
// Storehouse) and Free Play's opening opponent (0..5 factions, 6 Custom,
// 7 Random; 1 Wizard is the cartridge's own).
int lb_setting_plantation_name(void);
// docs/425: cosmetic renames 0 Claw Tank, 1 Gemma, 2 Broadside (0 off, 1 on).
int lb_setting_rename(int which);
int lb_setting_default_opponent(void);

// Rebindable input: SDL scancode -> DS key bit, from the rebindable table. Replaces
// frontend.cpp's key_bit() ENTIRELY rather than being consulted first, so a
// rebound button does not leave its shipped key working as well. Returns 0 for
// an unbound key.
int lb_binding_key_bit(int scancode);

// Rebindable input, controller half. Replaces frontend.cpp's controller_bit().
int lb_binding_pad_bit(int sdl_game_controller_button);

// LOBBY-CARRIED settings change the SIMULATION. In a multiplayer game the host
// decides and joining clients must conform, or the lockstep desyncs. Nothing
// enforces that yet because there is no client.
// Cheats: the cartridge's own 18 cheats. Index i is the byte the game keeps at
// flags[i] in the block at *(u32*)0x02150A70 -- docs/171 reads the record table
// that names them and the jump table that ties case i to flags[i], and the
// mapping was verified live (flags[0] revealed the whole minimap).
//
// LOBBY-CARRIED, every one of them: they change the simulation.
int lb_setting_cheat(int index);
int lb_setting_cheat_count(void);
int lb_overlay_wide_touch(float u, float v, int* tx, int* ty);
// WHERE THE GUEST'S 256 COLUMNS ARE ACTUALLY DRAWN. (src/lb_widemap.cpp)
//
// Declared here so the touch mapping in frontend.cpp can invert the picture
// rather than assume it. The band is NOT the centred split whenever the camera
// is near a map edge -- measured left=132 on a 1024-wide surface where the
// centred split is 384 -- and a click mapped through the centred value lands
// 252 px from where the player pointed. See docs/275.
int lb_band_left(int output_width);
int lb_band_top(int output_height);
// docs/348: the band the frame ON SCREEN was composited with (gpu2d.cpp). The
// latch above belongs to the camera read at the last VBlank; near a map edge,
// while the camera moves, the picture the player is clicking on was drawn with
// a band a pan step away from it. Touch takes this one off.
int lb_shown_band_x(void);
int lb_shown_band_y(void);
// The drag rectangle, in SURFACE coordinates. (src/lb_patches.cpp)
//
// The cartridge's own box is an 8x8 tilemap on its 32x24 screen grid and is
// clamped into 0..255 before it is drawn, so it cannot reach the widened
// columns whatever coordinate it is given. This port draws the box instead.
void lb_marquee_move(int sx, int sy);
void lb_marquee_release(void);
int lb_marquee_get(int* x0, int* y0, int* x1, int* y1);
void lb_settings_save(void);
void lb_settings_load(void);
int lb_setting_view_width(void);
int lb_setting_unlock_units(void);
int lb_setting_unlock_extras(void);
// docs/407: 0 units, 1 cheat codes, 2 maps, 3 concept art, 4 campaigns.
#define LB_UNLOCK_KINDS 5
int lb_setting_unlock(int kind);

int lb_setting_hotwire_anything(void);

// Fix hotwired healing: skip the heal when the target no longer belongs to the aura's
// owner. docs/95 found the heal path has no owner check at all.
int lb_setting_fix_hotwired_healing(void);
int lb_setting_fix_monkey_cage(void);   // docs/385, cosmetic, default on
int lb_cage_fix_live(void);             // docs/385: that, unless LB_CAGEFIX=0
int lb_setting_own_hero_spells(void);   // docs/385, default on
int lb_setting_patch_hotwire_glitch(void);

// 0 vanilla, 1 destroy the occupants, 2 convert them to the new owner.
int lb_setting_hotwire_transport(void);
int lb_setting_hotwire_mode(void);   // docs/392: this machine's, as recorded

// 0 vanilla, 1 the attack boost also reaches ranged units.
int lb_setting_attack_boost_ranged(void);
int lb_setting_same_builders(void);

// Per build-limit class cap override; 0 keeps the cartridge's own value.
// Classes follow docs/68: 0 Heroes, 1 Minifigures, 2 Specials, 3 Towers,
// 6 Walls, 7 Bridges, 8 Gates. 4, 5 and 9 are unnamed in the decomp.
int lb_setting_cap(int build_limit_class);

// ── Balance patches (port/runner/src/lb_balance.cpp) ───────────────────────
// Loads a romhack's BP/Entities.ebp from the PLAYER'S own copy and applies its
// stats over the in-RAM blueprints. Never ships anyone else's balance work.
int  lb_balance_apply(int slot, unsigned int blueprint_base,
                      char* error, int error_size);
int  lb_balance_is_applied(void);
int  lb_balance_custom_written(void);   /* docs/418 */
int  lb_balance_applied_slot(void);
const char* lb_balance_name(int slot);
int lb_balance_pack_count(void);   /* docs/426: presets in the generated table */
int  lb_balance_byte_count(int slot);
// docs/386: the loaded bytes every pack touches, taken before any is applied,
// and taking the applied pack back out with them.
void lb_balance_snapshot(unsigned int blueprint_base);
int  lb_balance_revert(unsigned int blueprint_base);
unsigned int lb_balance_extent(void);   // one past any pack's last byte

// docs/404: THE BALANCE TAB -- the player's own values, field by field, on top
// of the pack and Same-builders. Records 0..181 are units and buildings
// (0x7C each), 182..208 their attacks (projectiles, 0x74 each). Fields are
// numbered within a kind: lb_balance_field_* name them.
// docs/416: LB_BAL_GLOBALS, values in the code (record 209: field 0 a Gold
// Brick's worth); lb_balance_global gives the one in force, or -1.
enum { LB_BAL_UNITS = 0, LB_BAL_ATTACKS = 1, LB_BAL_GLOBALS = 2 };
int lb_balance_global(int field);
// docs/418: the host's balance online. A host sends its own set (seed and
// values, lb_teams.cpp); a joiner runs the host's in place of its own.
unsigned long long lb_balance_local_seed(int slot, int builders);
int  lb_balance_local_values(int* triples, int max);   /* returns the count */
int  lb_balance_host_set(unsigned long long seed, int slot, int builders,
                         const int* triples, int n);
void lb_balance_host_clear(void);
int  lb_balance_host_on(void);
int  lb_balance_host_slot(void);
int  lb_balance_host_builders(void);
int  lb_balance_host_size(void);
unsigned long long lb_balance_host_seed(void);
// lb_patches.cpp: the pack, Same Builders source and seed this machine would
// send as a host; and whether the host's set is fully in force on a joiner.
int  lb_rules_host_balance(unsigned long long* seed, int* slot, int* builders,
                           int* triples, int max);   /* lb_teams.cpp; -1 not yet */
int  lb_balance_effective_slot(void);
int  lb_balance_effective_builders(void);
int  lb_balance_host_set_current(void);
const char* lb_balance_host_set_why(void);
// docs/408: units' fields 0..9 are the table's columns; 10..14 the spells.
enum { LB_BAL_SPELL_FIELD0 = 10, LB_BAL_SPELLS = 5 };
// docs/426: units' fields 15..25 are the Traits section (lb_balance.cpp).
enum { LB_BAL_TRAIT_FIELD0 = 15, LB_BAL_TRAITS = 11, LB_BAL_SPEED_FIELD = 4 };
// docs/426: a blueprint diffed against the player's own; fills msg; returns the
// number of values set, or -1 on failure.
int lb_balance_import_file(const char* path, char* msg, int msg_cap);
int lb_pick_balance_rom_file(char* buf, int cap);
int         lb_balance_field_count(int kind);
const char* lb_balance_field_name(int kind, int field);
int         lb_balance_field_max(int kind, int field);   // the field's range
int         lb_balance_record_first(int kind);
int         lb_balance_record_count(int kind);
int         lb_balance_record_name(int record, char* out, int cap);
int         lb_balance_record_category(int record);      // units: +0x5C
int         lb_balance_record_projectile(int record);    // units: +0x66, or -1
// The value in the game now, and the one beneath the player's (pack/builders).
// -1 when the blueprints are not in RAM, or the field does not apply (a
// building's speed).
int         lb_balance_value(int record, int field);
int         lb_balance_base(int record, int field);
// The player's set. value -1 clears one entry. Persisted through
// lb_balance_custom_text / _parse ("rec:field=value,...").
int         lb_balance_custom_get(int record, int field);   // -1 none
void        lb_balance_custom_set(int record, int field, int value);
void        lb_balance_custom_clear_all(void);
int         lb_balance_custom_size(void);
unsigned    lb_balance_custom_generation(void);   // bumps on every change
int         lb_balance_custom_text(char* out, int cap);
int         lb_balance_custom_parse(const char* text);
// The patcher's half (lb_patches.cpp): whether the set in force differs from
// what is written, and writing or taking it out.
int         lb_balance_custom_dirty(void);
int         lb_balance_custom_apply(unsigned int blueprint_base);
void        lb_balance_custom_revert(unsigned int blueprint_base);
void        lb_balance_custom_on_reset(void);
// Recordings (lb_demo.cpp): a replay brings its own set and its changes.
void        lb_balance_custom_play_begin(const char* text);
void        lb_balance_custom_play_set(int record, int field, int value);
void        lb_balance_custom_play_end(void);
// Why the player's values cannot change now (an online session), or null.
const char* lb_balance_custom_blocked(void);

// The address docs/95's patch found the in-RAM BPNZ block at, or 0.
unsigned int lb_patches_blueprint_base(void);
// docs/386: why the balance pack and same-builders cannot change now (a
// battle, a battle loading, a lobby), or null when they can.
const char* lb_blueprint_edit_blocked(void);
int lb_same_builders_applied(void);   // docs/386: equalised in the game now

// Called once per frame from the VBlank path; applies data-level patches.
void lb_patches_on_vblank(void);

// Hand the overlay this frame's TOP-screen framebuffer so it can be drawn as a
// movable, scalable panel inside the main window. The pointer
// is only borrowed for the current frame.
//
// The top screen has its own SDL_Renderer, and a texture belongs to the
// renderer that created it, so the pixels have to be re-uploaded to a texture
// owned by the bottom renderer rather than the existing one being reused.
void lb_overlay_set_top_pixels(const void* pixels, int width, int height);

// The rect, IN WINDOW PIXELS, that the bottom screen was drawn into this frame,
// and how many guest columns are inside it.
//
// Enhanced mode does not use SDL_RenderSetLogicalSize. Logical size scales
// mouse events as well as drawing, so with it set the guest touch mapping, the
// ImGui panels and SDL all disagreed about what a coordinate meant: ImGui was
// handed logical coordinates while believing its display was the whole window,
// which put the Top Screen panel off the bottom-right of the screen and made
// ImGui capture clicks the player had aimed at the game. Enhanced therefore
// keeps ONE coordinate space -- window pixels -- and maps clicks through this
// rect instead.
// The original implementation of a guest address, skipping this port's
// overrides, so an override can do extra work and then call what it replaced.
//
// Without it an override must reimplement the function it displaces, and for
// anything non-trivial that is guessing, which the project
// avoids -- Entity::SetOwner (func_020598C4) de-registers and re-registers the
// entity in team lists, and reproducing that by hand to add one line would risk
// exactly the kind of silent divergence docs/105 already cost.
//
// Returns NULL when the address has no dispatch entry.
void (*lb_dispatch_original(int cpu, unsigned int addr))(void);

void lb_overlay_set_field_rect(int x, int y, int w, int h, int guest_width,
                               int guest_height);
// BR-0010 / SG-0011: DS Vanilla's two screens, in WINDOW PIXELS. The layout
// is computed here from the window and the Display tab's two size sliders
// (top[] and bot[] are x, y, w, h); the rect that shows the touch screen is
// published for the mouse -> touch mapping (the top one while the screens are
// swapped), so one coordinate space serves drawing and clicks.
void lb_overlay_vanilla_layout(int win_w, int win_h, int w0, int w1, int* top,
                               int* bot);
int  lb_overlay_touch_rect(int* x, int* y, int* w, int* h);   /* 0 until published */

// How many of the guest's columns the window is actually SHOWING.
//
// The enhanced layout covers rather than letterboxes: the 448-wide surface is
// scaled to fill the window and whatever does not fit is cropped. So on a 16:9
// window only about 341 of those 448 columns are on screen, and the edge the
// player sees is not the edge of the surface.
//
// The HUD has to anchor to the VISIBLE edge, or a corner button moved to the
// surface's own edge lands in the cropped-off region and vanishes -- which is
// exactly the "hud buttons don't fully match the window size" report.
// The vertical half is the same story: once the window buys ROWS as well as
// columns, a bottom-anchored element is drawn extra_y rows above the bottom of
// the surface, because melonDS centres all 3D geometry and the in-field HUD is
// 3D geometry.
void lb_hud_set_visible_size(int columns, int rows);
// What the last call above reported: 0,0 until the layout has run once.
void lb_hud_visible_size(int* columns, int* rows);

// 1 while the cartridge itself draws the minimap's camera box at the widened
// view's size (lb_patches.cpp, apply_minimap_box; docs/370). The top-screen
// panel then shows the game's own box and neither scrubs nor overdraws it.
int lb_minimap_box_live(void);

// Report a click on a MOVED HUD element at its original position, so the
// cartridge's own hit test still finds it. gx is in surface columns.
// Returns 1 when the point was inside a moved element.
int lb_hud_unmap_touch(int* gx, int* gy);
// 1 when the point lands on the empty patch a MOVED HUD element used to
// occupy, where the cartridge's hit test still is. Only valid after
// lb_hud_unmap_touch has returned 0 -- see the note at its definition.
int lb_hud_touch_phantom(int gx, int gy);
// docs/371: the cartridge's widget hit test (FUN_020EA1A4) corrected per
// widget. Guest coordinates; see the definition for the three answers.
int lb_hud_hit_unmap(int x0, int y0, int x1, int y1, int* x, int* y);
// 1 while that correction is in force (lb_patches.cpp). The frontend then
// delivers true coordinates and neither rewrites nor drops a HUD click.
int lb_hud_hit_live(void);
// Whether the pen was already down before the event being handled. Lets a
// caller tell a fresh press from a drag continuation.
int lb_touch_is_down(void);

// The TRUE screen x of the current touch, which may be negative or past 255
// when the click is in a widened column. The DS touch controller cannot
// carry such a value, so it reaches the game through a different route --
// see apply_touch_extension in lb_patches.cpp.
void lb_touch_set_extended(int x);
/* The vertical twin. The touch widening was X-only until this; on a surface
   taller than 255 rows every Y wrapped modulo 256, which is why clicks above
   the band landed at the native top edge. */
void lb_touch_set_extended_y(int y);
void lb_touch_clear_extended(void);

// 1 = show every rendered column and bar the remainder; 0 = fill the window and
// crop the sides. See the Display tab.
// lb_setting_fit_all is removed -- it had no caller, so its LB_FIT knob did
// nothing. Adaptive window size is one toggle: lb_setting_enhanced.

// Guest pixels per window pixel; the surface is sized window/scale.
float lb_setting_pixel_scale(void);

// The world's pixel extent, from the guest's own bound rect (lb_patches.cpp).
// Returns 0 until a level is up. The frontend uses it to floor the pixel scale
// so the guest surface never exceeds the world -- which is both the zoom-out
// limit and the reason there are no black bars beside the map.
int lb_world_extent(int* w, int* h);

// Music. 0 vanilla, 1 random, 2 faction-aligned by builder,
// 3 map-specific, 4..9 always King/Wizard/Pirate/Imperial/Earth/Alien
// (docs/387); LB_MUSIC_MODE overrides. src/lb_music.cpp consumes it and
// carries the reading of where the cartridge chooses a track.
int  lb_setting_music_mode(void);
// docs/387: Level Palettes (0 Per-Mod, 1 Vanilla, 2 FactionWars, 3 Random) and
// Force Tileset (0 Off, 1 Castle, 2 Pirates, 3 Mars, 4 Random).
int  lb_setting_level_palette(void);
int  lb_setting_force_tileset(void);
// docs/391: lb_mods.cpp's summary of the mods folder, e.g. "18 mod map(s)
// from 3 source(s)".
const char* lb_mods_status(void);
// docs/413: the mod maps (lb_mods.cpp): how many, each one's lobby key (its
// name's LOC id), name and content hash, and the map holding a hash (-1).
int                lb_mods_count(void);
unsigned           lb_mods_name_id(int k);
const char*        lb_mods_name(int k);
unsigned long long lb_mods_hash(int k);
int                lb_mods_find_hash(unsigned long long hash);
// docs/413: a joiner's view of the host's mod maps, by lobby key: 1 listed
// (hash and name filled), 0 not listed, -1 the host sends no list, -2 no
// fresh rules yet (lb_teams.cpp).
int  lb_rules_host_map(unsigned key, unsigned long long* hash, char* name, int cap);
// docs/419: the second master server "Both" lists on and browses ("" none),
// and whether to try HTTPS first (lb_net.cpp, lb_overlay.cpp).
void lb_net_set_extra_server(const char* host, int port);
int  lb_setting_master_https(void);
// docs/420: fetch a host's mod map from the community archive (lb_mapfetch.cpp),
// when the setting allows; once per name per run.
void lb_mapfetch_request(const char* name);
int  lb_setting_fetch_maps(void);
int  lb_setting_fast_link(void);      /* docs/423 */
int  lb_relay_status(char* out, int cap);   /* lb_relay.cpp, docs/424 */
// docs/413: a message for the player that the game's own screens cannot show,
// drawn over the game whether or not the menu is open (lb_overlay.cpp).
void lb_overlay_notice(const char* text);
// docs/427: the notification sound (lb_sfx.cpp), on its own audio device.
int  lb_setting_notify_sound(void);    /* 0 off, 1 Group set (default) .. 8 */
int  lb_setting_notify_volume(void);   /* 0..100 */
const char* lb_sfx_choice_name(int i);
void lb_sfx_tick(void);
void lb_sfx_preview(void);
void lb_sfx_notify(void);              /* at most once per 300 ms */
// docs/428: the picture of a match for the master server's Discord log
// (lb_matchpic.cpp). Roster hook -> note_player; lb_net -> on_playing.
void lb_matchpic_note_player(int player, const unsigned char rec[20]);
void lb_matchpic_on_playing(void);     /* host only, once per match */
/* SG-0020 (lb_discord.cpp): Discord Rich Presence, once per frame. */
void lb_discord_tick(void);
int  lb_setting_discord_status(void);
int  lb_setting_discord_hero(void);
/* SG-0018 (lb_scores.cpp): a match's post-match statistics, observe only. */
typedef struct LbMatchStats {
    int valid; int won;
    int units_built, units_lost, units_killed;
    int buildings_built, buildings_lost, buildings_killed;
    int bricks;
} LbMatchStats;
int  lb_scores_player_stats(int player, LbMatchStats* out);   /* 1 if valid; -1 = unknown field */
void lb_scores_tick(void);          /* once per vblank (lb_patches.cpp) */
void lb_scores_match_begin(void);   /* reset; also automatic when a level comes up */
int  lb_scores_match_seconds(void); /* game ticks / 60 since the begin, -1 if none */
/* SG-0019: names (one per line) of the teams the game decided won / lost. */
int lb_matchpic_result(char* winners, int wcap, char* losers, int lcap);
/* docs/432: ask the host for a mod map this machine lacks (lb_maptx.cpp). */
void lb_maptx_want(unsigned long long hash, const char* name);
void lb_maptx_forget(void);
/* SG-0018: the /score report's fields (before lb_matchpic_result). */
int lb_matchpic_score_json(char* out, int cap);
/* SG-0018 (lb_net.cpp): a blocking GET to a master server; the reply's length, or
   -1 with the error in `out`. */
int lb_net_http_get(const char* host, int port, const char* path, char* out, int cap);
/* One on/off setting straight from settings.json, before the settings load. */
int lb_settings_file_bool(const char* key, int dflt);
/* SG-0018: Network > Scores. 1 when this player keeps out of the tables. */
int lb_setting_scores_opt_out(void);
void lb_matchpic_tick(void);           /* LB_MATCHPIC_TEST */
int  lb_setting_match_picture(void);
const char* lb_level_map_stem(void);   /* "mpNN" / "z/NN" / campaign stem */
int  lb_rom_read_unwrapped(const char* path, unsigned char* out, int cap);
int  lb_teams_hud_icon_rgba(int blueprint, unsigned int out[576]);
int  lb_teams_local_bans(unsigned char* bits, int n);
// docs/415: online chat and the swear filter (lb_chat.cpp). The bridge takes
// the datagrams to send and hands over the ones received; the overlay opens,
// closes and draws the chat box and toasts.
int         lb_chat_outgoing(unsigned char* buf, int cap);
void        lb_chat_incoming(const unsigned char* buf, int len, unsigned int sender);
int         lb_chat_online(void);
int         lb_chat_is_open(void);
void        lb_chat_open(void);
void        lb_chat_close(void);
void        lb_chat_draw(SDL_Renderer* r);
// The filter: always applied (1 when it changed something), and as shown
// (applied only while the setting is on; valid until the next call).
int         lb_swear_filter(const char* in, char* out, int cap);
const char* lb_swear_shown(const char* in);
/* docs/431 (lb_mods.cpp): the map folder's content hash (1 = ok); and whether it is
   already in the game's map lists (1) or needs a restart (0). Read-only, any thread. */
int lb_mods_folder_hash(const char* path, unsigned long long* hash);
/* docs/432 (lb_mods.cpp): Reset the game with the cartridge rebuilt from the mods
   folder as it is now (0 when no image was kept). */
int lb_mods_request_rebuild(void);
int lb_mods_register_folder(const char* path);
/* docs/431 (lb_mapfetch.cpp): extract the archive map with this content hash into
   mods/maps/_archive (1 + its folder path), BLOCKING: call from a worker thread. */
int lb_mapfetch_install_by_hash(unsigned long long hash, char* path_out, int cap,
                                char* err_out, int errcap);
void        lb_swear_selftest_if_asked(void);
int         lb_setting_swear_filter(void);
int         lb_setting_chat_toasts(void);
// Once per frame, from lb_patches_on_vblank (src/lb_palette.cpp).
void lb_palette_tick(void);
// Once per frame, from lb_patches_on_vblank.
void lb_music_tick(void);
// The side this port wants, or -1 for "leave the game alone".
int  lb_music_wanted_side(void);
// What is playing: the two words the cartridge reads, and the STRM base id its
// own table maps them to.
void lb_music_state(int* side, int* kind, int* strm);
// Multiplayer sets which player the local client is; 0 until it does, which is
// the local player in Free Play and in the campaign.
void lb_music_set_local_owner(int owner);
int  lb_overlay_field_rect(int* x, int* y, int* w, int* h, int* guest_width,
                           int* guest_height);

// Does this mouse position count as a press on the touch screen?
//
// The frontend gates SDL_MOUSEBUTTONDOWN on a 256-column band at band_left and,
// in the stacked layout, on y being below the top screen. Both are logical-space
// tests, so in Enhanced -- which uses window pixels and draws the field across
// the whole window -- they accept only a narrow strip and reject the rest of a
// field the player can plainly see. Enhanced tests the drawn rect instead.
// Vanilla keeps the original test exactly.
int lb_overlay_mouse_in_touch_area(int x, int y, int band_left, int separate);

// ---- docs/384: Team Builder and Ban Units (src/lb_teams.cpp) ---------------
//
// A team is the Free Play army screen's nine slots, as blueprint indices, in
// the screen's slot order: hero, builder, melee, ranged, mounted, siege one,
// siege two, siege three, transport.
enum { LB_TEAM_SLOTS = 9 };
typedef struct LbTeam { unsigned short pick[LB_TEAM_SLOTS]; } LbTeam;

void         lb_rom_attach(const unsigned char* data, size_t size);
// docs/387: a NitroFS file of the player's ROM as stored; its size or -1.
int          lb_rom_read(const char* path, unsigned char* out, int cap);
int          lb_teams_slot_count(int slot);
int          lb_teams_slot_unit(int slot, int k);
void         lb_teams_slot_pos(int slot, int* x, int* y);   // native px
const char*  lb_teams_slot_label(int slot);
const char*  lb_teams_unit_name(int blueprint);
int          lb_teams_unit_name_id(int blueprint);   // docs/385: bp+0x06
int          lb_teams_unit_faction(int blueprint);   // 0 King .. 5 Alien
int          lb_teams_unit_slot(int blueprint);      // -1 none
int          lb_teams_is_king_default(int blueprint);
// -1 unknown, 0 not unlocked, 1 unlocked but not bought, 2 fieldable.
int          lb_teams_unit_state(int blueprint);
SDL_Texture* lb_teams_atlas(SDL_Renderer* r, int grey);
int          lb_teams_icon_uv(int blueprint, float* u0, float* v0, float* u1,
                              float* v1);
const char*  lb_teams_icons_status(void);
// docs/408: the battle HUD's icons (UI/MiniHeadsGame via UI/Game/UIMgrData),
// one per blueprint 0..181; the Balance menu's.
SDL_Texture* lb_teams_hud_atlas(SDL_Renderer* r);
const char*  lb_teams_spell_name(int spell_id);   /* null until text loads */
int          lb_teams_hud_icon_uv(int blueprint, float* u0, float* v0, float* u1,
                                  float* v1);
void         lb_teams_shutdown(void);
void         lb_teams_king_default(LbTeam* t);
int          lb_teams_valid(const LbTeam* t);
int          lb_teams_is_default(const LbTeam* t);
int          lb_teams_read_save(LbTeam* t, int* confirmed);
const char*  lb_teams_set_team_blocked(void);   // null: Set Team may apply
int          lb_teams_set_team(const LbTeam* t, char* msg, int cap);
void         lb_teams_set_enemy(const LbTeam* t);
void         lb_teams_get_enemy(LbTeam* t);
void         lb_teams_randomize(LbTeam* t, int fieldable_only);
// The Custom/Random opponent (lb_patches.cpp): choices 0..5 are the six
// factions, 6 Custom, 7 Random.
int          lb_teams_enemy_frame(int choice);
int          lb_teams_enemy_choice(int frame);
// docs/390: 1 for the 24x24 fallback frames; the portraits' per-frame
// record check (lb_patches.cpp's vblank path).
int          lb_teams_enemy_frame_small(int frame);
void         lb_teams_portrait_tick(void);
int          lb_teams_enemy_army(int choice, unsigned char rec[20]);
// Bans: this machine's list (a host's), what a host sent (a client's), and
// the match-start substitution.
void         lb_teams_set_bans(const unsigned char* bits, int nbytes);
int          lb_teams_received_bans(unsigned char* bits, int nbytes,
                                    unsigned* from);
// Returns 0 (unchanged), 1 (a banned pick: the King army) or 2 (Random team
// (tourney): `player` fields a seeded random team). docs/429.
int          lb_teams_enforce_bans(unsigned char rec[20], char* why, int cap,
                                   int player);
const char*  lb_teams_loc_text(unsigned id);   /* the game's text, this language */
int          lb_rules_fresh(void);             /* a host's rules arrived lately */
// ---- docs/429: map bans, per-map unit bans, Random maps, ban presets
// (src/lb_mapbans.cpp). Maps are named by tokens: "s5" (stock map 5) and
// "h<16 hex>" (a mod map's content hash); a list is comma-separated, and
// the units banned on some maps only are "bp:tok+tok;bp:tok".
void lb_maps_set_local(const char* banned_maps, const char* unit_map_bans,
                       const char* preset_maps, int preset_active,
                       int tourney_team, int random_maps);
int  lb_maps_count(void);
int  lb_maps_entry(int i, char* token, int tcap, char* name, int ncap, int* players);
int  lb_maps_list_has(const char* list, const char* token);
void lb_maps_list_toggle(char* list, int cap, const char* token);
int  lb_maps_umb_get(const char* umb, int bp, char* toks, int cap);
void lb_maps_umb_set(char* umb, int cap, int bp, const char* toks);
void lb_maps_names_of(const char* toks, char* out, int cap);
int  lb_maps_preset_count(void);
const char* lb_maps_preset_name(int i);
int  lb_maps_preset_resolve(int i, char* units, int ucap, char* maps, int mcap,
                            char* umb, int bcap, char* msg, int gcap);
int  lb_maps_mission_banned(unsigned mission);
void lb_maps_fill_begin(void);
void lb_maps_pool_add(unsigned mission);
int  lb_maps_fill_extras(unsigned out[2]);
int  lb_maps_extra_count(void);
const char* lb_maps_extra_label(int idx);
unsigned lb_maps_extra_redraw(int idx);
void lb_maps_note_pick_id(unsigned name_id, int online);
void lb_maps_forget(void);
int  lb_maps_unit_banned_here(int bp);
int  lb_maps_tourney_team(unsigned* seed);
void lb_maps_new_seed(void);
unsigned lb_maps_current_seed(void);
int  lb_maps_received(char* maps, int mcap, char* umb, int ucap);
int  lb_maps_wire_out(unsigned char* buf, int cap);
int  lb_maps_wire_in(const unsigned char* p, int len, unsigned sender);
int          lb_rules_outgoing(unsigned char* buf, int cap);
void         lb_rules_incoming(const unsigned char* buf, int len,
                               unsigned int sender);
// docs/406: rules that change the simulation, so online every machine runs
// the host's: bits of the rules datagram's flags byte.
#define LB_RULE_EMP_TRANSPORTS 0x01
#define LB_RULE_CREATURES      0x02   /* docs/410 */
int          lb_rules_local_flags(void);   /* this machine's (lb_overlay) */
int          lb_rules_host_flags(void);    /* the host's; -1 unless joined */
int          lb_setting_emp_transports(void);
int          lb_emp_transports_live(void); /* the host's or this machine's */
// docs/406: the red-brick cheats as a mask (bit i = flags[i]); a replay's
// (-1 when not replaying one from sets=3), and the AI-gets-them option.
int          lb_demo_play_cheats(void);
int          lb_cheat_mask_live(void);
int          lb_setting_ai_cheats(void);
int          lb_demo_play_ai_cheats(void);
int          lb_ai_cheats_live(void);      /* lb_patches: offline only */
// docs/406: the AI rebuilds its hero after every death (kind 11).
int          lb_setting_ai_hero_rebuild(void);
int          lb_demo_play_ai_hero_rebuild(void);
int          lb_ai_hero_rebuild_live(void);
// SG-0017: the AI casts single-target spells (Balance > Spells). Offline only.
int          lb_setting_ai_tap_spells(void);
int          lb_setting_ai_spectate(void);   /* SG-0002 */
// docs/408: a hotwired transport leaves its old owner's AI (kind 12).
int          lb_setting_transport_release(void);
int          lb_demo_play_transport_release(void);
int          lb_transport_release_live(void);
// docs/410: the neutral creatures in armies (kind 13; online, the host's).
int          lb_setting_creatures(void);
int          lb_demo_play_creatures(void);
int          lb_creatures_live(void);
// BR-0011 / BR-0015: the rest of what changes the simulation, carried in an
// 'S' page of the host's rules (lb_teams.cpp). On a joiner these are the
// host's while joined, else this machine's.
typedef struct LbHostSettings {
    int hotwire_mode, hotwire_transport, transport_release, attack_boost,
        fix_healing;
    unsigned cheats;   /* bit i = cheat i */
    int caps[10];
} LbHostSettings;
/* -1: this machine decides (not joined); 0: joined, no 'S' page (an older
   host, or none yet): all zero; 1: the host's values. */
int          lb_rules_host_settings(LbHostSettings* out);
void         lb_rules_on_leave(void);      /* the lobby was left: forget its rules */
int          lb_demo_play_attack_boost(void);
int          lb_demo_play_fix_healing(void);
int          lb_attack_boost_live(void);
int          lb_fix_healing_live(void);
int          lb_host_rules_current(void);  /* the host's S settings are in force here */
const char*  lb_host_rules_why(void);
int          lb_setting_cheats_locked(void);
// docs/409: the field's size, recorded and replayed (lb_demo.cpp).
void         lb_demo_field(int* w, int* h);

#ifdef __cplusplus
}
#endif

#endif  // LB_OVERLAY_H
