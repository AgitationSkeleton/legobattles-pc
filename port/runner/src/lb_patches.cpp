// lb_patches.cpp - gameplay patches, installed as dispatch overrides.
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file; see
// port/runner/README.md.
//
// HOW A RECOMP CHANGES THE GAME
//
// The cartridge's code is lifted to C ahead of time, so there is nothing to
// patch at runtime the way an emulator would poke an instruction. Instead the
// runner looks a guest address up in a list of dispatch tables and calls the C
// function registered for it, and `nds_register_dispatch` APPENDS while lookup
// returns the FIRST match. So a table registered BEFORE the title banks wins,
// and `lsw2_register_dispatch_overrides` is upstream's hook for exactly that:
// it is called before the banks are registered and is weak, so this file's
// definition replaces the empty one.
//
// The name is upstream's (it was added for the LSW2 port). It is generic in
// effect and is the only hook of its kind, so this port uses it as-is rather
// than forking the runner further to rename it.
//
// EVERY OVERRIDE HERE MUST REPRODUCE THE ORIGINAL EXACTLY WHEN ITS TOGGLE IS
// OFF. Behaviour is read from the ROM, never guessed, and an override is a
// rewrite of shipped behaviour: if the "off" path is not the cartridge's own
// logic, the port is a different game whether or not anyone notices.

#include <cstring>      // std::strstr, for LB_TOUCHSKIP
#include <cctype>       // std::tolower: docs/396's keyboards

#include "lb_overlay.h"
#include "gpu3d.h"
#include "lb_field.h"
#include "lb_fxvtables.h"
#include "lb_shadow.h"

extern "C" {
#include "runtime_arm.h"
}

#include "bus.h"
#include "state.h"

#include <algorithm>    // std::clamp, docs/372
#include <array>        // docs/392: the detail fill's class histogram
#include <atomic>       // docs/383: a shortcut requested on the UI thread
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>       // docs/385: std::to_string
#include <vector>

extern "C" int lb_band_left(int output_width);   /* src/lb_widemap.cpp */
extern "C" int lb_band_right(int output_width);  /* src/lb_widemap.cpp */
extern "C" int lb_band_span(int output_width);   /* src/lb_widemap.cpp */
extern "C" int lb_band_top(int output_height);   /* src/lb_widemap.cpp */
extern "C" int lb_band_bottom(int output_height);/* src/lb_widemap.cpp */
extern "C" int lb_band_span_y(int output_height);/* src/lb_widemap.cpp */
extern "C" int lb_band_latch(int output_width, int output_height);
extern "C" unsigned lb_dump_frame_get(void);     /* src/lb_widemap.cpp */
extern "C" int lb_widemap_cam_px(int* x, int* y);/* src/lb_widemap.cpp */
extern "C" int lb_widemap_map_px(int* w, int* h);/* src/lb_widemap.cpp */
/* src/lb_overlay.cpp: the map chain captured where 0x02156CB0 was valid. */
extern "C" int lb_field_map_chain(uint32_t* map, int* w, int* h);
extern "C" int lb_field_camera_write(int map_x, int map_y);/* src/lb_field.cpp */
extern "C" uint32_t lb_field_camera_obj(void);   /* src/lb_field.cpp */
extern "C" int lb_music_wanted_side(void);       /* src/lb_music.cpp */
extern "C" int lb_widemap_cam_tile(int* tx, int* ty);
extern "C" int lb_band_render_cam(int* x, int* y);
extern "C" int lb_band_render(void);
extern "C" int lb_band_render_y(void);

// Declared BEFORE the anonymous namespace on purpose. Declaring it inside
// would make it `(anonymous)::lb_kill_test_run_if_armed` -- a different
// function from the one defined at file scope further down, which the
// linker reports only as `ld returned 1 exit status`.
void lb_kill_test_run_if_armed(void);
// Same reason: the extended-Y touch channel, defined at file scope further
// down, read by the drag-select guard inside the namespace (docs/344).
extern int g_ext_touch_y;

namespace {

// ── RETRACTED: func_020872DC is NOT a hotwire function ────────────────
//
// This file used to install an override on func_020872DC, called
// "HotWire_ResolveTarget", to implement "Hotwire Anything" and "Patch Hotwire
// Glitch". THAT WAS WRONG AND THE OVERRIDE IS REMOVED. Recorded here rather
// than deleted quietly: wrong findings are retracted in place.
//
// What func_020872DC actually is, read from the ROM:
//
//     Entity* ResolveLiveEntity(EntityHandle* h) {
//         Entity* e = ResolveEntityHandle(h);          // func_02087350
//         if (e && *(u8*)(*(u32*)(*(u32*)(e+0xE0)+4)+8) == 0) return e;
//         return NULL;
//     }
//
// func_02087350 takes a two-byte ENTITY HANDLE, not a TilePos: it looks the
// handle up in the entity pool at 0x02155028 via func_0205C9B4 and, when the
// entity is gone, writes a null handle back through the caller's pointer. The
// byte at [[e+0xE0]+4]+8 is a liveness/state test, not a hotwire target filter.
//
// It has 80 direct callers and 264 through its wrapper func_0208727C, among
// them FreezeEffect, TeleportEffect, LoadAction, RepairStructureAction and
// ConstructProgressAction. It is the game's universal handle-to-entity
// resolver. Overriding it applied a hotwire rule to the whole engine:
//
//   * "Patch Hotwire Glitch" returned NULL for every entity outside categories
//     5 and 6. func_020D1744 (the build/spell sidebar) resolves the selected
//     entity through func_0208727C, so selecting a builder would have yielded
//     NULL and THE RED SIDEBAR WOULD NEVER HAVE OPENED. Construct, repair,
//     freeze and teleport break the same way.
//   * "Hotwire Anything" dropped the liveness test engine-wide.
//
// Where the real hotwire lives (docs/105): Battles::Sim::HotWireSpell, vtable
// 0x0214A920. Slot 3 func_02078D5C is the tick; in state 0 it resolves the
// stored target handle at self+0x50 and calls func_020598C4 -- Entity::SetOwner
// -- with the caster's owner from func_02087390(self+0x18). Slot 5
// func_02078CFC records the target handle from entity+0xDC when none is set.
// NEITHER APPLIES A CATEGORY FILTER. The validation layer IS located now:
// func_0207B73C, hotwire arm at 0x0207B790, which reads the target category
// from [+0x160]->[+8]->[+0x5C] and compares it to 6. See kHotwireSpecialsOnly.
// It also settles what vanilla actually restricts: NOT a category whitelist --
// there is none in the game -- but a stolen special the farms cannot support.

// ── Hotwire Kills Transport Occupants ──────────────────────────────────────
//
// The requirement: when a transport is hotwired, either destroy its occupants or
// convert them to the new owner. Vanilla leaves them aboard as hostages, which
// the LEGO Battles community treats as a wart (the area darkens and the units
// are stuck until the hijacker unloads or destroys the transport).
//
// WHAT IS OVERRIDDEN, AND WHY IT IS THIS FUNCTION
//
// func_020598C4 is Entity::SetOwner(Entity*, const u8* owner) -- read in
// docs/108 and identified by HotWireSpell::tick calling it with the caster's
// owner (docs/105). Hotwiring IS a change of owner, so every hotwire passes
// through here, and so does every other ownership change the game makes. The
// override therefore adds work only when the entity is a TRANSPORT whose owner
// is actually changing, and is otherwise the cartridge's own function.
//
// HOW THE ORIGINAL STILL RUNS
//
// lb_dispatch_original returns the title bank's implementation, which is
// TAIL-CALLED as the last thing here. A recompiled body is resumable -- it can
// return mid-function on a scheduler yield and be re-entered at a label
// (docs/108) -- so nothing may follow it. The "off" path is therefore the
// original because it IS the original, not a hand-copy hoping to match.
//
// WHY RE-ENTRY IS SAFE
//
// The occupant conversions are issued with runtime_dispatch, which runs the
// callee synchronously but can unwind on a slice yield. When it does, this
// function returns and is later re-entered from the top, redoing work it had
// already done. That is harmless because SetOwner's own first act is
//
//     if (e->owner == owner[0]) return;
//
// so converting an already-converted occupant is a no-op. The sequence is
// idempotent by the cartridge's own construction, not by an assumption here.
//
// OCCUPANTS ARE FOUND BY THE BACK-REFERENCE, NOT A LIST
//
// entity+0x114 is the two-byte handle of the transport an entity is riding in
// (docs/108: UnloadAction::virtual_02 tests it against the null handle and
// resolves it; func_0205BE04 defers to it for position). There is no forward
// transport->occupants list and none is needed: the occupants of a transport
// are the entities whose +0x114 names it.

constexpr uint32_t kSetOwner = 0x020598C4u;
constexpr uint32_t kEntityPoolPtr = 0x02155028u;   // -> pool; +8 array, +0xC count
constexpr uint8_t  kCategoryTransport = 5u;
// docs/72: category 6 is Siege/Special -- Ballista, Catapult, Dragon.
constexpr uint8_t  kCategorySpecial   = 6u;

// Return from an override WITHOUT running the function it replaced.
//
// docs/134 recorded this as the one missing mechanism, and it is not a guess:
// it is exactly what the recompiler emits for a returning BX, from
// arm_codegen.cpp:
//
//     g_cpu.R[15] = <target>;
//     if (runtime_call_should_return(g_cpu.R[15])) return;
//     runtime_dispatch_with_exchange(target);
//     return;
//
// runtime_call_should_return matches the address against the call-return stack
// that runtime_call_push_return filled, pops the frame, and reports that the
// host frame should simply unwind. Declining a call is therefore a normal guest
// return, performed the way every recompiled epilogue performs it -- not an
// invented convention.
//
// Returns true when the caller may `return` immediately.
// LB_TRACE_DISPATCH=1 reports the first few times each override actually runs.
// This is how docs/175 established that overrides on GAMEPLAY addresses do fire,
// against docs/143's conclusion that they never do.
bool lb_trace_dispatch(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_TRACE_DISPATCH") ? 1 : 0;
    return on != 0;
}

bool lb_override_return(void) {
    const uint32_t lr = g_cpu.R[14];
    g_cpu.R[15] = lr;
    if (runtime_call_should_return(g_cpu.R[15])) return true;
    runtime_dispatch_with_exchange(lr);
    return true;
}

// -- Hotwire targets: "Patch the glitch (specials only)" --------------------
//
// The targeting layer is LOCATED: func_0207B73C, hotwire arm at 0x0207B790.
// The approach below -- refusing the ownership change at the effect point --
// was abandoned because its gate can never hold; see the note in
// set_owner_override. HotWireSpell's tick
// calls Entity::SetOwner on the handle at self+0x50, which is the EFFECT point,
// and refusing the ownership change there enforces the rule whatever targeting
// offered.
//
// "Specials" is category 6, Siege/Special -- Ballista, Catapult, Dragon --
// confirmed in docs/72 from the function that reacts to unit counts, not
// inferred from the blueprint table. Category 5 is Transport Ship and is
// deliberately NOT included: the mode is named for specials, and transports have
// their own separate setting.
constexpr uint32_t kHotWireTick = 0x02078D5Cu;   // HotWireSpell virtual_03
bool g_in_hotwire_tick = false;

void hotwire_tick_override(void) {
    { static unsigned f = 0u; if (lb_trace_dispatch() && ++f <= 2u) std::fprintf(stderr, "[lb_disp] hotwire_tick_override FIRED (%u)\n", f); }
    const bool was = g_in_hotwire_tick;
    g_in_hotwire_tick = true;
    void (*original)(void) = lb_dispatch_original(0, kHotWireTick);
    if (original) original();
    g_in_hotwire_tick = was;
}


// ── Observer: does a band click ever reach the move issuer? ────────────────
//
// GOAL WITHIN THE PROJECT: the widened field must be just as interactable as
// the native one -- tapping or COMMANDING a unit. docs/238 measured that
// it is not: a move order to a destination in the band issues no command, while
// the same order to a native destination walks the unit there. Selection out in
// the band works 4 of 4, so the extended coordinate arrives and the pick uses
// it -- the click simply never becomes a move.
//
// func_020A8F28 is the move issuer: it copies the destination point, builds a
// one-entry entity array, and calls the MoveEntitiesCommand factory
// (func_02005884, tag 0x17) followed by the submit func_020EB910 that docs/137
// verified. It is bound to event id 0x0B by the handler table in
// func_020A889C, and 0x0B is raised out of a state machine (func_020AA130's
// transition table) rather than posted directly.
//
// That is why this is an OBSERVER and not a patch. The question it answers is
// the one static reading cannot: does this function run at all for a band
// click? If it never fires, the state machine is not transitioning and the
// cause is upstream of everything already understood. If it fires with a
// destination, the order is being made and rejected later.
//
// It only logs, and only with LB_ORDERLOG=1, so it cannot change behaviour --
// which matters, because an observer override was once wrongly blamed
// for a regression that turned out to be a ~50% race.
constexpr uint32_t kMoveIssue = 0x020A8F28u;   // thumb

void move_issue_observer(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_ORDERLOG") ? 1 : 0;
    if (on) {
        // r0 is the destination point on entry. Print the two halfwords it
        // holds and the live camera, so the logged order can be compared
        // against the click that was asked for.
        const uint32_t pt = g_cpu.R[0];
        int px = 0, py = 0;
        if (pt >= 0x02000000u && pt < 0x02400000u) {
            px = (int)(int16_t)bus_read_u16(pt);
            py = (int)(int16_t)bus_read_u16(pt + 2u);
        }
        int cx = 0, cy = 0;
        const bool have_cam = lb_field_camera(&cx, &cy);
        std::fprintf(stderr,
                     "[lb_order] move issued: point=%08X (%d,%d) cam=(%d,%d)%s\n",
                     pt, px, py, cx, cy, have_cam ? "" : " (no camera)");
        std::fflush(stderr);
    }
    void (*original)(void) = lb_dispatch_original(0, kMoveIssue);
    if (original) original();
}

// ---- item #4: WHO ACTUALLY PICKS, and what rect it is given -----------------
//
// The defect is still open: in the touch-screen expansion the selection box
// cannot reliably be clicked and dragged, nor can a click land outside the
// original aspect.
//
// docs/224 and docs/260 between them established the state of it: the extended
// coordinate ARRIVES (the delivered slot holds -38), is stored correctly as a
// signed halfword, and is normalised correctly into a selection rect at
// obj+0x10..0x1a by func_020aa8e4 -- a pure min/max on signed compares. The
// entities out there are gathered. And the cartridge still does not act on it:
//
//     in band,  native x 123:   hud 0 -> 6   SELECTED
//     out band, native x -37:   hud 0 -> 0   nothing, slot held -38
//
// docs/260 then followed the consumer statically and hit a wall worth naming:
// the reader of that rect is dynamic dispatch. func_020aa8d4 is
//
//     020aa8d4  ldr  r2, [r0, #4]      ; a function pointer IN the object
//     020aa8d8  cmp  r2, #0
//     020aa8da  beq  #0x20aa8e0
//     020aa8dc  ldr  r1, [r0, #8]      ; its bound argument
//     020aa8de  blx  r2
//
// so "find what reads obj+0x10" has no static answer -- the reader is whatever
// [obj+4] happens to hold, installed by whoever configured the object. The
// recorded next step was to watch that blx at runtime, and run_to_pc refuses in
// play mode ("the frontend owns execution"), which is where it stopped.
//
// AN OBSERVER AT THE FUNCTION ENTRY ANSWERS IT WITHOUT AN EXEC WATCH. The very
// first instruction of func_020aa8d4 loads [r0+4], so on entry r0 already points
// at the object and the target can simply be read out of guest memory, together
// with the rect the picker is about to be handed. That turns "the identity is
// data, not code" from a wall into one memory read.
//
// LOGS ONLY, behind LB_CLICKDISPATCH=1, and calls the original -- the same
// contract as move_issue_observer above, and for the same reason recorded
// there.
// THE CONTROL FOR THE OBSERVER BELOW, and it has to exist before anything is
// read into a silent probe.
//
// func_020a8794 is the touch handler docs/224 and docs/260 both followed: it
// reads the touch x twice (020A8800 ldrsh feeds the rect builder), calls
// func_020aa8e4 to normalise the rect, and installs a callback via
// func_020aa0a4. It is ON the click path by construction. If this one speaks
// and kClickDispatch does not, the chain is live and 0x020AA8D4 is simply not
// where a drag-select goes; if NEITHER speaks, the override is not catching
// these addresses and no conclusion about the cartridge can be drawn from
// their silence. That distinction is the whole reason this entry exists --
// "the probe printed nothing" has been read as a finding in this project
// before (see blank-readout notes in docs/259 and lb_widemap.cpp).
constexpr uint32_t kTouchRect = 0x020A8794u;      // thumb

// AND IT IS THE PROBE, not just the control.
//
// The control did its job: this fires 40 times on a click sweep while
// kClickDispatch below fires ZERO, so the override mechanism works and
// func_020aa8d4 is simply never called on a click path -- docs/260's
// reconstructed caller chain is not the one taken. 0x020AA8D4 is declared as
// its own function in port/generated/lb_arm9.h, so that is a fact about the
// game rather than about the lift.
//
// Which makes the indirect branch unnecessary to watch. r0 here IS the object
// docs/260 identified as the stack local at sp+0x40 (027E35E4, constant across
// calls, which is what a stack local looks like). By the time this function
// RETURNS, func_020aa8e4 has normalised the touch into the rect at
// obj+0x10..0x1a and func_020aa0a4 has installed a callback at obj+4 -- or has
// not, which is the answer to "does the cartridge act on an out-of-band
// coordinate".
//
// So: save r0, run the original, then read the object back. r0 is clobbered by
// the call, which is why it is saved rather than re-read.
void touch_rect_observer(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_CLICKDISPATCH") ? 1 : 0;
    const uint32_t obj = g_cpu.R[0];
    // WHICH GUARD REJECTS THE CLICK.
    //
    // All four early exits test r4 -- the input event -- and not a coordinate,
    // so the two that are plain loads can be read right here, at entry, before
    // the function runs. (The first guard is a call, func_020af040, whose
    // verdict is not visible from this hook; if these two are both clear it is
    // the remaining suspect.) Captured BEFORE original() because r1 is
    // clobbered by the call, the same reason obj is saved.
    const uint32_t evt = g_cpu.R[1];
    uint32_t evt_12 = 0xFFFFFFFFu, evt_29 = 0xFFFFFFFFu;
    if (evt >= 0x02000000u && evt < 0x02800000u) {
        evt_12 = bus_read_u32(evt + 12u);
        evt_29 = bus_read_u8(evt + 29u);
    }
    void (*original)(void) = lb_dispatch_original(0, kTouchRect);
    if (original) original();
    if (on && obj >= 0x02000000u && obj < 0x02800000u) {
        const uint32_t cb = bus_read_u32(obj + 4u);
        const uint32_t arg = bus_read_u32(obj + 8u);
        const int x0 = (int)(int16_t)bus_read_u16(obj + 0x10u);
        const int y0 = (int)(int16_t)bus_read_u16(obj + 0x12u);
        const int x1 = (int)(int16_t)bus_read_u16(obj + 0x18u);
        const int y1 = (int)(int16_t)bus_read_u16(obj + 0x1Au);
        // Print a change in the (callback, out-of-band) pair rather than every
        // call: the question is which handler an out-of-band rect gets, and a
        // per-frame flood of identical in-band lines answers it no better
        // while costing an fflush on a redirected handle.
        // KEY THE RATE LIMIT ON THE TOUCH, NOT ON THE OUTCOME.
        //
        // The first version keyed it on (callback, out-of-band) and printed ONE
        // line for a whole run, because both stayed constant at zero -- so what
        // survived was the first call, which had no touch active, and its
        // "rect 0..0" said nothing about any click. The interesting calls were
        // the ones it suppressed.
        //
        // These are the cartridge's own touch globals: the handler loads them
        // at 020A880C/020A8810 through the literals at 020A882C/020A8830, and
        // docs/214 named them. Keying on them means one line per distinct touch
        // point, which is exactly one per click.
        const int tx = (int)(int16_t)bus_read_u16(0x0215376Au);
        const int ty = (int)(int16_t)bus_read_u16(0x0215376Cu);
        const bool oob = (x0 < 0 || x1 < 0 || x0 > 255 || x1 > 255 ||
                          tx < 0 || tx > 255);
        static int last_tx = 0x7FFFFFFF, last_ty = 0x7FFFFFFF;
        static uint32_t last_cb = 0xFFFFFFFFu;
        if (tx != last_tx || ty != last_ty || cb != last_cb) {
            last_tx = tx;
            last_ty = ty;
            last_cb = cb;
            std::fprintf(stderr,
                         "[lb_touchrect] touch=(%d,%d) cb=%08X arg=%08X  "
                         "rect x %d..%d y %d..%d  evt=%08X +12=%08X "
                         "+29=%02X%s%s\n",
                         tx, ty, cb, arg, x0, x1, y0, y1,
                         evt, evt_12, evt_29 & 0xFFu,
                         (evt == 0 || evt_12 != 0 || (evt_29 & 0xFFu) != 0)
                             ? "  <- GUARD REJECTS" : "",
                         oob ? "  <- OUT OF BAND" : "");
            std::fflush(stderr);
        }
    }
}

// ── The drag rectangle, drawn by this port because the cartridge cannot ────
//
// The selection box is NOT geometry. func_0203b7fc clears a buffer to tile
// 0x1f and then writes halfword tile indices into it -- 0x0d/0x0e/0x1d/0x1e/
// 0x2d/0x2e, a nine-slice border -- through func_0203d8e8, indexing it with
// the corner coordinates shifted right by three. It is an 8x8 TILEMAP on the
// guest's own 32x24 screen grid, and before any of that it clamps x into
// 0..255 and y into 0..191.
//
// So the box cannot leave the native 256 columns for the same reason the DS
// cannot: there are only 32 tile cells across. docs/275 measured the symptom
// from outside -- every drawn box anchored inside the band whatever pixel the
// press was at, and the anchor tracked (extended x) modulo 256 exactly -- and
// the tile loop is why. No instruction patch reaches it: widening the clamp
// would index tile cells that do not exist.
//
// The port therefore draws its own, over the finished surface, from the
// coordinates the frontend already has in surface space. Two things make that
// honest rather than a guess at when a box should appear:
//
//   * WHEN. This function runs once per frame and only while the box is up --
//     its first four instructions test a flag and take an entirely different
//     path when set. Reading that same flag here means the port's box appears
//     on exactly the frames the cartridge's own would, with no reimplementation
//     of the gesture rules and nothing to drift out of step with them.
//
// STILL OPEN: the guest's own box is not suppressed, so out of band there are
// two -- this one at the press, and the cartridge's clamped one inside the
// native columns. Captured side by side: the guest's is YELLOW (which is why
// the port's is drawn yellow) and sits exactly at
// band + (extended x mod 256), the byte-truncated position docs/275 measured.
//
// TWO SUPPRESSION ATTEMPTS FAILED, and what they ruled out is worth more than
// the code was. Both cleared the tilemap this function writes: first at
// 0x021530A0 for the 0x680 bytes its own head clears, then through the ROW
// POINTER TABLE the tile writes actually use --
//
//     func_0203d8e8:  ldr r2, [pc, #8]           = 0x021520f8
//                     ldr r1, [r2, r1, lsl #2]   row = table[y]
//                     add r0, r1, r0, lsl #1     cell = row + x * 2
//
// -- which resolves to that same buffer (measured live: row0=021530A0,
// row1=021530E0, a 0x40 stride for 32 halfword cells). Clearing all 832 cells
// after the original ran changed nothing, on the guarded path and on both
// paths. So the visible box is NOT this tilemap. The remaining suspect is the
// second half of the function, which converts the tile coordinates back to
// pixels (`lsl #3`), runs a three-state machine and indexes a table at
// 0x02152160 -- the shape of OAM sprite entries. Identifying that is the next
// step, and it is why this function is NOT simply skipped: it does far more
// than draw a rectangle, so refusing to call it would take the rest with it.
constexpr uint32_t kMarquee = 0x0203B7FCu;   // arm
// THE TRANSFER, which is where the guest's box can actually be stopped.
//
// Both earlier attempts cleared the staging buffer AFTER the function
// returned, and the function's own tail is why that could never work:
//
//     0203be34  ldr r0, =0x021530a0     the staging buffer
//     0203be40  mov r1, #0x680          size
//     0203be48  bl  func_0210eaf4       flush
//     0203be7c  ldr r2, =0x021530a0     source
//     0203be84  mov r3, #0x680          size
//     0203be8c  bl  func_0204e0e0       queue the VRAM transfer
//
// It draws, flushes and SENDS, all before the exit hook gets to run. Clearing
// the buffer afterwards tidied memory the hardware had already copied out of.
//
// So the hook goes on the transfer instead, gated on exactly this source and
// size -- func_0204e0e0 is generic and everything else that uses it must be
// left alone. r2 is the source and r3 the length, read off the call site
// above, and the buffer is blanked just before it is handed over.
constexpr uint32_t kGfxTransfer = 0x0204E0E0u;   // arm
constexpr uint32_t kMarqueeMap = 0x021530A0u;
constexpr uint32_t kMarqueeMapBytes = 0x680u;

// Frames of life left. The observer runs during the guest's frame and the draw
// happens when the surface is handed over, so a flag set and cleared in the
// same breath would race; a small countdown the draw decrements is enough and
// cannot leave the box stuck on -- it decays in three frames if the function
// stops being called.
//
// DECLARED HERE, ahead of gfx_transfer_observer, because that observer needs it
// to tell two different senders of the same transfer apart. See the note there.
int g_mq_ttl = 0;

bool marquee_hide_guest() {
    static const bool on = [] {
        const char* v = std::getenv("LB_MARQUEE_KEEPGUEST");
        return !(v && v[0] != '0');
    }();
    return on;
}

// THE BUILDING PLACEMENT FOOTPRINT SHARES THIS BUFFER, and blanking it while a
// building is being sited erases the footprint the guest just painted.
//
// The building UI could become dishevelled when clicking around to place a
// building, and this is a mechanism that produces exactly that: intermittent, and only while placing.
//
// func_020370B4 paints the footprint into 0x021530A0 and func_020396E0 uploads
// it directly via func_0210CBE0, which is NOT the transfer this observer hooks
// -- so the common frame is safe. But three other sites DO push this same
// buffer and length through func_0204E0E0: the marquee's own tail at
// 0x0203BE8C, func_02038890 at 0x0203897C, and func_0203C78C at 0x0203C9AC --
// which is the SELECTION-MARKER tilemap pass, NOT the debug-text overlay this
// comment previously called it. In any frame where one of those runs after the
// footprint was painted, the blank below wipes all 832 cells.
//
// The guest's own placement gate is the byte func_020DDDD4 tests before it
// touches the placement object: view+0x78, where view = *(u32*)0x02152034.
// (The placement state lives at view+0x6C and its "active" byte is +0x0C, so
// +0x6C +0x0C = +0x78 -- the same byte, reached two ways.) When it is set, the
// port must leave the buffer alone.
// docs/390: THE IMAGE UPLOAD. FUN_0203242C decodes an image file, flushes the
// pixels and queues their transfer with FUN_0204E1D0(kind, vram, desc, obj,
// 0, image+0x8C, name): desc+0x14 is the pixel buffer, desc+0x10 its size,
// and the seventh argument (entry sp+8) the image's name, FUN_020F1FA0's
// c_str of image+0x90. For UI/AllInOne/Iconic_FreeplayFaction the Custom and
// Random portraits are pasted into the buffer here, before it leaves RAM
// (lb_teams.cpp). Iconic_FactionSelect holds the same pixels and is left
// alone: its grey cells are its locked factions.
constexpr uint32_t kImageXfer = 0x0204E1D0u;   // arm
extern "C" int lb_teams_paste_portraits(uint32_t buf);   /* lb_teams.cpp */
void image_xfer_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kImageXfer);
    const uint32_t desc = g_cpu.R[2];
    const uint32_t name = bus_read_u32(g_cpu.R[13] + 8u);
    auto ram = [](uint32_t p) { return p >= 0x02000000u && p < 0x02400000u; };
    if (ram(desc) && ram(name) && bus_read_u32(desc + 0x10u) == 65536u) {
        char s[64];
        int n = 0;
        for (; n < 63; ++n) {
            const char ch = (char)bus_read_u8(name + (uint32_t)n);
            if (!ch) break;
            s[n] = ch;
        }
        s[n] = 0;
        if (std::strstr(s, "Iconic_FreeplayFaction")) {
            const int ok = lb_teams_paste_portraits(bus_read_u32(desc + 0x14u));
            static int said = 0;
            if (said < 3) {
                ++said;
                std::fprintf(stderr, "[lb_enemy] %s: Custom and Random "
                             "portraits %s\n", s, ok ? "pasted" : "NOT pasted");
                std::fflush(stderr);
            }
        }
    }
    if (original) original();
}

bool placement_active() {
    const uint32_t view = bus_read_u32(0x02152034u);
    if (view < 0x02000000u || view >= 0x02400000u) return false;
    return bus_read_u8(view + 0x78u) != 0u;
}

void gfx_transfer_observer(void) {
    const uint32_t src = g_cpu.R[2];
    const uint32_t len = g_cpu.R[3];
    // TWO DIFFERENT PASSES SEND THIS EXACT TRANSFER, and blanking on the
    // source alone suppressed both.
    //
    // The comment below called func_0203C78C "the debug-text overlay". It is
    // not: it is the SELECTION-MARKER tilemap pass -- the routine that stamps a
    // 3x2 marker under every entity whose highlight byte (+0x71) is set, over a
    // 10x12 metatile window around the camera. It issues
    // func_0204E0E0(0x16, .., 0x021530A0, 0x680) at 0x0203C9AC, which is the
    // same source, the same length and the same r0 as the marquee's own
    // transfer at 0x0203BE8C. The register values cannot tell them apart.
    //
    // So with the guest marquee hidden (the default) the port was also erasing
    // the selection markers, every frame that pass ran.
    //
    // g_mq_ttl is the discriminator and it already exists: marquee_observer
    // sets it to 3 only when the guest's own box-drawing function actually
    // runs, and it decays. Non-zero means a marquee transfer is what is in
    // flight; zero means this transfer belongs to somebody else and must be
    // left alone.
    if (src == kMarqueeMap && len == kMarqueeMapBytes && marquee_hide_guest() &&
        !placement_active() && g_mq_ttl > 0) {
        // THE GAME'S OWN CLEAR VALUE, SAMPLED, NOT GUESSED.
        //
        // The head of the drawing function clears this buffer with
        // `mov r0,#0x1f`, but whether that lands as 0x001F or 0x1F1F depends
        // on whether the helper fills bytes or halfwords, and getting it wrong
        // paints 832 cells of garbage. The map is 32x26 while the box is
        // clamped into the screen's 32x24, so the LAST row can never contain
        // any part of it -- its final cell is always whatever "empty" is.
        const uint16_t blank = bus_read_u16(src + len - 2u);
        for (uint32_t o = 0; o + 1u < len; o += 2u)
            bus_write_u16(src + o, blank);
    }
    void (*original)(void) = lb_dispatch_original(0, kGfxTransfer);
    if (original) original();
}

int g_mq_x0 = 0, g_mq_y0 = 0, g_mq_x1 = 0, g_mq_y1 = 0;
bool g_mq_down = false;

void marquee_observer(void) {
    // The guard the function's own first four instructions test: it loads
    // *(0x02157E3C + 0x170), reads the byte at +0x22C, and branches to a
    // different routine entirely when it is non-zero. Read BEFORE original(),
    // because the call is free to change it.
    bool draws = false;
    const uint32_t owner = bus_read_u32(0x02157E3Cu + 0x170u);
    if (owner >= 0x02000000u && owner < 0x02800000u)
        draws = (bus_read_u8(owner + 0x22Cu) == 0u);
    // docs/344 LB_MQTRACE: WHAT THE BOX ROUTINE IS ABOUT TO USE, read at its
    // entry. A drag below the native rows writes the box's bottom border
    // (0x2d/0x2e) into ITCM over the interrupt dispatcher; clamping the touch
    // panel to 0..191 did NOT stop it. So print the inputs the decomp shows it
    // indexing with: the drag object's start/current corners and stored
    // camera (object via the literal at 0x0203BEAC), the live camera (R0 =
    // view, +0x5E/+0x60), the row indices it will compute, and the row-pointer
    // table (literal at 0x0203D8F8) including the words just past its 24.
    {
        static const bool trace = std::getenv("LB_MQTRACE") != nullptr;
        if (trace && draws) {
            const uint32_t view = g_cpu.R[0];
            const uint32_t pp = bus_read_u32(0x0203BEACu);
            const uint32_t obj = (pp >= 0x02000000u && pp < 0x02800000u)
                                     ? bus_read_u32(pp) : 0u;
            const uint32_t tb = bus_read_u32(0x0203D8F8u);
            if (obj >= 0x02000000u && obj < 0x02800000u &&
                view >= 0x02000000u && view < 0x02800000u) {
                const int sx = bus_read_u8(obj + 6u), sy = bus_read_u8(obj + 7u);
                const int cx = bus_read_u8(obj + 8u), cy = bus_read_u8(obj + 9u);
                const int scx = (int16_t)bus_read_u16(obj + 10u);
                const int scy = (int16_t)bus_read_u16(obj + 12u);
                const int vcx = (int16_t)bus_read_u16(view + 0x5Eu);
                const int vcy = (int16_t)bus_read_u16(view + 0x60u);
                // The start corner exactly as func_0203B7FC computes it.
                int ux = ((sx - 4) & 0xFF) + (scx - vcx);
                int uy = sy + (scy - vcy);
                ux = ux < 0 ? 0 : (ux > 255 ? 255 : ux);
                uy = uy < 0 ? 0 : (uy > 191 ? 191 : uy);
                const int ymin = cy < uy ? cy : uy, ymax = cy < uy ? uy : cy;
                std::fprintf(stderr,
                    "[lb_mq] frame %u start %d,%d cur %d,%d scam %d,%d "
                    "vcam %d,%d -> y rows %d..%d  table %08X",
                    lb_dump_frame_get(), sx, sy, cx, cy, scx, scy, vcx, vcy,
                    ymin >> 3, ymax >> 3, tb);
                if (tb >= 0x02000000u && tb < 0x02800000u) {
                    std::fprintf(stderr, "  t[22..27]");
                    for (int i = 22; i < 28; ++i)
                        std::fprintf(stderr, " %08X",
                                     bus_read_u32(tb + 4u * (uint32_t)i));
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    // docs/344 THE DRAG-SELECT CRASH. func_0203B7FC draws the box's nine-slice
    // border as halfword tiles through func_0203D8E8, cell = table[y >> 3] +
    // x * 2, and table has 24 rows (0x021520F8). It clamps the START corner to
    // 0..191 but takes the CURRENT corner's y straight from the drag object's
    // byte at +9. On a DS that byte cannot exceed 191 -- the panel has no row
    // 192. This port's extended-Y touch channel (kOrderFix group 128) now
    // writes the true below-native y there, so a drag into the widened rows
    // reached +9 = 205..211 (measured, LB_MQTRACE), rows 25..26, and the words
    // past the table -- 0x00000000 and 0xFF000000 -- sent the 0x2d/0x2e border
    // into ITCM at address 0 (the mirror of 0x01FF8000). The interrupt
    // dispatcher was overwritten, its lifted code refused, the game fell back
    // to the interpreter and died on an ITCM miss: the game slowed down until
    // it failed, docs/343 item 1.
    //
    // THE CLAMP STAYS. The first version clamped, called, and restored the
    // true value -- scoped to this call so nothing else would lose the
    // extended y. It did not stop the crash. Measured (LB_MQTRACE): the clamp
    // took (read back 191), yet ITCM was corrupt by the next call; with the
    // routine skipped outright it stayed intact, and with the clamp LEFT IN
    // PLACE both repro drags (to native y 230 and 300) survived with 0 ITCM
    // rejections and a normal frame rate after release (58-59 fps against the
    // failing run's 28). So something later in the frame reads the restored
    // out-of-range byte; which, is not identified.
    //
    // Leaving it clamped is safe for selection: the port's extended-Y readers
    // (kOrderFix group 128) take the row from the 16-bit shadows, not from this
    // byte, so the drag rectangle still reaches the rows outside the native
    // field. The byte only has to stay inside the cartridge's own 0..191.
    //
    // CORRECTED (docs/348): that was NOT true when written. The selection
    // rectangle's current corner is loaded from this very byte at 020AF1BC
    // (the drag in progress) and 020AF310 (pen-up), and neither was in group
    // 128 -- so a drag reaching outside the native rows selected against a
    // row wrapped modulo 256, and after this clamp against 0 or 191. Both are
    // in group 128 now, which is what makes the sentence above true.
    {
        const uint32_t pp = bus_read_u32(0x0203BEACu);
        if (pp >= 0x02000000u && pp < 0x02800000u) {
            const uint32_t o = bus_read_u32(pp);
            if (o >= 0x02000000u && o < 0x02800000u) {
                const int y = bus_read_u8(o + 9u);
                if (y > 191) {
                    // Which native edge: the true row is on the port's own
                    // extended-Y channel. Negative (above the native rows)
                    // arrives in this byte as 256 + y, which is ALSO above
                    // 191 and overflows the table the same way, so the side
                    // is taken from the sign of the true value, not the byte.
                    const int top = (g_ext_touch_y != INT32_MIN &&
                                     g_ext_touch_y < 0);
                    bus_write_u8(o + 9u, top ? 0u : 191u);
                    if (std::getenv("LB_MQTRACE"))
                        std::fprintf(stderr,
                            "[lb_mq] clamp +9 %d -> %d (ext y %d)\n", y,
                            top ? 0 : 191, g_ext_touch_y);
                }
            }
        }
    }
    void (*original)(void) = lb_dispatch_original(0, kMarquee);
    if (original) original();
    if (draws) g_mq_ttl = 3;
}

// Published by the frontend's touch mapping in SURFACE coordinates -- the same
// space the box is drawn in, so nothing has to be converted back through a band
// that has already caused two defects (docs/275).
//
// docs/348: THE PRESS CORNER IS A POINT IN THE WORLD. The cartridge's
// rectangle keeps its press corner fixed in world space (FUN_020AF15C/23C add
// the camera stored at the press, recogniser +0xA/+0xC, minus the camera now),
// so when the camera scrolls during a drag its rectangle stretches with the
// ground under it. This box used to keep the press corner fixed on the SCREEN,
// and the two came apart. The camera and band of the frame on screen are
// latched at the press and the corner is moved by what has changed since.
extern "C" int lb_shown_cam(int* x, int* y);   // lb_widemap.cpp
int g_mq_cam0x = 0, g_mq_cam0y = 0, g_mq_band0x = 0, g_mq_band0y = 0;
bool g_mq_cam0_ok = false;

extern "C" void lb_marquee_move(int sx, int sy) {
    if (!g_mq_down) {
        g_mq_down = true;
        g_mq_x0 = sx;
        g_mq_y0 = sy;
        g_mq_cam0_ok = lb_shown_cam(&g_mq_cam0x, &g_mq_cam0y) != 0;
        g_mq_band0x = lb_shown_band_x();
        g_mq_band0y = lb_shown_band_y();
    }
    g_mq_x1 = sx;
    g_mq_y1 = sy;
}

extern "C" void lb_marquee_release(void) { g_mq_down = false; }

extern "C" int lb_marquee_get(int* x0, int* y0, int* x1, int* y1) {
    if (g_mq_ttl <= 0 || !g_mq_down) return 0;
    --g_mq_ttl;
    int ax = g_mq_x0, ay = g_mq_y0;
    int cx = 0, cy = 0;
    if (g_mq_cam0_ok && lb_shown_cam(&cx, &cy)) {
        ax += (g_mq_cam0x - cx) + (lb_shown_band_x() - g_mq_band0x);
        ay += (g_mq_cam0y - cy) + (lb_shown_band_y() - g_mq_band0y);
    }
    if (x0) *x0 = ax < g_mq_x1 ? ax : g_mq_x1;
    if (x1) *x1 = ax < g_mq_x1 ? g_mq_x1 : ax;
    if (y0) *y0 = ay < g_mq_y1 ? ay : g_mq_y1;
    if (y1) *y1 = ay < g_mq_y1 ? g_mq_y1 : ay;
    return 1;
}

constexpr uint32_t kClickDispatch = 0x020AA8D4u;   // thumb

void click_dispatch_observer(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_CLICKDISPATCH") ? 1 : 0;
    if (on) {
        const uint32_t obj = g_cpu.R[0];
        if (obj >= 0x02000000u && obj < 0x02400000u) {
            const uint32_t cb = bus_read_u32(obj + 4u);
            const uint32_t arg = bus_read_u32(obj + 8u);
            // The rect func_020aa8e4 built: min x/y at +0x10/+0x12, max at
            // +0x18/+0x1a, all SIGNED halfwords -- an out-of-band click is
            // supposed to show up here as a negative minimum.
            const int x0 = (int)(int16_t)bus_read_u16(obj + 0x10u);
            const int y0 = (int)(int16_t)bus_read_u16(obj + 0x12u);
            const int x1 = (int)(int16_t)bus_read_u16(obj + 0x18u);
            const int y1 = (int)(int16_t)bus_read_u16(obj + 0x1Au);
            // RATE LIMITED BY DISTINCT TARGET, not by time. The question is
            // which handlers exist and what rect each is given; a per-frame
            // flood of the same pair answers it no better and costs an fflush
            // on a redirected handle, which this project has already paid for
            // once (see lb_widemap.cpp's [lb_fogserve] note).
            static uint32_t seen[16];
            static unsigned seen_n = 0;
            bool fresh = true;
            for (unsigned i = 0; i < seen_n; ++i)
                if (seen[i] == cb) { fresh = false; break; }
            if (fresh && seen_n < 16u) seen[seen_n++] = cb;
            if (fresh || x0 < 0 || x1 < 0) {
                std::fprintf(stderr,
                             "[lb_clickdispatch] obj=%08X cb=%08X arg=%08X  "
                             "rect x %d..%d  y %d..%d%s\n",
                             obj, cb, arg, x0, x1, y0, y1,
                             (x0 < 0 || x1 < 0) ? "  <- OUT OF BAND" : "");
                std::fflush(stderr);
            }
        }
    }
    void (*original)(void) = lb_dispatch_original(0, kClickDispatch);
    if (original) original();
}

// ── Hotwire transport, mode 1: "destroy the occupants" ────────────────────
//
// ── The selection rectangle inverts on a left-band drag ───────────────────
//
// Clicking and drag-selecting units outside the original aspect, or across the
// boundary between it and the extended screenspace, was unreliable: a drag
// might not select all, or only, the units it encompassed.
//
// func_020AA8E4 normalises the drag rect -- it orders each axis with a SIGNED
// compare and writes min to event+0x10/+0x12 and max to +0x18/+0x1A. That is
// correct as the cartridge wrote it. What broke it is the port's own redirect.
//
// The 16-bit shadows the port publishes are read with LDRH, because Thumb has
// no immediate-offset LDRSH (lb_patches.cpp's kTouchPatches notes record this).
// LDRH ZERO-extends. So a true x of -85 arrives in the register as 0x0000FFAB
// = 65451, wins the signed compare, lands in the MAX slot, and is then
// truncated by `strh` back to 0xFFAB -- which the consumer reads with LDRSH as
// -85. The rect comes out INVERTED: min 40, max -85.
//
// An inverted rect makes the two narrow-phase X tests mutually exclusive
// (entity.minx <= -85 AND entity.maxx >= 40), so it selects NOTHING, and the
// broad phase compounds it by padding min-24/max+24, which SHRINKS an inverted
// box instead of growing it.
//
// It also explains the asymmetry a player would see: a drag entirely in the
// RIGHT band works, because x > 255 is positive and zero-extends to itself. Only
// a corner in the LEFT band inverts.
//
// THE FIX IS THREE REGISTERS, NOT A REWRITE. The guest's own arithmetic is
// right; it was handed sign-mangled inputs. Sign-extending r1/r2/r3 before the
// original runs makes its signed compares behave, and its `strh` then stores
// the correct bits for the LDRSH that reads them back. Nothing in the guest's
// logic is replaced, so there is no second implementation to keep in step.
//
// cur_y arrives on the stack and the guest loads it with LDRSH itself
// (0x020AA8F8), so it needs nothing here. That changes the day an extended Y is
// published: the callers store it with a plain `ldrb`, and those sites must be
// widened at the same time or the vertical axis inverts exactly as x did.
constexpr uint32_t kSelRect = 0x020AA8E4u;   // thumb

void sel_rect_override(void) {
    auto sx = [](uint32_t v) {
        return (uint32_t)(int32_t)(int16_t)(uint16_t)v;
    };
    // ONE SHOT. Proof of arrival, and of the thing this exists to fix: if
    // either x reads as a large positive where it should be negative, the
    // zero-extend is still upstream. Per-call logging here is what flooded the
    // log once; this costs one line per run.
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr,
                     "[lb_selrect] first rect: press x %d, current x %d "
                     "(raw %u / %u)\n",
                     (int)(int16_t)(uint16_t)g_cpu.R[1],
                     (int)(int16_t)(uint16_t)g_cpu.R[3],
                     (unsigned)g_cpu.R[1], (unsigned)g_cpu.R[3]);
        std::fflush(stderr);
    }
    g_cpu.R[1] = sx(g_cpu.R[1]);   // press x
    g_cpu.R[2] = sx(g_cpu.R[2]);   // press y
    g_cpu.R[3] = sx(g_cpu.R[3]);   // current x
    // docs/349 LB_SELRECTLOG=1: THE RECTANGLE THE GAME SELECTS AGAINST, every
    // time it changes. The builder's own code (020AA8E4..020AA908) takes press
    // x/y in r1/r2, current x in r3 and current y as a signed halfword at
    // [sp] (`ldr r3,=-16; mov r1,#16; add r3,sp; ldrsh r1,[r3,r1]`), and
    // stores min/max x at rect+0x10/+0x18 and min/max y at +0x12/+0x1A. Read
    // here, after the sign fix above, so the line is exactly the builder's
    // input. Whether what it selects matches the bounding box is a comparison
    // of THIS against the box on screen and the set selected.
    {
        static const bool log = std::getenv("LB_SELRECTLOG") != nullptr;
        static int32_t last[4] = {0x7FFFFFFF, 0, 0, 0};
        if (log) {
            const int32_t v[4] = {
                (int32_t)g_cpu.R[1], (int32_t)g_cpu.R[2], (int32_t)g_cpu.R[3],
                (int32_t)(int16_t)bus_read_u16(g_cpu.R[13])};
            if (std::memcmp(v, last, sizeof v) != 0) {
                std::memcpy(last, v, sizeof v);
                std::fprintf(stderr,
                             "[lb_selrect] press %d,%d current %d,%d\n",
                             (int)v[0], (int)v[1], (int)v[2], (int)v[3]);
            }
        }
    }
    // CPU INDEX, NOT THE THUMB FLAG. This read `1` and cost 74 MB of log and a
    // broken feature: lb_dispatch_original(int cpu, unsigned addr) takes the
    // CPU, and 1 is the ARM7's bank, so an ARM9 address never matched. The
    // override then printed and RETURNED WITHOUT CALLING THE GUEST, so the
    // selection rectangle was never built at all -- worse than the inversion it
    // was added to fix. Every other site here passes 0, including the thumb
    // ones; the table's own `1u` column is the thumb flag and is unrelated.
    void (*original)(void) = lb_dispatch_original(0, kSelRect);
    if (original) { original(); return; }
    // RATE-LIMITED. This sits on the click path and fires per call; unbounded
    // it wrote 1,782,151 lines in one route, which is slow enough to push the
    // replay past its own time budget and read as a freeze.
    static unsigned said = 0u;
    if (said < 4u) {
        ++said;
        std::fprintf(stderr, "[lb_patch] sel rect: no original to call\n");
    }
}

// GOAL WITHIN THE PROJECT: bug-fix toggles around the
// hotwire-through-a-vehicle bug, and this combo's middle position has said
// "not handled" since it was added -- honestly, but incompletely, and no
// setting is meant to be left incomplete.
//
// WHY IT WAITED, AND WHY IT NEED NOT ANY LONGER
//
// docs/136 established that death travels as a `WaitingToDieCommand` and that
// clearing an entity by hand would break lockstep: the kill has to go through
// the command system both peers replay. docs/137 then assembled the recipe and
// left one piece on positional evidence -- whether `func_020EB910` is really
// the submit. It is: its body appends the command to the list for the player
// named by the low nibble of `cmd+0x10` and bumps the counter at `0x021585AC`
// (read from the ROM, and that counter is what the order probe now watches).
//
// THE SEQUENCE, READ FROM A LIVE CALL SITE (func_02046530, thumb):
//
//     func_020EC5A8(&vec, 1)         ArrayEntityRef, capacity one
//     scratch[0] = entity[+0xDC]     handle index
//     scratch[1] = entity[+0xDD]     handle generation
//     func_020EC8DC(&vec, *scratch)  push it
//     func_020A186C()  -> [+0x64]    the command sink
//     func_02005B0C(sink, &vec)      construct the command
//     func_020EB910(cmd)             SUBMIT
//     func_020EC728(&vec)            release the vector
//
// The same shape appears independently in `func_020A8F28`, the move-order
// issuer, which is what makes it a pattern rather than one reading.
// The first three are no longer CALLED -- issue_waiting_to_die builds the
// vector itself now (docs/243) -- but they are kept deliberately rather than
// deleted. They name the shipped recipe this code is modelled on, they are what
// the comments below refer to, and the resumable-state-machine option in
// docs/241 would use them again. Unused, not dead.
constexpr uint32_t kArrayInit  = 0x020EC5A8u;
constexpr uint32_t kArrayPush  = 0x020EC8DCu;
constexpr uint32_t kArrayFree  = 0x020EC728u;
constexpr uint32_t kSessionCtx = 0x020A186Cu;
constexpr uint32_t kMakeDieCmd = 0x02005B0Cu;
constexpr uint32_t kSubmitCmd  = 0x020EB910u;

// One guest call, in the convention this file already uses for SetOwner: push a
// return marker, point LR and PC at the target, dispatch. The callee's return
// matches the marker and unwinds back here. False means the slice ended
// mid-call and the caller must bail.
// NAME THE STEP THAT FAILS. The first run of the self-test reported only
// "UNWOUND" with no command submitted, which says the sequence broke but not
// where -- and the five targets are not alike: kArrayInit, kMakeDieCmd and
// kArrayFree are ARM, while kSubmitCmd (func_020EB910) is THUMB, and guest code
// stores thumb targets with bit 0 set. A failure at step 1 and a failure at
// step 5 have completely different causes, so the log has to distinguish them.
static const char* g_call_step = "?";

static bool call_guest(uint32_t addr) {
    runtime_call_push_return(addr);
    g_cpu.R[14] = addr;
    g_cpu.R[15] = addr;
    runtime_dispatch(addr);
    if (runtime_unwinding()) {
        std::fprintf(stderr, "[lb_call] UNWOUND at %s (%08X)\n",
                     g_call_step, addr);
        std::fflush(stderr);
        return false;
    }
    return true;
}

// Issue a WaitingToDieCommand for one entity. Scratch lives BELOW the live
// frame, which is where the guest's own callers put theirs, and the stack
// pointer is restored on every exit including the unwinding ones.
static bool issue_waiting_to_die(uint32_t entity) {
    const uint32_t saved_sp = g_cpu.R[13];
    const uint32_t vec = (saved_sp - 0x60u) & ~3u;
    const uint32_t scratch = vec - 8u;
    g_cpu.R[13] = scratch - 0x20u;

    // THE VECTOR IS BUILT HERE, NOT BY THE GUEST -- see docs/243.
    //
    // This used to be two guest calls, kArrayInit then kArrayPush. docs/241
    // measured that the failure mode is a chain of guest calls losing its return
    // to a cooperative slice yield, so the length of the chain IS the problem
    // and every call removed is a real improvement. Six are now three.
    //
    // Every field and every WIDTH below is read out of the image rather than
    // assumed, because this project has already lost three answers to matching
    // an offset with the wrong access width:
    //
    //   func_020EC5A8 (init)     +0x00 vtable 0x0214F8AC, +0x04/+0x08/+0x0C zero,
    //                            +0x10 = 0x10, +0x14 BYTE 1, +0x15 BYTE = its arg
    //   func_020EC770 (copy)     reads +0x04 (data) and +0x08 (count); shifts the
    //                            count left by one, so elements are TWO bytes
    //   func_020ECA5C (reserve)  +0x0C is capacity; returns early without
    //                            reallocating when the byte at +0x14 is zero
    //   func_020EC7E8 (teardown) ALSO skips the free when +0x14 is zero
    //
    // That last pair is what makes this safe. +0x14 is not a "may grow" flag, it
    // is an OWNS-THE-BUFFER flag, and the shipped code honours it on both the
    // realloc path and the free path. Setting it to zero hands the game a
    // non-owning view over a buffer this port supplies: it will neither move it
    // nor free it. The mechanism is the cartridge's own, not a trick.
    //
    // Because of that the trailing kArrayFree call is gone too -- there is
    // nothing to free, and calling it would only be a sixth chance to unwind.
    bus_write_u8(scratch + 0u, bus_read_u8(entity + 0xDCu));
    bus_write_u8(scratch + 1u, bus_read_u8(entity + 0xDDu));
    bus_write_u8(scratch + 2u, 0u);
    bus_write_u8(scratch + 3u, 0u);

    bus_write_u32(vec + 0x00u, 0x0214F8ACu);  // Battles::ArrayEntityRef vtable
    bus_write_u32(vec + 0x04u, scratch);      // data -> the two-byte handle
    bus_write_u32(vec + 0x08u, 1u);           // one element
    // Capacity 4 against a needed of 1: reserve returns early when
    // needed <= capacity>>1, so even if something does reserve against this
    // vector it takes the no-growth path rather than the realloc path.
    bus_write_u32(vec + 0x0Cu, 4u);
    bus_write_u32(vec + 0x10u, 0x10u);        // growth granularity, as init sets
    bus_write_u8 (vec + 0x14u, 0u);           // NOT owned: no realloc, no free
    bus_write_u8 (vec + 0x15u, 1u);           // init's own second byte

    if (!(g_call_step = "ctx", call_guest(kSessionCtx))) { g_cpu.R[13] = saved_sp; return false; }
    const uint32_t sink = bus_read_u32(g_cpu.R[0] + 0x64u);

    g_cpu.R[0] = sink;
    g_cpu.R[1] = vec;
    if (!(g_call_step = "construct", call_guest(kMakeDieCmd))) { g_cpu.R[13] = saved_sp; return false; }

    // r0 carries the constructed command straight into the submit, exactly as
    // the live call site does.
    const bool ok = (g_call_step = "submit", call_guest(kSubmitCmd));
    g_cpu.R[13] = saved_sp;
    return ok;
}

// docs/392: the hotwired transport's occupants, one hop each (set_owner_override).
constexpr uint32_t kKillEntity = 0x0205E78Cu;          // arm: (entity, flag)
constexpr uint32_t kHotwireSetOwnerRet = 0x02078DE4u;  // HotWireSpell::v03's lr
extern "C" int lb_demo_play_hotwire_transport(void);   /* src/lb_demo.cpp */

// The setting a replay takes from its recording, else this machine's.
int lb_hotwire_transport_live() {
    const int played = lb_demo_play_hotwire_transport();
    return played >= 0 ? played : lb_setting_hotwire_transport();
}

struct OccupantHops {
    bool active = false;
    bool calling = false;      // mode 2: a hop issued, its SetOwner not in yet
    int mode = 0;              // 1 destroy, 2 hotwire with the transport
    uint32_t r[4] = {0, 0, 0, 0}, r12 = 0, lr = 0, sp = 0, sp_call = 0;
    uint32_t transport = 0, owner_ptr = 0;
    uint32_t occ[16] = {};
    int n = 0, at = 0;
};
OccupantHops g_occ;

uint8_t entity_category(uint32_t entity);   // just below

// Find the transport's occupants (the units whose +0x114/+0x115 hold its
// handle, as Load writes them) and save the call. False when there are none.
bool occupant_start(uint32_t entity, uint32_t owner_ptr, int mode) {
    const uint8_t new_owner = bus_read_u8(owner_ptr);
    if (new_owner == bus_read_u8(entity + 0xE4u) ||
        entity_category(entity) != kCategoryTransport)
        return false;
    const uint8_t h0 = bus_read_u8(entity + 0xDCu);
    const uint8_t h1 = bus_read_u8(entity + 0xDDu);
    const uint32_t pool = bus_read_u32(kEntityPoolPtr);
    if (pool < 0x02000000u || pool >= 0x02400000u) return false;
    const uint32_t array = bus_read_u32(pool + 8u);
    const int32_t count = (int32_t)bus_read_u32(pool + 0xCu);
    g_occ.n = 0;
    for (int32_t i = 0; array && i < count && i < 4096 && g_occ.n < 16; ++i) {
        const uint32_t o = bus_read_u32(array + (uint32_t)i * 4u);
        if (!o || o == entity) continue;
        if (bus_read_u8(o + 0x114u) != h0 || bus_read_u8(o + 0x115u) != h1)
            continue;
        if (bus_read_u8(o + 0x227u) == 0u) continue;          // not alive
        if (mode == 2 && bus_read_u8(o + 0xE4u) == new_owner) continue;
        g_occ.occ[g_occ.n++] = o;
    }
    if (g_occ.n == 0) return false;
    for (int i = 0; i < 4; ++i) g_occ.r[i] = g_cpu.R[i];
    g_occ.r12 = g_cpu.R[12];
    g_occ.lr = g_cpu.R[14];
    g_occ.sp = g_cpu.R[13];
    g_occ.sp_call = (g_cpu.R[13] - 8u) & ~7u;
    g_occ.transport = entity;
    g_occ.owner_ptr = owner_ptr;
    g_occ.mode = mode;
    g_occ.at = 0;
    g_occ.calling = false;
    g_occ.active = true;
    return true;
}

// Issue the next occupant's call; false when every one is done.
bool occupant_hop() {
    if (g_occ.at >= g_occ.n) return false;
    const uint32_t o = g_occ.occ[g_occ.at++];
    g_cpu.R[0] = o;
    g_cpu.R[1] = g_occ.mode == 1 ? 0u : g_occ.owner_ptr;
    g_cpu.R[13] = g_occ.sp_call;
    g_cpu.R[14] = kSetOwner;               // ARM: the return lands back here
    const uint32_t target = g_occ.mode == 1 ? kKillEntity : kSetOwner;
    g_occ.calling = g_occ.mode == 2;
    g_cpu.R[15] = target;
    runtime_dispatch_with_exchange(target);
    return true;
}

uint8_t entity_category(uint32_t entity) {
    const uint32_t def = bus_read_u32(entity + 0x160u);
    if (!def) return 0xFFu;
    const uint32_t info = bus_read_u32(def + 8u);
    if (!info) return 0xFFu;
    return bus_read_u8(info + 0x5Cu);
}

void set_owner_override() {
    // Perform an armed kill self-test here: this override runs
    // inside the guest call stack, which the sequence requires.
    lb_kill_test_run_if_armed();
    // Does a dispatch override on a GAMEPLAY address actually run?
    //
    // docs/143 counted zero dispatch entries for gameplay functions and
    // concluded overrides on them never fire, which is why three settings carry
    // the BLOCKED badge. tier3_coverage shows this address executing 16 times,
    // so the question is whether execution arrives through the dispatcher (our
    // override wins) or through tier-3 interpreting the guest bytes (it does
    // not). Counting here answers it instead of assuming.
    {
        static unsigned fired = 0u;
        if (lb_trace_dispatch() && ++fired <= 3u)
            std::fprintf(stderr, "[lb_disp] set_owner_override FIRED (%u)\n",
                         fired);
    }
    const uint32_t entity = g_cpu.R[0];
    const uint32_t owner_ptr = g_cpu.R[1];
    const int mode = lb_setting_hotwire_transport();

    // docs/392: THE OCCUPANTS OF A HOTWIRED TRANSPORT, ONE GUEST CALL AT A
    // TIME. Mode 1 destroys them, mode 2 hands them to the new owner.
    //
    // The kill is the game's own immediate one, FUN_0205E78C(entity, 0): it
    // gives the unit a DieEntityCommand state and takes it off its team's
    // count. A dying transport calls exactly that on each occupant
    // (0x0206A2F4, 0x0206A3C0), so the occupants of a hotwired one die the
    // way the occupants of a destroyed one do. It runs inside the simulation,
    // so every machine of a match does it on the same frame. docs/136-138 and
    // 243 had built a WaitingToDieCommand instead: that state only waits for
    // a kill, and its issuer skips units inside a transport, so it could
    // never have worked even without the chain losing its return.
    //
    // The chain is gone too. Each occupant is one hop (docs/383): the call is
    // made with LR = SetOwner, so its return dispatches back here, recognised
    // by the stack pointer it was given; after the last one the transport's
    // own SetOwner runs as the tail call it always was. Mode 2 used to make a
    // nested SetOwner call from this frame, the shape that can lose its
    // return to a slice yield (docs/369); it hops the same way now.
    // Only the hotwire's own call (HotWireSpell::virtual_03, 0x02078D5C,
    // returning to 0x02078DE4) starts it; SetOwner's six other callers are
    // left alone.
    if (g_occ.active) {
        if (g_cpu.R[13] != g_occ.sp_call) {   // someone else's SetOwner
            void (*orig)(void) = lb_dispatch_original(0, kSetOwner);
            if (orig) orig();
            return;
        }
        if (g_occ.mode == 2 && g_occ.calling) {
            // The hop's own SetOwner arriving: let it run for the occupant.
            g_occ.calling = false;
            void (*orig)(void) = lb_dispatch_original(0, kSetOwner);
            if (orig) orig();
            return;
        }
        if (occupant_hop()) return;          // the next occupant
        // All done: the transport's own SetOwner, as it was asked for.
        g_cpu.R[0] = g_occ.r[0];
        g_cpu.R[1] = g_occ.r[1];
        g_cpu.R[2] = g_occ.r[2];
        g_cpu.R[3] = g_occ.r[3];
        g_cpu.R[12] = g_occ.r12;
        g_cpu.R[14] = g_occ.lr;
        g_cpu.R[13] = g_occ.sp;
        g_occ.active = false;
        std::fprintf(stderr, "[lb_hotwire] transport %08X: %d occupant(s) %s\n",
                     g_occ.transport, g_occ.n,
                     g_occ.mode == 1 ? "destroyed" : "hotwired with it");
        std::fflush(stderr);
    } else {
        const uint32_t entity = g_cpu.R[0];
        const uint32_t owner_ptr = g_cpu.R[1];
        const int mode = lb_hotwire_transport_live();
        auto ram = [](uint32_t p) { return p >= 0x02000000u && p < 0x02400000u; };
        if ((mode == 1 || mode == 2) && g_cpu.R[14] == kHotwireSetOwnerRet &&
            ram(entity) && ram(owner_ptr) &&
            occupant_start(entity, owner_ptr, mode)) {
            if (occupant_hop()) return;
            g_occ.active = false;   // nothing to do after all
        }
    }

    // "Specials only" USED TO BE FILTERED HERE, and it could never work.
    //
    // The gate was `g_in_hotwire_tick && lb_setting_patch_hotwire_glitch()`,
    // set around HotWireSpell's tick. That flag is always false by the time
    // SetOwner is dispatched: the tick's body at 0x02078D80 is not lifted, so
    // it runs under tier 3, and tier 3 unwinds the host stack the moment it
    // reaches a banked address -- which SetOwner is. The override has already
    // returned, and restored the flag, before the call arrives.
    //
    // It is done in the validator instead now, at 0x0207B7B0, which is where
    // the cartridge itself decides whether a hotwire may proceed. See
    // kHotwireSpecialsOnly. Nothing hotwire-specific belongs in this function.

    // Tail-call the cartridge's own Entity::SetOwner. Nothing may follow.
    void (*original)(void) = lb_dispatch_original(0, kSetOwner);
    if (original) {
        original();
        return;
    }
    // No original to defer to should never happen; do nothing rather than
    // invent ownership semantics.
    std::fprintf(stderr, "[lb_patch] SetOwner: no original to call\n");
}

// ── Attack Boost Spell Affects Ranged Units ────────────────────────────────
//
// The problem: the attack boost is rated below the speed and defence spells
// because it does not reach ranged units.
//
// THE DEFECT, READ FROM THE ROM (docs/95)
//
//   melee   base damage = func_0205F0F0(attacker, target)
//                       = func_0205CC88(attacker + 0x164, target->blueprint)
//                       = max(1, stats[0x0C] + matchup)
//   ranged  base damage = the ProjectileBlueprint's own damage_min/damage_max
//
// `entity+0x164` is Sim::MagicInfluencedUnitStats. `func_0205CC14`, its
// ResetToBase, seeds `stats[0x0C]` from the blueprint's melee_damage at +0x68
// and `stats[0x14]` from attack_interval at +0x6D -- both matching
// docs/data/entities.tsv -- and it runs at the top of the buff recompute
// `func_0205CCC4`, which is driven from the buff list at `entity+0x1E4`.
//
// So `entity+0x170` (= stats[0x0C]) is base melee damage PLUS the buffs, the
// base is always recoverable from the blueprint, and the ranged path never
// reads any of it. That is the whole bug.
//
// WHY THE DELTA AND NOT func_0205F0F0
//
// Substituting the melee routine would give a ranged unit its MELEE damage.
// docs/data/entities.tsv has heroes with melee_damage 0 and a projectile, so
// that would silently zero their shots. Adding (buffed - base) instead applies
// exactly the spell's contribution and leaves the projectile's own blueprint
// damage intact.
//
// WHERE THE BOOST IS APPLIED: AT THE APPLIER'S DOOR, NOT BY REPLACING THE
// PROJECTILE PATH (docs/369)
//
// The ranged path is func_0206E408, 72 bytes, read from the ROM:
//
//   0206E414  add r0, r6, #0x24 ; bl func_02087250   projectile = handle(cmd+0x24)
//   0206E420  mov r0, r5        ; bl func_0208727C   target = resolve(r1)
//   0206E428  cmp r0, #0        ; popeq {r4-r6,pc}   no live target: return
//   0206E430  ldr r1, [r4,#0x160] / ldr r3, [r1,#8]  info = projectile->def->info
//   0206E440  ldrh r2, [r3,#0x6C] / ldrh r3, [r3,#0x6E]
//   0206E448  bl func_0206E970                       ApplyDamage(cmd, r1, min, max)
//
// This used to be REPLACED by an override that re-did the two lookups: the
// target check as a nested guest call, then the applier as a tail call. It
// ran on every ranged hit whatever the setting, and the nested call was not
// re-entrant. runtime_should_yield() ends a slice at ANY instruction once the
// cycle cap is reached. The guest then resumes INSIDE func_0208727C, returns
// to the marker the override pushed (0x0206E408), and so enters the override
// a SECOND time: with the resolver's result in r0, its leftovers in r1 and
// lr, and the real caller's return address gone with the host frame. That
// then tail-called ApplyDamage with an entity as its "command". Its own
// comment ("re-entry redoes a pure lookup") assumed the opposite.
//
// So the cartridge's function now runs untouched, and the boost is applied in
// front of its one call: an observer on ApplyDamage that fires only when lr is
// 0x0206E44C -- the return address of the bl at 0x0206E448, which only the
// ranged path has -- and only with the setting on. It adds the spell's
// contribution to r2/r3 and tail-calls the cartridge's applier. No guest call,
// nothing after the tail call, and with the setting off it changes nothing.
//
// func_02087250 is the pool lookup (func_0205C9B4) with a kind test: kind 1 is
// a projectile, kind 0 is everything else (docs/105). The observer repeats it
// read-only in C (resolve_handle_kind) to find the shooter; the cartridge
// still does its own lookups, write-backs included.

constexpr uint32_t kApplyDamage      = 0x0206E970u;   // three callers
constexpr uint32_t kRangedApplyRet   = 0x0206E44Cu;   // after func_0206E408's bl
constexpr uint32_t kEntityPool       = 0x02155028u;

// func_0205C9B4: index and generation into the entity pool, then a kind test.
uint32_t resolve_handle_kind(uint32_t handle_addr, uint8_t want_kind) {
    const uint32_t pool = bus_read_u32(kEntityPool);
    if (!pool) return 0u;
    const uint8_t index = bus_read_u8(handle_addr);
    const uint8_t generation = bus_read_u8(handle_addr + 1u);
    const int32_t count = (int32_t)bus_read_u32(pool + 0xCu);
    if ((int32_t)index >= count) return 0u;
    const uint32_t array = bus_read_u32(pool + 8u);
    if (!array) return 0u;
    const uint32_t entity = bus_read_u32(array + (uint32_t)index * 4u);
    if (!entity) return 0u;
    if (bus_read_u8(entity + 0xDDu) != generation) return 0u;
    const uint32_t a = bus_read_u32(entity + 0xE0u);
    if (!a) return 0u;
    const uint32_t b = bus_read_u32(a + 4u);
    if (!b) return 0u;
    if (bus_read_u8(b + 8u) != want_kind) return 0u;
    return entity;
}

void apply_damage_observer() {
    // A SECOND guest-call context for the kill self-test (it does nothing
    // unless a developer armed it). set_owner_override fires on entity SPAWN,
    // and a passive Free Play battle spawns roughly one unit every few minutes
    // -- one attempt per run, which starves a retry loop that needs several.
    // Damage is applied whenever anything hits, so between the two the test
    // gets real opportunities.
    lb_kill_test_run_if_armed();
    { static unsigned f = 0u; if (lb_trace_dispatch() && ++f <= 2u) std::fprintf(stderr, "[lb_disp] apply_damage_observer FIRED (%u)\n", f); }

    if (g_cpu.R[14] == kRangedApplyRet) {
        // DID THE RANGED PATH RUN AT ALL? Without this, "the attack boost
        // never fired" cannot be told apart from "no ranged attack happened",
        // and the second is not evidence about the control. Counted before
        // the boost's own condition, so the two are separate numbers.
        static unsigned shots = 0;
        const unsigned n = ++shots;
        if ((n & (n - 1u)) == 0u)   // 1, 2, 4, 8, ...
            std::fprintf(stderr,
                         "[lb_projectile] ranged damage applied %u times\n", n);
    }

    // r0 is the damage command, whose +0x24 is the projectile's handle -- the
    // same field func_0206E408 resolved on its way here.
    const uint32_t projectile = (g_cpu.R[14] == kRangedApplyRet &&
                                 lb_setting_attack_boost_ranged())
                                    ? resolve_handle_kind(g_cpu.R[0] + 0x24u, 1u)
                                    : 0u;
    if (projectile) {
        const uint32_t shooter = resolve_handle_kind(projectile + 0x164u, 0u);
        if (shooter) {
            const uint32_t sdef = bus_read_u32(shooter + 0x160u);
            const uint32_t sinfo = sdef ? bus_read_u32(sdef + 8u) : 0u;
            // WIDTH: kStatFields below records melee_damage at +0x68 as
            // ONE byte, from tools/lb_blueprints.py. This read was u16,
            // which folds melee_spread at +0x6A's low byte into the
            // value and inflates the base, making the computed delta too
            // small or negative. Matching the recorded width.
            const int32_t base = sinfo ? (int32_t)bus_read_u8(sinfo + 0x68u) : 0;
            const int32_t buffed = (int32_t)bus_read_u16(shooter + 0x170u);
            const int32_t delta = buffed - base;
            if (sinfo && delta > 0) {
                g_cpu.R[2] += (uint32_t)delta;   // damage_min
                g_cpu.R[3] += (uint32_t)delta;   // damage_max
                // COUNT IT, so "this control is wired" can be a measurement.
                // The badge stays READ until a run shows this firing: the
                // consumer being present is not evidence that it ever runs,
                // and it only can when a BOOSTED RANGED unit actually shoots.
                static unsigned fired = 0;
                const unsigned n = ++fired;
                if ((n & (n - 1u)) == 0u)   // 1, 2, 4, 8, ...
                    std::fprintf(stderr,
                                 "[lb_attackboost] applied +%d to a ranged "
                                 "attack (%u so far)\n", (int)delta, n);
            }
        }
    }

    // Tail-call the cartridge's applier. Nothing may follow.
    void (*original)(void) = lb_dispatch_original(0, kApplyDamage);
    if (original) {
        original();
        return;
    }
    std::fprintf(stderr, "[lb_patch] ApplyDamage: no original to call\n");
}


// -- Fix Hotwired Healing ---------------------------------------------------
//
// docs/95 read Sim::HealSpell end to end and found the bug is a plain ABSENCE:
// the tick's three filters are category, category-filter and hp-not-full, and
// none of them is ownership. A hotwired unit changes owner but does not move,
// so no exit callback fires, it stays in the candidate array at +0x3C, and it
// keeps being healed by its former owner's aura.
//
// The tick already has everything needed to decide. At 0x02078900 it resolves
// the source from the two-byte handle at `this+0x18` and reads the source's
// owner byte at +0xE4 -- but only to gate a bookkeeping call. The heal itself,
// func_0205E7F4, runs on the next line unconditionally.
//
// So the fix is to apply that same comparison to the heal. Rather than
// reimplement a 600-byte tick -- which would mean guessing at its behaviour
// -- this brackets the original tick and filters the HP write inside it:
//
//   1. HealSpell::tick is overridden to resolve the source's owner, set a flag,
//      call the ORIGINAL tick, and clear the flag;
//   2. the HP write is overridden to skip when the flag is set, the setting is
//      on, and the target's owner differs from the source's.
//
// The HP write has 19 callers across the image. The flag is what keeps the
// other 18 untouched: outside the heal tick this override does nothing at all.
// docs/125 is the reason that check was made before writing this.
constexpr uint32_t kHealTick = 0x02078770u;   // Sim::HealSpell slot 3
constexpr uint32_t kSetHp    = 0x0205E7F4u;   // the HP write in that tick
constexpr uint32_t kResolveLiveEntity = 0x020872DCu;  // docs/105's real name

// kResolveLiveEntity DONE IN C, because it is a pure function of memory and
// entering the guest for it cost this port a crash (see heal_tick_override).
//
// The guest chain is three functions and none of them needs a stack:
//
//   func_0205C9B4  the lookup. Its literal is 0x02155024 and it reads +4, so
//                  the table pointer is *(u32*)0x02155028 -- the same word
//                  func_02087350 loads and passes in r0, which this function
//                  then ignores and re-loads for itself.
//   func_02087350  the above, plus a null-handle write-back on failure.
//   func_020872DC  the above, plus "reject an entity that is dying".
//
// The write-back is deliberately NOT reproduced. It clears the caller's stale
// handle, and the guest does that for itself at 0x0207AA40 in
// SpellBase::virtual_03 two instructions after this override runs. Doing it
// here as well would be the port writing simulation state that the cartridge
// already maintains -- exactly what docs/112's rule warns against.
//
// The slot bound is SIGNED in the original (`cmp r2,r0 / movge r0,#0`), and
// kept signed here: slot comes from a byte so it cannot go negative, but the
// count is a full word and a garbage table would compare differently unsigned.
uint32_t resolve_live_entity_c(uint32_t handle_ptr) {
    const uint32_t table = bus_read_u32(0x02155028u);
    if (!table) return 0u;
    const uint32_t slot = bus_read_u8(handle_ptr);
    if ((int32_t)slot >= (int32_t)bus_read_u32(table + 0x0Cu)) return 0u;
    const uint32_t arr = bus_read_u32(table + 8u);
    if (!arr) return 0u;
    const uint32_t obj = bus_read_u32(arr + slot * 4u);
    if (!obj) return 0u;
    // The handle's second byte is a generation stamp; a slot reused since the
    // handle was taken fails here rather than returning the wrong entity.
    if (bus_read_u8(obj + 0xDDu) != bus_read_u8(handle_ptr + 1u)) return 0u;
    // func_020872DC's liveness test: obj->[0xE0]->[4]->u8[8] must be zero.
    const uint32_t a = bus_read_u32(obj + 0xE0u);
    if (!a) return 0u;
    const uint32_t b = bus_read_u32(a + 4u);
    if (!b) return 0u;
    if (bus_read_u8(b + 8u) != 0u) return 0u;
    return obj;
}

bool    g_in_heal_tick = false;
unsigned g_heal_ticks = 0u;      /* LB_TRACE_HEAL=1 diagnostics */
unsigned g_heal_writes = 0u;
unsigned g_heal_blocked = 0u;
bool lb_trace_heal(void) {
    static const int on = std::getenv("LB_TRACE_HEAL") ? 1 : 0;
    return on != 0;
}
uint8_t g_heal_source_owner = 0xFFu;

void heal_tick_override(void) {
    { static unsigned f = 0u; if (lb_trace_dispatch() && ++f <= 2u) std::fprintf(stderr, "[lb_disp] heal_tick_override FIRED (%u)\n", f); }
    const uint32_t spell = g_cpu.R[0];
    const bool was_in = g_in_heal_tick;
    const uint8_t was_owner = g_heal_source_owner;

    g_in_heal_tick = false;
    g_heal_source_owner = 0xFFu;
    if (lb_setting_fix_hotwired_healing() && spell) {
        // Resolve the source through the SAME function the tick itself uses on
        // this field, so the handle interpretation cannot drift from the game's.
        // SAVE EVERY REGISTER THE SYNTHESISED CALL CAN CLOBBER, not just r0.
        //
        // This override runs BEFORE original(), and original() is the real heal
        // tick, entered by the guest with its arguments in r0..r3 and its return
        // in lr. Calling the resolver in between destroys all of that: an ARM
        // callee may freely clobber r0-r3, r12 and lr, and the flags with them.
        // Only r0 was being put back, so original() ran with five corrupted
        // registers and eventually branched through one of them.
        //
        // That is what the crash actually is. The miss is at 0x02193E14, which
        // is past the end of static code (0x02150880) AND past every declared
        // overlay entry point (0216A2E0..0216BBC4) -- it is HEAP, not code the
        // lift missed. runtime_dispatch_miss's own advice is to declare the
        // address as an entry point; here that would have been exactly wrong,
        // because there is no function there to declare. A garbage branch
        // target and an unlifted function look identical in that log and are
        // opposite problems.
        //
        // Reproduced and verified against a recorded combat session
        // (demos/user-combat-2026-09-14.lbdemo) with the setting on.
        // NOTHING IS SAVED OR RESTORED HERE ANY MORE. Six registers and the
        // CPSR used to be preserved across the synthesised call so the original
        // heal tick would see its own arguments instead of the resolver's
        // leftovers. With the call gone the guest's register file is never
        // touched, so there is nothing to put back -- and a save/restore pair
        // that protects against nothing is worse than none, because the next
        // reader has to work out what it was guarding.
        //
        // RETURN TO THIS OVERRIDE, NOT TO THE FUNCTION BEING CALLED.
        //
        // This pushed kResolveLiveEntity -- the call TARGET -- as its own
        // return address, and set R14 to it as well. projectile_damage_override
        // two hundred lines up does the same call correctly:
        //
        //     runtime_call_push_return(kProjectileDamage);   // itself
        //     g_cpu.R[14] = kProjectileDamage;               // itself
        //     g_cpu.R[15] = kResolveTarget;                  // the target
        //     runtime_dispatch(kResolveTarget);
        //     if (runtime_unwinding()) return;
        //
        // [RETRACTED, docs/369: that call was not correct either. A slice can
        //  end at any instruction inside it; the guest then resumes inside the
        //  callee and re-enters the override through the pushed marker with
        //  the callee's registers, the real return address lost. The override
        //  was removed; apply_damage_observer replaces it with no guest call.]
        //
        // so the convention is established and this site departed from it in
        // two ways: the return address, and the missing unwinding check. The
        // crash this setting causes is a dispatch miss at 0x020872DC reached
        // with lr=0x02087368 -- an lr this override never sets, so the miss is
        // on a path the guest took AFTER being entered wrongly here.
        //
        // Reproduced deterministically by replaying a recorded combat
        // session (demos/user-combat-2026-09-14.lbdemo) with the setting on,
        // which is what makes this testable rather than arguable.
        // NO SYNTHESISED GUEST CALL. This used to dispatch into
        // kResolveLiveEntity and the setting crashed the game; the crash was a
        // SMASHED RETURN ADDRESS, not a bad branch target:
        //
        //   [dispatch-miss] pc=0x02193E14 arm (lr=0x0207AA90)
        //
        // lr 0x0207AA90 is the instruction after `bl func_0207AAB8` at
        // 0x0207AA8C, and func_0207AAB8 is three instructions with no indirect
        // branch -- so the bl returned fine and the containing function then hit
        // `pop {r4, pc}` at 0x0207AA94 and loaded 0x02193E14, a heap pointer,
        // off the guest stack. The containing function is
        // Battles::Sim::SpellBase::virtual_03 (0x0207AA00) -- the spell tick
        // itself.
        //
        // Two hypotheses were tested and one is REFUTED. Removing the three
        // interior entry points of docs/280 and re-lifting clean changed
        // nothing: same pc, same lr, same RAM, same generation 40431,
        // bit-for-bit. So the truncated containers were not the cause, and that
        // line is dropped rather than restated.
        //
        // What is left is this call, because it is the only thing on this path
        // that touches the guest stack at all -- and it never needed to.
        // Everything it produced was ONE BYTE: the owner at source+0xE4. The
        // resolution behind it is a pure function of memory, with no call, no
        // stack and no side effect the override depends on:
        //
        //   func_0205C9B4   t3   = *(u32*)0x02155028      (its own literal
        //                                                  0x02155024, +4)
        //                   slot = u8 handle[0]
        //                   if (slot >= (s32)t3[0x0C]) return 0
        //                   obj  = ((u32*)t3[8])[slot]
        //                   if (!obj) return 0
        //                   if (u8 obj[0xDD] != u8 handle[1]) return 0
        //                   return obj
        //   func_02087350   calls the above; on failure writes a null handle
        //                   back through the caller's pointer -- a side effect
        //                   the guest performs for itself two instructions
        //                   later at 0x0207AA40, so the override must NOT.
        //   func_020872DC   calls the above, then rejects a dying entity:
        //                   a = u32 obj[0xE0]; b = u32 a[4]; if (u8 b[8]) 0
        //
        // Replicated exactly below. The guest is now never entered, so there is
        // no return address to smash, no call-return-stack entry to leave
        // stale, and no re-entrancy window to check for.
        const uint32_t source = resolve_live_entity_c(spell + 0x18u);
        if (source) {
            g_heal_source_owner = bus_read_u8(source + 0xE4u);
            g_in_heal_tick = true;
        }
    }

    if (lb_trace_heal() && g_in_heal_tick && (g_heal_ticks % 64u) == 0u)
        std::fprintf(stderr,
                     "[lb_heal] tick #%u source owner %u\n",
                     g_heal_ticks, (unsigned)g_heal_source_owner);
    ++g_heal_ticks;

    void (*original)(void) = lb_dispatch_original(0, kHealTick);
    if (original) original();

    g_in_heal_tick = was_in;
    g_heal_source_owner = was_owner;
}

void set_hp_override(void) {
    // Unconditional probe: proves whether this override is REACHED at all,
    // which distinguishes "the dispatch never fires" from "no heal spell exists
    // in this match". Only the first few calls are logged.
    if (lb_trace_heal() && g_heal_writes < 3u)
        std::fprintf(stderr,
                     "[lb_heal] SetHp reached: entity %08X hp<-%u in_tick=%d\n",
                     (unsigned)g_cpu.R[0], (unsigned)g_cpu.R[1],
                     g_in_heal_tick ? 1 : 0);
    ++g_heal_writes;
    if (g_in_heal_tick) { /* counted above */ }
    if (g_in_heal_tick && g_heal_source_owner != 0xFFu) {
        const uint32_t target = g_cpu.R[0];
        if (target && bus_read_u8(target + 0xE4u) != g_heal_source_owner) {
            // Not this player's unit any more, so the aura must not heal it.
            //
            // Neutralise the heal by asking for the CURRENT hp (docs/95: hp is
            // at entity+0x1A0) rather than by fabricating a return. The runtime
            // has no documented "return from an override" helper, and inventing
            // one would be guessing at a calling convention; writing hp = hp
            // keeps every path, side effect and return value exactly as the
            // cartridge would have them, and changes only the amount.
            g_cpu.R[1] = bus_read_u16(target + 0x1A0u);
            ++g_heal_blocked;
            if (lb_trace_heal())
                std::fprintf(stderr,
                             "[lb_heal] BLOCKED heal: target owner %u != "
                             "source owner %u (blocked %u of %u)\n",
                             (unsigned)bus_read_u8(target + 0xE4u),
                             (unsigned)g_heal_source_owner,
                             g_heal_blocked, g_heal_writes);
        }
    }
    void (*original)(void) = lb_dispatch_original(0, kSetHp);
    if (original) {
        original();
        return;
    }
    std::fprintf(stderr, "[lb_patch] SetHp: no original to call\n");
}



// ── Freeplay starts on King's music whatever army you field ───────────────
//
// With a non-vanilla music preference, beginning a new freeplay/multiplayer
// match should play the chosen pair rather than the stock King pair: the fix
// repoints which two tracks freeplay uses instead of the static King pair
// vanilla always uses. (Pair = the faction's CALM and BATTLE tracks.)
//
// The cartridge is not hardcoded to King, which is the surprise. func_02088590
// -- the match-load sound preload -- ends by choosing a side and starting the
// music:
//
//     020887D2  blx func_0200239C      ; is this faction a CustomFactionBlueprint?
//     020887DA  adds r6,#0x31          ; if so, read its BASE faction id
//     020887DE  subs r4,#0x7C          ; faction 124..129 -> side 0..5
//     020887E2  cmp  r4,#5 / bhi       ; anything else -> side 6 (none)
//     020887F0  add  pc,r0             ; jump table -> movs r1,#0..5
//     02088814  bl   func_02088980     ; SetMusicSide(obj, side)
//     0208881E  bl   func_0208898C     ; SetMusicKind(obj, 1)
//     02088824  bl   func_02088974     ; StartMusic(obj)
//
// So it DOES derive the side from the faction. But in freeplay the human's
// faction is the custom-army slot (OpponentSetup hardcodes 0x84), so this takes
// the custom branch and reads the army record's BASE faction byte -- which is
// King by default no matter which units are actually fielded (docs/23: the
// record's +0x13 defaults to 0x7C). An Earth army therefore plays King's pair
// because the record still says King.
//
// WHY THE PORT'S EXISTING WRITE WAS TOO LATE. lb_music_tick runs at vblank, so
// it corrects the side only after StartMusic has already begun King's track,
// and the game's re-pick will not act on the change until the current stream
// passes its halfway point (docs/261). That is why the choice did not take
// effect until part-way through the default King music.
//
// Setting it HERE needs no crossfade and cuts nothing: the side is chosen
// before the music starts, so the right calm/battle pair plays from the first
// bar. Only the SIDE is substituted -- kind stays the game's, so calm-vs-battle
// continues to follow combat exactly as it always did.
//
// Vanilla is untouched: lb_music_wanted_side() returns -1 when music_mode is 0.
constexpr uint32_t kSetMusicSide = 0x02088980u;   // thumb

void set_music_side_override(void) {
    const int want = lb_music_wanted_side();
    // ONE SHOT, NOT PER CALL. Proof that this fired and what it did, at the one
    // moment it matters -- match load. Absence of an error is not proof of
    // arrival: with the failure path rate-limited, "no complaints" reads the
    // same whether the override ran or was never reached. This says which.
    // A per-call line here would sit on the match-load path and is exactly the
    // shape of message that cost 74 MB and 4-10 fps once already.
    // THE FIRST CALL IS NOT THE MATCH LOAD. The front end sets side 6 ("none")
    // before a battle exists, so a one-shot here reports the menu and says
    // nothing about the load -- which is exactly what it did: "guest chose 6,
    // port declined". Log the first few instead, so the sequence identifies
    // which call is the match's own. Still bounded, still off the hot path.
    static unsigned calls = 0u;
    if (calls < 6u) {
        ++calls;
        std::fprintf(stderr,
                     "[lb_music] SetMusicSide #%u: guest %u, port wants %d -> "
                     "%s\n",
                     calls, (unsigned)g_cpu.R[1], want,
                     (want >= 0 && want <= 5) ? "substituted" : "declined");
        std::fflush(stderr);
    }
    if (want >= 0 && want <= 5) g_cpu.R[1] = (uint32_t)want;
    // CPU index, not the thumb flag -- see the note in sel_rect_override.
    void (*original)(void) = lb_dispatch_original(0, kSetMusicSide);
    if (original) { original(); return; }
    static unsigned said = 0u;
    if (said < 4u) {
        ++said;
        std::fprintf(stderr, "[lb_patch] set music side: no original to call\n");
    }
}

// -- The placement footprint in the bands -----------------------------------
//
// docs/306 design (3), measured against docs/318. func_020370B4 paints the
// placement footprint into a 32x26 buffer that covers the native 256 px and
// nothing else, so a ghost dragged into a widened band is evaluated by the
// game -- `verdict` still reads 1 out there -- and drawn nowhere.
//
// The fix asks the GUEST to paint the band, by moving its own camera by whole
// metatiles and calling its own painter, then harvesting what it wrote. Every
// per-cell verdict stays the cartridge's; func_0205C034 is never called from
// here and its predicate is never replicated.
//
// ONE CALLER. func_020370B4 is called from func_02037EC0 and nowhere else, so
// this override is the whole of the painting path.
//
// docs/306 measured the three side effects that made repeating the call look
// unsafe, and all three are benign: the return value is discarded, `view+0x68`
// is an idempotent dirty flag storing the constant 1, and the BitArray2D pair
// is a balanced null-guarded constructor/destructor over a stack local.
constexpr uint32_t kFpPaint = 0x020370B4u;

extern "C" int lb_fp_placement_active(void);      /* src/lb_widemap.cpp */
extern "C" int lb_fp_band_shift2(int* sx, int* sy);   /* docs/383 */
extern "C" void lb_fp_harvest2(int sx, int sy);
extern "C" void lb_fp_band_clear(void);
extern "C" int lb_fp_cam_x(void);
extern "C" int lb_fp_cam_y(void);
extern "C" void lb_fp_cam_set_x(int px);
extern "C" void lb_fp_cam_set_y(int px);
extern "C" int lb_fp_log(void);
extern "C" int lb_fp_native_ready(void);
extern "C" int lb_fp_native_rebuild2(int sx, int sy);
extern "C" void lb_fp_set_site_size(int w, int h);
extern "C" void lb_fp_line_note(uint32_t view, int mx, int my, int tile);
extern "C" void lb_fp_line_commit(int mode);           /* docs/392 */

// The shift CURRENTLY applied to the guest's camera, and the camera it was
// applied to. Statics rather than locals because a recompiled body is
// resumable: original() can return mid-function on a slice yield and this
// override is then re-entered from the top with the guest's PC still inside
// the painter. Tracking the applied delta makes re-applying it a no-op, so a
// re-entry resumes instead of double-shifting.
int g_fp_applied = 0;     // x
int g_fp_applied_y = 0;   // docs/383: and y
int g_fp_saved_cam = 0;
unsigned g_fp_unwinds = 0;

// HOW THE BAND IS PAINTED: BY THE GUEST, ON ITS OWN SCHEDULE.
//
// Two earlier shapes are recorded in docs/319 and both are dead. Calling
// `original()` more than once cannot work, because a recompiled body does not
// return -- its epilogue dispatches onward into the caller and comes back only
// at slice end. Calling the painter through `runtime_call_push_return` +
// `runtime_dispatch` cannot work either, and that one is a property of the
// function rather than of the machinery:
//
//   0210a75c  ldr  r1, [pc, #0x28]   = 0x04000280    ; DIVCNT
//   0210a760  ldrh r0, [r1]
//   0210a764  tst  r0, #0x8000                       ; the divider's BUSY bit
//   0210a768  bne  #0x210a760                        ; SPIN
//
// The painter busy-waits on the DS hardware divider, and that bit is cleared
// by the SCHEDULER advancing emulated time. Inside a nested synchronous call
// the scheduler does not run, so the call either yields for ever (400 of 400)
// or spins for ever (the seventh call froze the game).
//
// So nothing here calls guest code out of turn. The guest calls its own
// painter, once per frame as it always did, and these two overrides only
// arrange what it sees and tidy up after it:
//
//   ghost inside the window   painter: nothing          post: nothing
//   ghost out, even frame     painter: nothing          post: save the NATIVE
//                                                             buffer
//   ghost out, odd frame      painter: camera += band   post: harvest that
//                                                             band, put the
//                                                             native copy
//                                                             back, camera
//
// A fixed three-phase cycle was written first and harvested correctly, but it
// paid for a band nobody could see and left the native buffer up to two frames
// old on two frames in three, so a ghost dragged INSIDE the window would
// judder. Gating on lb_fp_band_shift2() makes the common case cost nothing at
// all and the native copy exactly one frame old when it costs anything.
//
// `func_02036FBC` is the post hook because it is the right place, not a
// convenient one: it has exactly ONE caller, `func_02037EC0`, it is the very
// next call after the painter (0x02037F60 against 0x02037F44), every path
// through that function reaches it, and it USES the camera itself -- it calls
// func_0203CA10(&out, view, &in) -- so the restore has to happen before it
// runs rather than after.
constexpr uint32_t kFpPost = 0x02036FBCu;

// Whether the post hook owes a harvest this frame (-1 nothing, 1 owed), and
// the camera shift it was painted at (docs/383: both axes).
int g_fp_pending = -1;
int g_fp_pending_shift = 0;
int g_fp_pending_shift_y = 0;
unsigned g_fp_saves = 0, g_fp_restores = 0, g_fp_abandoned = 0;

// UNDO THE DELTA, DO NOT RESTORE AN ABSOLUTE.
//
// This used to write `g_fp_saved_cam + want`, an absolute value read when the
// painter was entered. Between the painter and the post hook the guest is free
// to move its own camera -- func_020398B8 writes view+0x5E every tick -- and
// restoring the absolute then CLOBBERS that movement, snapping the camera back
// to where it was a moment ago. Repeated once per painter call that is exactly
// a camera that breaks itself nudging around.
//
// Applying the difference leaves any guest movement intact: whatever the
// camera is now, it ends up shifted by `want` instead of by `g_fp_applied`.
void fp_set_shift(int want, int want_y) {
    if (want != g_fp_applied) {
        const int cur = lb_fp_cam_x();
        if (cur != INT_MIN) {
            lb_fp_cam_set_x(cur + (want - g_fp_applied));
            g_fp_applied = want;
        }
    }
    if (want_y != g_fp_applied_y) {   /* docs/383 */
        const int cur = lb_fp_cam_y();
        if (cur != INT_MIN) {
            lb_fp_cam_set_y(cur + (want_y - g_fp_applied_y));
            g_fp_applied_y = want_y;
        }
    }
}

// Called at render time, after the guest's frame.
//
// The shift is meant to live for exactly one function -- the painter -- and to
// be taken back by the post hook before anything else reads the camera. A
// shift still applied at render time means the guest yielded inside the
// painter and has not reached the post hook yet, and the camera must not
// survive the frame shifted or the whole picture jumps.
//
// Putting it back also means the painter, when it resumes, finishes against
// the UNSHIFTED camera -- so what it leaves in the buffer is the native
// window, not the band. The pending harvest is therefore abandoned rather than
// stored: a band table holding a copy of the native view would be worse than a
// band table that is one cycle out of date.
extern "C" void lb_fp_cam_rescue(void) {
    if (!g_fp_applied && !g_fp_applied_y) return;
    fp_set_shift(0, 0);
    if (g_fp_pending > 0) { g_fp_pending = -1; ++g_fp_abandoned; }
    ++g_fp_unwinds;
}

void fp_paint_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kFpPaint);
    if (!original) return;
    // SAY WHY NOTHING WAS HARVESTED. A run reached placement, dragged the
    // ghost across both bands and produced no harvest line at all, which is
    // equally consistent with "the override never fired", "the flag read 0"
    // and "the band width was 0". Print the three of them once each.
    if (lb_fp_log()) {
        static long last = -2;
        const int a = lb_fp_placement_active();
        // WHAT THE GATE ASKED FOR (docs/383: a shift in both axes, 0,0 when
        // the site is inside the guest's own window), AND WHAT THE POST HOOK
        // ACTUALLY DID: `abandoned` separates "the painter yielded before
        // func_02036FBC and the render-time rescue dropped it" from "never
        // taken at all".
        int nsx = 0, nsy = 0;
        const int nd = lb_fp_band_shift2(&nsx, &nsy);
        const long key = ((((long)a * 4 + nd) * 4096 + (nsx & 4095)) * 4096 +
                          (nsy & 4095)) * 64 +
                         (long)(g_fp_saves + g_fp_restores * 7u +
                                g_fp_abandoned * 53u);
        if (key != last) {
            last = key;
            std::fprintf(stderr,
                         "[lb_fp] painter override: active=%d needed=%d "
                         "shift=%+d,%+d native=%d cam=%d,%d  "
                         "saves=%u restores=%u abandoned=%u\n",
                         a, nd, nsx, nsy, lb_fp_native_ready(), lb_fp_cam_x(),
                         lb_fp_cam_y(), g_fp_saves, g_fp_restores,
                         g_fp_abandoned);
            std::fflush(stderr);
        }
    }
    if (!lb_fp_placement_active()) {
        // Nothing being placed: the cartridge's own function, unchanged, and
        // a true tail call with nothing after it.
        if (g_fp_applied || g_fp_applied_y) fp_set_shift(0, 0);
        g_fp_pending = -1;
        lb_fp_band_clear();
        original();
        return;
    }
    g_fp_saved_cam = lb_fp_cam_x();
    if (g_fp_saved_cam == INT_MIN) { g_fp_pending = -1; original(); return; }

    // ONLY WHEN A BAND IS ACTUALLY NEEDED, AND THEN EVERY OTHER FRAME.
    //
    // A fixed three-phase cycle was tried first and it works -- the harvest
    // read 24 cells out of the left band -- but two frames in three then show
    // a native buffer that is up to two frames old, so a ghost being dragged
    // inside the window would judder. It also pays for a band nobody can see.
    //
    // So: a ghost inside the guest's window costs nothing at all, and a ghost
    // at or past an edge alternates native/band. The native copy the post hook
    // puts back is then exactly ONE frame old -- 17 ms at the 57 fps these
    // runs measure -- and the band refreshes at 30 Hz, which is faster than
    // anyone drags a building.
    // docs/383: the shift is chosen from the SITE, in both axes, so the
    // painter's own window holds it wherever it is in the widened view --
    // above or below the DS's rows, or further out than one band-width.
    int sx = 0, sy = 0;
    if (!lb_fp_band_shift2(&sx, &sy)) {
        // The ghost is inside the guest's own window. Its own paint has every
        // cell, so this frame is left completely alone.
        g_fp_pending = -1;
        g_fp_pending_shift = 0;
        g_fp_pending_shift_y = 0;
        original();
        return;
    }
    g_fp_pending = 1;
    g_fp_pending_shift = sx;
    g_fp_pending_shift_y = sy;
    fp_set_shift(sx, sy);
    original();
}

// The tidy-up, and it runs BEFORE the cartridge's own function for the reason
// in the comment above: this one reads the camera.
// ── The placement checkmark keeps to the window the player sees (docs/372) ──
//
// The red/green build placement indicator was inconsistent, and its checkmark
// moved unexpectedly. func_02036FBC -- the function kFpPost sits on
// -- places the checkmark (widget 631, *0x021575B4) every frame of a
// placement, read from the ROM:
//
//   p = tile_origin * (24, 16) - camera        (FUN_0201479C, FUN_0203CA10)
//   p.x += 4
//   0203704C  cmp p.y, #8 : y >= 8 ? y -= 8 : y += h*16    above, else below
//   0203706C  cmp p.x, #0 : x <  0 ? x += w*24 - 8      : x   left end, else right
//   -> the widget's SetPosition (020E5B7C -> 020EA4D8)
//
// Those two tests keep the checkmark inside the DS's own screen: x = 0 is its
// left edge and y = 8 its top margin. In the widened view both are lines
// through the middle of the picture, so dragging a site across them makes the
// checkmark jump by w*24-8 or h*16+8 px, in plain view.
//
// So the same rule is measured from the visible window instead: its left edge
// L and top edge T in native coordinates, x < L and y < T + 8 (L = T = 0 gives
// the ROM's). The byte patch is impossible here: an override's original() is
// the lifted body and never re-reads its bytes. So this observer computes the
// cartridge's p with the cartridge's arithmetic and the widened thresholds,
// and the SetPosition observer below hands it to the call that would have
// placed the checkmark. Everything else -- whether it is shown, the widget
// call, the record writes -- stays the cartridge's.
constexpr uint32_t kWidgetSetPos = 0x020EA4D8u;     // thumb: Widget::SetPosition
constexpr uint32_t kCheckmarkPtr = 0x021575B4u;     // -> the checkmark widget
constexpr uint32_t kCheckSetPosRet = 0x020E5BEAu;   // after 020E5BE6's bl (thumb)
struct CheckStash {
    bool pending;
    uint32_t widget;
    int16_t x, y;
};
CheckStash g_check = {false, 0u, 0, 0};

static bool check_in_ram(uint32_t a) {
    return (a >= 0x02000000u && a < 0x02400000u) ||
           (a >= 0x027E0000u && a < 0x027E4000u);   // the stack is in DTCM
}

// `state` is func_02036FBC's r0: w at +4, h at +5, the site's tile at +0x20/+0x21.
static void checkmark_stash(uint32_t state) {
    g_check.pending = false;
    if (!lb_setting_enhanced()) return;
    if (const char* v = std::getenv("LB_CHECKMARK"))
        if (v[0] == '0') return;
    if (!check_in_ram(state)) return;
    const uint32_t w = bus_read_u32(kCheckmarkPtr);
    const uint32_t view = bus_read_u32(0x02152034u);
    if (!check_in_ram(w) || !check_in_ram(view)) return;
    // The visible window's top-left, in native coordinates: the frontend shows
    // the centred vis x visr of the surface, whose native 256x192 starts at
    // the band.
    const int gw = (int)nds_gpu3d_output_width();
    const int gh = (int)nds_gpu3d_output_height();
    int vis = 0, visr = 0;
    lb_hud_visible_size(&vis, &visr);
    if (vis <= 0 || vis > gw) vis = gw;
    if (visr <= 0 || visr > gh) visr = gh;
    if (vis < 256) vis = 256;
    if (visr < 192) visr = 192;
    if (vis == 256 && visr == 192) return;       // native view: the stock rule
    const int band = std::clamp(lb_band_render(), 0, gw > 256 ? gw - 256 : 0);
    const int band_y = std::clamp(lb_band_render_y(), 0, gh > 192 ? gh - 192 : 0);
    const int L = (gw - vis) / 2 - band;
    const int T = (gh - visr) / 2 - band_y;
    // p as FUN_0201479C and FUN_0203CA10 compute it, in s16.
    const int tx = bus_read_u8(state + 0x20u), ty = bus_read_u8(state + 0x21u);
    const int16_t camx = (int16_t)bus_read_u16(view + 0x5Eu);
    const int16_t camy = (int16_t)bus_read_u16(view + 0x60u);
    int16_t x = (int16_t)(tx * 24 - camx);
    int16_t y = (int16_t)(ty * 16 - camy);
    x = (int16_t)(x + 4);
    const int16_t x0 = x, y0 = y;                 // for the stock answer
    const int fw = bus_read_u8(state + 4u), fh = bus_read_u8(state + 5u);
    if (y >= T + 8) y = (int16_t)(y - 8);
    else            y = (int16_t)(y + fh * 16);
    if (x < L) x = (int16_t)(x + fw * 24 - 8);
    g_check = CheckStash{true, w, x, y};
    // LB_CHECKLOG=1: the stock rule's point beside this one, when either moves.
    static const bool log = std::getenv("LB_CHECKLOG") != nullptr;
    if (log) {
        const int sy = y0 >= 8 ? y0 - 8 : y0 + fh * 16;
        const int sx = x0 < 0 ? x0 + fw * 24 - 8 : x0;
        static int last[4] = {INT32_MIN, 0, 0, 0};
        if (last[0] != sx || last[1] != sy || last[2] != x || last[3] != y) {
            last[0] = sx; last[1] = sy; last[2] = x; last[3] = y;
            std::fprintf(stderr, "[lb_check] site %d,%d (%dx%d) cam %d,%d "
                         "window L %d T %d: stock %d,%d -> widened %d,%d\n",
                         tx, ty, fw, fh, camx, camy, L, T, sx, sy, x, y);
            std::fflush(stderr);
        }
    }
}

void widget_setpos_observer(void) {
    // Only the checkmark, only from func_02036FBC's widget call, and only the
    // point the stash above was made for.
    if (g_check.pending && g_cpu.R[0] == g_check.widget &&
        (g_cpu.R[14] & ~1u) == kCheckSetPosRet) {
        const uint32_t pt = g_cpu.R[1];
        if (check_in_ram(pt)) {
            bus_write_u16(pt + 4u, (uint16_t)g_check.x);
            bus_write_u16(pt + 6u, (uint16_t)g_check.y);
        }
        g_check.pending = false;
    }
    void (*original)(void) = lb_dispatch_original(0, kWidgetSetPos);
    if (original) original();
}

void fp_post_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kFpPost);
    if (g_fp_pending >= 0) {
        g_fp_pending = -1;
        ++g_fp_saves;
        lb_fp_harvest2(g_fp_pending_shift, g_fp_pending_shift_y);
        // The guest's own window, rebuilt from what it just painted rather
        // than from a copy of what it painted a while ago.
        if (lb_fp_native_rebuild2(g_fp_pending_shift, g_fp_pending_shift_y))
            ++g_fp_restores;
        fp_set_shift(0, 0);
    }
    // docs/383: the site's size in metatiles (this function's state, w at +4,
    // h at +5), so the next paint knows how much of it has to fit.
    {
        const uint32_t st = g_cpu.R[0];
        if (check_in_ram(st)) {
            lb_fp_set_site_size(bus_read_u8(st + 4u), bus_read_u8(st + 5u));
            // docs/392: and the wall line just painted, if it was one (+0x10
            // is the mode func_02037EC0 chose its painter by).
            lb_fp_line_commit(static_cast<int>(bus_read_u32(st + 0x10u)));
        }
    }
    // After the camera is back: the checkmark's point for this call (docs/372).
    checkmark_stash(g_cpu.R[0]);
    if (original) original();
}

// docs/392: every piece of a wall line, as func_02037B9C hands it to the cell
// painter -- including the ones the painter culls at the DS's 32x24. An
// observer: it reads r0 (the view), r1 (the cell, x at +4, y at +5) and r2
// (the tile) and tail-calls the cartridge. Only the line painter's call is
// taken (its bl at 02037E2C); func_020370B4's six calls are harvested by the
// camera shift instead. See lb_fp_line_note.
constexpr uint32_t kFpCell = 0x0203CBB8u;          // ARM
constexpr uint32_t kFpLineCellRet = 0x02037E30u;   // after 02037E2C's bl

void fp_cell_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kFpCell);
    if (g_cpu.R[14] == kFpLineCellRet && lb_fp_placement_active()) {
        const uint32_t cell = g_cpu.R[1];
        if (check_in_ram(cell))
            lb_fp_line_note(g_cpu.R[0], bus_read_u8(cell + 4u),
                            bus_read_u8(cell + 5u),
                            static_cast<int>(g_cpu.R[2] & 0xFFFFu));
    }
    if (original) original();
}

// -- The decompilation's first graded function (docs/332, docs/333) ---------
//
// func_02016A00 is the Battles::RegisteredCreateInfo registry lookup: a key at
// node+0x24, a successor at node+0x04, the head at 0x0215111C+0x40, and -1 as
// the refuse-before-touching-RAM sentinel. src/battles/registry.c reimplements
// it from the disassembly, which is reproduced in that file's header.
//
// THIS IS NOT A PATCH. With LB_SHADOW unset the override does nothing but
// count and tail-call the cartridge, so the control arm is the shipped game
// with one increment added. With LB_SHADOW=native the reimplementation runs
// instead and the cartridge's copy is not entered. Grading is by replaying one
// recorded route under each arm with LB_RAMSUM=N and comparing the hash
// streams; see lb_shadow.h for why a per-call comparison is not available.
//
// WHY THIS FUNCTION FIRST. It is a true leaf -- no direct call and no virtual
// call, which puts it in the 1,480-function tranche docs/332 sized -- it is
// called from 69 sites so a replay exercises it heavily, and it is pure, so a
// divergence can only come from the lookup itself.
constexpr uint32_t kRegistryFind = 0x02016A00u;

// The signed divide-and-remainder every `/` and `%` in the game calls, and the
// second function graded. R0/R1 in, quotient in R0 and remainder in R1.
constexpr uint32_t kDivMod = 0x0211FDB4u;

// VERIFY MODE (docs/333), generalised over probes. Each records the arguments
// the guest arrived with, so the same questions can be put to the cartridge
// out of turn and both answer words compared.
//
// ONLY PURE FUNCTIONS MAY BE PROBED, and this is a hard constraint rather than
// a preference: the probe RE-EXECUTES the cartridge's code, so a function that
// writes memory would apply its effect a second time. Of the 1,480 dependency-
// free functions docs/332 sized, the ones with no store in their decompiler
// output are the gradeable subset; the rest need a different method.
//
// A small ring on purpose: one batch is a fair sample, and an unbounded buffer
// on a path taken 2,663 times in five minutes is a leak waiting to happen.
// Overflow is counted, not silently dropped.
constexpr unsigned kProbeCap = 64u;
// Batched so the drain's own overhead is amortised. FOUR, NOT SIXTEEN: the
// ring only ever drains when it fills, so a function the route calls rarely is
// never graded at all. Battles_IndirectU16IsSet was called 12 times in a
// five-minute replay and reported `graded 0` -- which the grader correctly
// refuses rather than reporting as a pass, but a floor of 16 means every cold
// function silently measures nothing. The batch size does not change the probe
// cost, which is one dispatch per recorded call either way.
constexpr unsigned kProbeBatch = 4u;
constexpr unsigned kProbeMax = 8u;

struct ProbeRing {
    uint32_t a0[kProbeCap];
    uint32_t a1[kProbeCap];
    unsigned n;
    unsigned lost;
};
ProbeRing g_probe[kProbeMax];
// Re-entrancy: the out-of-turn call dispatches to the probed address and lands
// back in its own override. Without the guard it would record its own probe
// arguments into the ring being iterated. One flag covers every probe because
// a drain never runs inside another drain.
bool g_probing = false;

typedef void (*ShadowNativeFn)(uint32_t a0, uint32_t a1,
                              uint32_t* r0, uint32_t* r1);

void shadow_drain(unsigned id, uint32_t addr, const char* name,
                  const char* sweep_name, unsigned resc,
                  ShadowNativeFn native);

// Record this call's arguments, draining the previous batch first.
//
// DRAIN BEFORE RECORDING so the arguments this call is about are not graded in
// the same breath they arrive -- which would put the probe call inside the
// dispatch for the very call it is sampling.
void shadow_record(unsigned id, uint32_t addr, const char* name,
                   const char* sweep_name, unsigned resc,
                   ShadowNativeFn native, uint32_t a0, uint32_t a1) {
    ProbeRing& p = g_probe[id];
    if (p.n >= kProbeBatch)
        shadow_drain(id, addr, name, sweep_name, resc, native);
    if (p.n < kProbeCap) { p.a0[p.n] = a0; p.a1[p.n] = a1; ++p.n; }
    else ++p.lost;
}

void native_registry_find(uint32_t a0, uint32_t, uint32_t* r0, uint32_t* r1) {
    *r0 = Battles_RegistryFind(a0);
    *r1 = 0u;
}

void native_divmod(uint32_t a0, uint32_t a1, uint32_t* r0, uint32_t* r1) {
    Battles_DivMod(a0, a1, r0, r1);
}

void native_split_array(uint32_t a0, uint32_t a1, uint32_t* r0, uint32_t* r1) {
    *r0 = Battles_SplitArrayElem(a0, a1);
    *r1 = 0u;
}

void native_list_find(uint32_t a0, uint32_t a1, uint32_t* r0, uint32_t* r1) {
    *r0 = Battles_ListFindByKey(a0, a1);
    *r1 = 0u;
}

void native_u16_is_set(uint32_t a0, uint32_t, uint32_t* r0, uint32_t* r1) {
    *r0 = Battles_IndirectU16IsSet(a0);
    *r1 = 0u;
}

void native_fadd(uint32_t a0, uint32_t a1, uint32_t* r0, uint32_t* r1) {
    *r0 = Battles_FAdd(a0, a1);
    *r1 = 0u;
}

void native_fsub(uint32_t a0, uint32_t a1, uint32_t* r0, uint32_t* r1) {
    *r0 = Battles_FSub(a0, a1);
    *r1 = 0u;
}

void native_fmul(uint32_t a0, uint32_t a1, uint32_t* r0, uint32_t* r1) {
    *r0 = Battles_FMul(a0, a1);
    *r1 = 0u;
}

// ONE SHAPE, WRITTEN ONCE. Every graded leaf does the same five things and
// differs only in its address, its ring slot, how many words it returns and
// which reimplementation serves it. Five hand-copied bodies would be five
// chances to paste the wrong address into `lb_dispatch_original` -- and an
// override that calls the ORIGINAL of a different function is a silent,
// plausible-looking corruption rather than a crash.
//
// RESC is compared against 1 at compile time: a one-word function must not
// write R1, because R1 is the caller's and the cartridge's own version leaves
// whatever its scratch left there (docs/333 -- reading that scratch is what
// made a correct function fail 635 of 635 calls).
#define LB_GRADED_LEAF(FN, ID, ADDR, NAME, RESC, NATIVE, A0, A1)            \
    void FN(void) {                                                         \
        const int mode = lb_shadow_mode();                                  \
        const bool nat = mode == kLbShadowNative;                           \
        lb_shadow_note(NAME, nat ? 1 : 0);                                  \
        if (mode == kLbShadowVerify && !g_probing)                          \
            shadow_record(ID, ADDR, NAME, NAME "/sweep", RESC, NATIVE,      \
                          A0, A1);                                          \
        if (nat) {                                                          \
            uint32_t q = 0u, s = 0u;                                        \
            NATIVE(A0, A1, &q, &s);                                         \
            g_cpu.R[0] = q;                                                 \
            if ((RESC) > 1u) g_cpu.R[1] = s;                                \
            lb_override_return();                                           \
            return;                                                         \
        }                                                                   \
        void (*orig)(void) = lb_dispatch_original(0, ADDR);                 \
        if (orig) { orig(); return; }                                       \
        static bool said = false;                                           \
        if (!said) {                                                        \
            said = true;                                                    \
            std::fprintf(stderr, "[lb_shadow] no original for %08X; the "    \
                                 "control arm is serving the native "       \
                                 "answer\n", (unsigned)(ADDR));             \
            std::fflush(stderr);                                            \
        }                                                                   \
        uint32_t q = 0u, s = 0u;                                            \
        NATIVE(A0, A1, &q, &s);                                             \
        g_cpu.R[0] = q;                                                     \
        if ((RESC) > 1u) g_cpu.R[1] = s;                                    \
        lb_override_return();                                               \
    }

constexpr uint32_t kSplitArray = 0x020EF1E0u;
constexpr uint32_t kListFind   = 0x0206AC2Cu;
constexpr uint32_t kU16IsSet   = 0x0205E638u;

LB_GRADED_LEAF(split_array_shadow, 2u, kSplitArray, "Battles_SplitArrayElem",
               1u, native_split_array, g_cpu.R[0], g_cpu.R[1])
LB_GRADED_LEAF(list_find_shadow, 3u, kListFind, "Battles_ListFindByKey",
               1u, native_list_find, g_cpu.R[0], g_cpu.R[1])
LB_GRADED_LEAF(u16_is_set_shadow, 4u, kU16IsSet, "Battles_IndirectU16IsSet",
               1u, native_u16_is_set, g_cpu.R[0], 0u)

// The soft-float trio. THEIR RING SLOTS ARE 5, 6 AND 7, which is the whole of
// the remaining capacity -- kProbeMax is 8 and the next function added needs
// it raised. A silent overrun here would index past g_probe and corrupt
// whatever follows it, so the static_assert below refuses the build instead.
constexpr uint32_t kFAdd = 0x0211F324u;
constexpr uint32_t kFMul = 0x0211F748u;
constexpr uint32_t kFSub = 0x0211F934u;

LB_GRADED_LEAF(fadd_shadow, 5u, kFAdd, "Battles_FAdd",
               1u, native_fadd, g_cpu.R[0], g_cpu.R[1])
LB_GRADED_LEAF(fmul_shadow, 6u, kFMul, "Battles_FMul",
               1u, native_fmul, g_cpu.R[0], g_cpu.R[1])
LB_GRADED_LEAF(fsub_shadow, 7u, kFSub, "Battles_FSub",
               1u, native_fsub, g_cpu.R[0], g_cpu.R[1])

static_assert(kProbeMax >= 8u,
              "the graded probes occupy ring slots 0..7; raise kProbeMax "
              "before adding another or shadow_record will index past "
              "g_probe");

// resc = 1: func_02016A00 returns only R0. R1 is scratch -- it holds
// 0215111C, the singleton it loaded -- and comparing it reported 635
// disagreements out of 635 on a correct function (docs/333).
LB_GRADED_LEAF(registry_find_shadow, 0u, kRegistryFind,
               "Battles_RegistryFind", 1u, native_registry_find,
               g_cpu.R[0], 0u)

// Ask the cartridge the same questions the game just asked it, and compare.
//
// WHY IT CANNOT RUN INSIDE THE OVERRIDE FOR THE CALL IT IS GRADING: a
// recompiled body does not return, so an override gets one tail call to the
// original and cannot read its result. The probe therefore grades the PREVIOUS
// batch of keys, at the top of a later call, before that call's own tail call.
//
// AND WHY IT RUNS FROM AN OVERRIDE AT ALL. It was written to run from
// lb_patches_on_vblank and every probe reported "[lb_call] UNWOUND". That hook
// is io.cpp's VCOUNT==192 branch -- the SCHEDULER -- and runtime_dispatch from
// there has no slice to return into. An override body is guest context, which
// is where issue_waiting_to_die already makes its calls.
//
// RE-ENTRANCY: the probe dispatches to this same address and lands back in the
// override. g_probing makes that arrival skip both the drain and the
// recording, so the depth is one and the ring being iterated is not written.
//
// THE WHOLE CPU IS SAVED AND RESTORED. The hook fires between guest
// instructions and the registers belong to whatever was interrupted. call_guest
// writes R14 and R15 and the callee clobbers R0..R3; leaving any of that behind
// would corrupt the guest in a way that looks like memory corruption three
// frames later.
//
// EXTRA CYCLES ARE FINE HERE and are the reason this design exists. The probe
// call costs emulated time, so a verify run is NOT frame-comparable with a
// normal one -- but the grade is a direct per-call comparison and never looks
// at frames, which is exactly what the trace-hash method could not do.
// ONE PROBE CALL, shared by the live drain and the synthetic sweep. Returns
// false when the slice preempted it, in which case nothing was learned and the
// caller must not advance past this input.
bool probe_once(uint32_t addr, const char* name,
                uint32_t a0, uint32_t a1, uint32_t* g0, uint32_t* g1) {
    ArmCpuState saved = g_cpu;
    g_cpu.R[0] = a0;
    g_cpu.R[1] = a1;
    // Not call_guest(): that logs a line per failure, and a preempted probe is
    // expected rather than exceptional here.
    runtime_call_push_return(addr);
    g_cpu.R[14] = addr;
    g_cpu.R[15] = addr;
    runtime_dispatch(addr);
    const bool ok = !runtime_unwinding();
    *g0 = g_cpu.R[0];
    *g1 = g_cpu.R[1];
    if (!ok) {
        // DO NOT CANCEL DURING A SLICE-YIELD UNWIND. runtime_unwinding() is
        // armed by runtime_slice_yield(), whose own contract is that "the
        // BL/BLX return-check uses this to PRESERVE the pending return-push
        // instead of cancelling it -- the call-return stack is saved/restored
        // per-CPU across the preemption". Cancelling there would destroy a
        // return the runtime intends to resume. A marker is only orphaned when
        // the call failed for some OTHER reason.
        //
        // A probe is preemptible whenever the target has a backward branch,
        // because that is exactly where runtime_slice_yield() is checked.
        if (!runtime_unwinding()) runtime_call_cancel_return(addr);
        lb_shadow_note_refused(name);
    }
    g_cpu = saved;
    return ok;
}

// ── The synthetic sweep ────────────────────────────────────────────────────
//
// WHY IT EXISTS. The live drain can only grade the arguments the route
// supplied, and for the soft-float routines that was ONE distinct first
// operand across 11,358 calls -- five test cases wearing a big number
// (docs/333). For a REGISTER-PURE function the probe does not need the game's
// arguments: it can call the cartridge with chosen bit patterns and compare.
//
// SAFE ONLY FOR REGISTER-PURE FUNCTIONS, and this is the whole constraint.
// The probe re-executes the cartridge's code, so a function that reads memory
// would be handed a fabricated pointer and one that writes would write through
// it. Only functions whose arguments and result live entirely in registers may
// appear in the table below: the signed divide and the three soft-float
// routines. The list walks and the array indexer must never be added.
//
// It runs from the drain, which is already guest context with g_probing set,
// and advances a bounded number of pairs per call so a sweep of hundreds of
// combinations spreads across a session instead of stalling a frame.
struct SweepPlan {
    const uint32_t* pat;
    unsigned n;
    unsigned i, j;        // cursor over the pat x pat grid
    bool done;
};

// BIT PATTERNS, NOT VALUES. These are fed to the cartridge as raw words, which
// is what its callers pass, and they are chosen for the places a soft-float
// implementation actually goes wrong rather than for being round numbers:
// both zeros, both infinities, a quiet and a signalling NaN, the largest
// denormal and the smallest normal either side of the boundary, and FLT_MAX.
const uint32_t kFloatPats[] = {
    0x00000000u, 0x80000000u,   /* +0, -0                           */
    0x00000001u, 0x80000001u,   /* smallest denormals               */
    0x007FFFFFu, 0x00800000u,   /* largest denormal / smallest normal */
    0x3F800000u, 0xBF800000u,   /* +1, -1                           */
    0x3F000000u, 0x40000000u,   /* 0.5, 2                           */
    0x3EAAAAABu, 0x4048F5C3u,   /* 1/3, 3.14                        */
    0x41200000u, 0x461C4000u,   /* 10, 10000                        */
    0x33800000u, 0x4B7FFFFFu,   /* tiny normal, 2^24-ish            */
    0x7F7FFFFFu, 0xFF7FFFFFu,   /* +FLT_MAX, -FLT_MAX               */
    0x7F800000u, 0xFF800000u,   /* +Inf, -Inf                       */
    0x7FC00000u, 0x7F800001u,   /* quiet NaN, signalling NaN        */
};

// For the signed divide. ZERO IS IN HERE ON PURPOSE: the ROM does not trap on
// it -- it returns (q = a, r = 0) -- and that is precisely the behaviour a
// native port would destroy by writing `a / b`. INT_MIN is here for the other
// documented edge, where `rsblt` cannot negate it.
const uint32_t kIntPats[] = {
    0x00000000u, 0x00000001u, 0xFFFFFFFFu, 0x00000002u, 0xFFFFFFFEu,
    0x00000007u, 0xFFFFFFF9u, 0x0000000Au, 0x00000064u, 0x000003E8u,
    0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0x0000FFFFu, 0xFFFF0000u,
    0x40000000u, 0xC0000000u, 0x00001000u,
};

SweepPlan g_sweep[kProbeMax] = {};

void sweep_init(void) {
    static bool done = false;
    if (done) return;
    done = true;
    // Ring slots, from the LB_GRADED_LEAF registrations below. Only the four
    // register-pure functions are listed; every other slot stays null and is
    // never swept.
    g_sweep[1].pat = kIntPats;                         /* kDivMod */
    g_sweep[1].n = (unsigned)(sizeof kIntPats / sizeof kIntPats[0]);
    for (unsigned id = 5u; id <= 7u; ++id) {           /* kFAdd/kFMul/kFSub */
        g_sweep[id].pat = kFloatPats;
        g_sweep[id].n = (unsigned)(sizeof kFloatPats / sizeof kFloatPats[0]);
    }
}

// Graded under "<name>/sweep" so synthetic coverage is reported BESIDE the
// live coverage and never added to it: hundreds of chosen pairs would drown
// the handful the route produced, and `graded` would stop meaning "the game
// did this".
void shadow_sweep(unsigned id, uint32_t addr, const char* sweep_name,
                  unsigned resc, ShadowNativeFn native) {
    sweep_init();
    SweepPlan& s = g_sweep[id];
    if (!s.pat || s.done) return;

    for (unsigned budget = 0u; budget < 8u; ++budget) {
        if (s.i >= s.n) { s.done = true; return; }
        const uint32_t a0 = s.pat[s.i], a1 = s.pat[s.j];
        uint32_t g0 = 0u, g1 = 0u;
        if (!probe_once(addr, sweep_name, a0, a1, &g0, &g1)) return;
        uint32_t n0 = 0u, n1 = 0u;
        native(a0, a1, &n0, &n1);
        lb_shadow_compare(sweep_name, resc, a0, a1, g0, g1, n0, n1);
        if (++s.j >= s.n) { s.j = 0u; ++s.i; }
    }
}

void shadow_drain(unsigned id, uint32_t addr, const char* name,
                  const char* sweep_name, unsigned resc,
                  ShadowNativeFn native) {
    ProbeRing& p = g_probe[id];
    if (lb_shadow_mode() != kLbShadowVerify) { p.n = 0u; return; }
    if (p.n == 0u) return;

    const unsigned n = p.n;
    p.n = 0u;
    g_probing = true;

    for (unsigned i = 0; i < n; ++i) {
        const uint32_t a0 = p.a0[i], a1 = p.a1[i];

        ArmCpuState saved = g_cpu;
        g_cpu.R[0] = a0;
        g_cpu.R[1] = a1;
        // Not call_guest(): that logs a line per failure, and a preempted
        // probe is expected rather than exceptional here.
        runtime_call_push_return(addr);
        g_cpu.R[14] = addr;
        g_cpu.R[15] = addr;
        runtime_dispatch(addr);
        const bool ok = !runtime_unwinding();
        const uint32_t g0 = g_cpu.R[0], g1 = g_cpu.R[1];
        if (!ok) {
            // DO NOT CANCEL DURING A SLICE-YIELD UNWIND. runtime_unwinding()
            // is armed by runtime_slice_yield(), and its own contract is that
            // "the BL/BLX return-check uses this to PRESERVE the pending
            // return-push instead of cancelling it -- the call-return stack is
            // saved/restored per-CPU across the preemption". Cancelling there
            // would destroy a return the runtime intends to resume. A marker
            // is only orphaned when the call failed for some OTHER reason.
            //
            // This is why func_02016A00 is preemptible at all: its list walk
            // ends in a backward branch, and runtime_slice_yield() is checked
            // exactly at backward branches.
            if (!runtime_unwinding()) runtime_call_cancel_return(addr);
        }
        g_cpu = saved;

        if (!ok) {
            // STOP THE BATCH. The host stack is unwinding for a slice yield;
            // every further probe would unwind too, and continuing to push
            // markers into an unwind is how the earlier version produced 876
            // failures in one run.
            lb_shadow_note_refused(name);
            break;
        }
        uint32_t n0 = 0u, n1 = 0u;
        native(a0, a1, &n0, &n1);
        lb_shadow_compare(name, resc, a0, a1, g0, g1, n0, n1);
    }

    // Still inside g_probing and still guest context: advance the synthetic
    // sweep for the register-pure functions. A no-op for every other id.
    shadow_sweep(id, addr, sweep_name, resc, native);

    g_probing = false;
}

// func_0211FDB4, the signed divide. Same shape as the registry probe, and the
// reason lb_shadow_compare takes two result words: the quotient comes back in
// R0 and the remainder in R1, so grading R0 alone would pass an
// implementation whose remainder was wrong at every call.
// resc = 2: quotient in R0, remainder in R1. Grading R0 alone would pass an
// implementation whose remainder was wrong at every call.
LB_GRADED_LEAF(divmod_shadow, 1u, kDivMod, "Battles_DivMod", 2u,
               native_divmod, g_cpu.R[0], g_cpu.R[1])

// ── STEADY PACING: NitroMain's two clock reads count VBlanks (docs/351) ─────
//
// NitroMain waits for a VBlank, runs the task list, and waits for one more if
// OS_GetMilliseconds says under 20 ms passed since just before the first wait.
// Any fixed millisecond threshold is wrong in one regime: the pass after an
// OVERRUN always measures ~33.4 ms (the first wait shrinks by exactly the
// overrun), so 20 ms judders a light game 1-2-1-2 and 33 ms stalls a heavy one
// 2-3-2-3 (measured, docs/351 section 6: 24-33% three-VBlank passes after the
// replay at every window size).
//
// The rule the loop is reaching for is "wait again only if no VBlank came
// while the task list ran". It is exactly expressible without touching guest
// code: at NitroMain's two call sites only, OS_GetMilliseconds answers with the
// cartridge's own VBlank counter x 17. A pass whose task list ends inside its
// frame then measures 17 (< 20: the second wait), one that ran past a VBlank
// measures 34 or more (no second wait), whatever the ARM9's timing -- passes
// two VBlanks apart for any task list under two frames. Every other caller
// (24 of 25), and every pass when pacing is the cartridge's, gets the real
// clock: the cartridge's own body, run unchanged.
constexpr uint32_t kGetMs = 0x020F2648u;          // OS_GetMilliseconds, ARM
constexpr uint32_t kVBlankCounter = 0x02158A58u;  // ++ in the VBlank ISR, 020F22E8
constexpr uint32_t kLoopT0Ret = 0x020F68D4u;      // NitroMain: t0 = ...()
constexpr uint32_t kLoopT1Ret = 0x020F6908u;      // NitroMain: t1, after the tasks

void get_ms_override(void) {
    const uint32_t lr = g_cpu.R[14];
    if ((lr == kLoopT0Ret || lr == kLoopT1Ret) && lb_cadence_effective() == 30) {
        const uint64_t t = (uint64_t)bus_read_u32(kVBlankCounter) * 17u;
        g_cpu.R[0] = (uint32_t)t;           // u64 in r0:r1, as the real one
        g_cpu.R[1] = (uint32_t)(t >> 32);
        lb_override_return();
        return;
    }
    void (*original)(void) = lb_dispatch_original(0, kGetMs);
    if (original) original();
}

// LB_GATHERLOG=1 (docs/357): HOW MANY TILES THE ENTITY GATHER SCANS A FRAME.
//
// FUN_0207E9B4 is the hottest guest function at a 3440x1440 window (4.5% of the main
// thread, and more: its callees run as separate host frames). It walks a tile
// rectangle -- x from rect+0x08 to +0x10, y from +0x09 to +0x11, inclusive
// bytes, R2 = rect -- and per tile looks up the entities there. Its cost is
// the rectangle's area, and one of its callers is the render gather whose
// rectangle this port widens to the surface (apply_cull_width). This says,
// per frame and per caller, how many tiles are scanned. Observer only: it
// reads the rectangle and runs the cartridge's function unchanged.
constexpr uint32_t kTileGather = 0x0207E9B4u;   // arm
void tile_gather_observer(void) {
    static const bool on = std::getenv("LB_GATHERLOG") != nullptr;
    if (on) {
        struct Site { uint32_t lr; uint64_t calls, tiles; };
        static Site sites[8];
        static uint32_t last_vb = 0, frames = 0;
        static uint64_t calls = 0, tiles = 0;
        const uint32_t rect = g_cpu.R[2];
        const int x0 = bus_read_u8(rect + 0x08u), x1 = bus_read_u8(rect + 0x10u);
        const int y0 = bus_read_u8(rect + 0x09u), y1 = bus_read_u8(rect + 0x11u);
        const uint64_t area = (x1 >= x0 && y1 >= y0)
            ? uint64_t(x1 - x0 + 1) * uint64_t(y1 - y0 + 1) : 0u;
        const uint32_t lr = g_cpu.R[14];
        ++calls;
        tiles += area;
        for (Site& s : sites) {
            if (s.lr == lr || s.lr == 0u) {
                s.lr = lr;
                ++s.calls;
                s.tiles += area;
                break;
            }
        }
        const uint32_t vb = bus_read_u32(kVBlankCounter);
        if (vb != last_vb) {
            last_vb = vb;
            if (++frames == 300u) {
                std::fprintf(stderr,
                             "[lb_gather] 300 VBlanks: %.1f calls, %.0f tiles "
                             "scanned per VBlank; by caller:",
                             calls / 300.0, tiles / 300.0);
                for (const Site& s : sites)
                    if (s.lr)
                        std::fprintf(stderr, " %08X %.1f calls %.0f tiles;",
                                     (unsigned)s.lr, s.calls / 300.0,
                                     s.tiles / 300.0);
                std::fprintf(stderr, " (last rect x %d..%d y %d..%d)\n",
                             x0, x1, y0, y1);
                std::fflush(stderr);
                frames = 0;
                calls = tiles = 0;
                for (Site& s : sites) s = Site{};
            }
        }
    }
    void (*original)(void) = lb_dispatch_original(0, kTileGather);
    if (original) original();
}

// LB_MARKERLOG=1: WHEN DOES THE MISSION MARKER FIRE?
//
// The marker function, thumb, (owner, x, y): a four-second marker for a point
// of interest. It tests the point against the DS's own 256x192
// (FUN_020A8DD0, fourth argument 0). A point outside it is placed at the
// point minus the camera (view +0x5E/+0x60, view = **0x020DAC78),
// clamped to [4,236] x [4,172], snapped to 8 px and handed to the widget
// at owner+0x1E4 as two 8-bit values. In a widened view, a point the
// player can see in a band is therefore marked at the native edge.
//
// Its entry is 0x020DAAE4 (push {r4-r7,lr}; the clock read is kGetMs), called
// by the mission code at 02048A78 as (obj+0x70, point.x - 128,
// point.y - 96). Checked against out/eur/arm9/section_static_02000000.bin,
// the image Ghidra and the recompiler use. A scratch decompression that
// differs from 0x0200000E on is shifted 0xF0 here, and briefly made this
// look like a label.
//
// Nothing replays a campaign mission yet, so this says when it fires and
// where the point is against the surface. Observer only: it reads its
// arguments and runs the cartridge's function unchanged.
constexpr uint32_t kMarkerPlace = 0x020DAAE4u;   // thumb
void marker_place_observer(void) {
    static const bool on = std::getenv("LB_MARKERLOG") != nullptr;
    if (on) {
        const int x = (int)g_cpu.R[1], y = (int)g_cpu.R[2];
        const uint32_t view_pp = bus_read_u32(0x020DAC78u);
        const uint32_t view = view_pp ? bus_read_u32(view_pp) : 0u;
        const int camx = view ? (int16_t)bus_read_u16(view + 0x5Eu) : 0;
        const int camy = view ? (int16_t)bus_read_u16(view + 0x60u) : 0;
        const int sx = x - camx, sy = y - camy;
        const bool native = sx >= 0 && sx < 256 && sy >= 0 && sy < 192;
        std::fprintf(stderr,
                     "[lb_marker] VBlank %u lr %08X owner %08X point (%d,%d) "
                     "camera (%d,%d) -> screen (%d,%d) %s\n",
                     (unsigned)bus_read_u32(kVBlankCounter),
                     (unsigned)g_cpu.R[14], (unsigned)g_cpu.R[0], x, y,
                     camx, camy, sx, sy,
                     native ? "inside the DS screen"
                            : "OUTSIDE the DS screen: clamped to its edge");
        std::fflush(stderr);
    }
    /* The first argument is the CPU (ARM9), not the instruction set. */
    void (*original)(void) = lb_dispatch_original(0, kMarkerPlace);
    if (original) original();
}

// ── The widget hit test, told where the port drew the widget (docs/371) ─────
//
// FUN_020EA1A4 (Thumb) is the cartridge's rectangle test for a button widget,
// called by the hit test (FUN_020E5610) and by the re-test while a button is
// held (FUN_020E5C34):
//
//   r0 = the record's margins (record+0x20), r1 = the widget, r2 = x, r3 = y
//   record = [W+4]; group = [record+0x14], offset s16 [+8]/[+0xA]
//   hit iff x0 + gx + m0*8 <= x <= x1 + gx + m2*8, likewise y (signed, inclusive)
//   x0 = s16 [record+6], x1 = [+0xA], y0 = [+4], y1 = [+8]
//
// The port draws moved HUD elements somewhere else; this makes the test answer
// for where the widget is DRAWN (lb_hud_hit_unmap). On its face: the native
// point under the press, so the cartridge's own test hits. Off it: a point no
// widget can hold, so the manager gives the press to the field (FUN_020E489C
// runs the screens' handlers, and the battle view's calls the field click,
// when no widget consumed it). Then a single tail call of the cartridge's own
// test. Nothing is decided here that the cartridge does not decide.
constexpr uint32_t kWidgetRect = 0x020EA1A4u;   // thumb

extern "C" int lb_hud_hit_live(void) {
    if (!lb_setting_enhanced()) return 0;
    static int env = -1;
    if (env < 0) {
        const char* v = std::getenv("LB_HUDHIT");
        env = (v && v[0] == '0') ? 0 : 1;
    }
    if (!env) return 0;
    // An older recording holds its HUD clicks already rewritten to native
    // points; it replays through the old path (lb_demo.cpp, hudhit=).
    return lb_demo_play_hudhit() != 0 ? 1 : 0;
}

// docs/396: ONLY THE BATTLE VIEW'S OWN WIDGETS. The HUD faces are scanned in
// the battle composite alone, so in every other screen the last battle
// frame's boxes stayed published -- and FUN_020EA1A4 tests every screen's
// buttons. A menu button whose rectangle matched a moved HUD element's
// source -- the back arrow, 4..28 x 168..192, is exactly where the deselect
// icon sits -- was taken for that element and missed every press: the back
// arrow after a battle, the Free Play army builder's slots, at the sizes
// where those HUD elements had moved. So the correction applies only to a
// widget in the layout of the battle view (registry slot 0x5C, 02157FAC,
// vtable 0214F0EC: kViewSlot/kViewVtable below), whose HUD it describes.
static bool widget_in_battle_view(uint32_t w) {
    // LB_HUDHITOWN=0 restores the old rule (every screen's widgets), for the
    // A/B that shows what this fixes.
    static const bool any = [] {
        const char* v = std::getenv("LB_HUDHITOWN");
        return v && v[0] == '0';
    }();
    if (any) return true;
    const uint32_t screen = bus_read_u32(0x02157FACu);
    if (screen < 0x02000000u || screen >= 0x02400000u) return false;
    if (bus_read_u32(screen) != 0x0214F0ECu) return false;
    const uint32_t layout = bus_read_u32(screen + 0x1Cu);
    if (layout < 0x02000000u || layout >= 0x02400000u) return false;
    const unsigned n = bus_read_u8(layout + 2u);
    for (unsigned i = 0; i < n && i < 128u; ++i)
        if (bus_read_u32(layout + 0xCu + 4u * i) == w) return true;
    return false;
}

void widget_rect_observer(void) {
    if (lb_hud_hit_live() && widget_in_battle_view(g_cpu.R[1])) {
        const uint32_t w = g_cpu.R[1];
        const uint32_t rec = (w >= 0x02000000u && w < 0x02400000u)
                                 ? bus_read_u32(w + 4u) : 0u;
        if (rec >= 0x02000000u && rec < 0x02400000u) {
            const uint32_t marg = g_cpu.R[0];
            int m[4] = {0, 0, 0, 0};
            if (marg >= 0x02000000u && marg < 0x02400000u)
                for (int i = 0; i < 4; ++i)
                    m[i] = (int8_t)bus_read_u8(marg + (uint32_t)i);
            int gx = 0, gy = 0;
            const uint32_t grp = bus_read_u32(rec + 0x14u);
            if (grp >= 0x02000000u && grp < 0x02400000u) {
                gx = (int16_t)bus_read_u16(grp + 8u);
                gy = (int16_t)bus_read_u16(grp + 0xAu);
            }
            const int x0 = (int16_t)bus_read_u16(rec + 6u) + gx + m[0] * 8;
            const int x1 = (int16_t)bus_read_u16(rec + 0xAu) + gx + m[2] * 8;
            const int y0 = (int16_t)bus_read_u16(rec + 4u) + gy + m[1] * 8;
            const int y1 = (int16_t)bus_read_u16(rec + 8u) + gy + m[3] * 8;
            int x = (int)g_cpu.R[2], y = (int)g_cpu.R[3];
            const int r = lb_hud_hit_unmap(x0, y0, x1, y1, &x, &y);
            // LB_HUDHITLOG=1: every answer that is not "as stock", capped.
            static const bool hitlog = std::getenv("LB_HUDHITLOG") != nullptr;
            static unsigned said = 0;
            if (hitlog && r != 0 && said < 200u) {
                ++said;
                std::fprintf(stderr,
                             "[lb_hudhit] widget %u rect %d..%d x %d..%d: "
                             "point %d,%d -> %s %d,%d (lr %08X)\n",
                             (unsigned)bus_read_u16(rec + 2u), x0, x1, y0, y1,
                             (int)g_cpu.R[2], (int)g_cpu.R[3],
                             r > 0 ? "ON ITS FACE, native" : "moved away, MISS",
                             x, y, (unsigned)g_cpu.R[14]);
                std::fflush(stderr);
            }
            if (r > 0) {
                g_cpu.R[2] = (uint32_t)x;
                g_cpu.R[3] = (uint32_t)y;
            } else if (r < 0) {
                g_cpu.R[2] = (uint32_t)-0x4000;   // left of any widget: a miss
            }
        }
    }
    void (*original)(void) = lb_dispatch_original(0, kWidgetRect);
    if (original) original();
}

// ---- docs/383: BATTLE SHORTCUTS RUN THE BUTTON'S OWN CALLBACK ---------------
//
// Space and right-click Deselect, Mouse 5 opens the blue brick menu, Mouse 4
// the red/yellow/white one -- each invokes the function, never a tap on the
// screen. So nothing here touches the touchscreen: a shortcut
// runs the code the cartridge runs when the button is clicked, with the
// arguments its widget manager passes, and no pen state is involved at all.
//
// WHAT THE CARTRIDGE RUNS (static trace, checked against the ARM9 image):
//   * Every button of the battle HUD has the same callback, FUN_020DC298
//     (Thumb), stored in W+8 by WorldViewScreen::virtual_08 at 020DBAD8. The
//     pen handler FUN_020E489C calls it at 020E49C8 with r0 = the widget,
//     r1 = the PenEvent, r2 = the screen, r3 = 1, and it switches on the
//     widget id (id - 0x231, table at 020DC34C).
//   * It acts on a CLICK: FUN_020E5628 returns 1 for a touch event
//     (E+0xC == 1, u32) whose phase is a release (E+0x18 == 1, u32; the test
//     is 0200AAC8). On a press it only raises the hint label.
//   * 628 Deselect runs input action 5 (FUN_020E23AC -> FUN_020EC7B0 on the
//     selection, FUN_020A8A24(0), FUN_020E3078(1)). 605 (blue, group 0), 606
//     (red, group 3), 608 (hero spells, group 2) and 607 (upgrades, group 1)
//     toggle their list through FUN_020DA6BC(screen, group) and post the
//     GameMenuEvent the tutorials wait for. All of it is client-local: no
//     simulation command is submitted.
//   * The callback plays the click before its own hidden test, so the gates
//     the pen path applies are applied here first (quick_gate).
//
// HOW IT IS CALLED, WITHOUT A NESTED GUEST CALL (docs/369:
// a nested call can be preempted and re-enters its caller). The call is made
// in front of the view's own per-frame update, WorldViewScreen::virtual_09
// (020DD708, Thumb, reached only through its vtable), as a TAIL call whose
// return is pointed back at 020DD708. The update's registers are kept here;
// when the callback returns into 020DD708 this override runs again, sees the
// stack pointer it gave the callback, puts the registers back and runs the
// update as normal. A slice that ends inside the callback simply resumes it.
constexpr uint32_t kViewUpdate = 0x020DD708u;      // thumb
constexpr uint32_t kHudButtonCb = 0x020DC298u;     // thumb
constexpr uint32_t kViewSlot = 0x02157FACu;        // registry slot 0x5C
constexpr uint32_t kViewVtable = 0x0214F0ECu;
constexpr uint32_t kPenEventVtable = 0x0213B998u;

extern "C" void lb_demo_note_quick(int action);   /* src/lb_demo.cpp */
extern "C" int lb_demo_playing(void);

namespace {
// -1 none; 0 Deselect, 1 the blue brick, 2 the red/yellow/white brick.
std::atomic<int> g_quick_req{-1};

struct QuickCall {
    bool active = false;
    uint32_t r[4] = {0, 0, 0, 0};
    uint32_t r12 = 0, lr = 0, sp = 0;
    uint32_t sp_call = 0;       // the SP the callback was entered with
    uint32_t screen = 0;
    int action = -1, id = 0;
};
QuickCall g_qc;

bool quick_log() {
    static const bool on = std::getenv("LB_QUICKLOG") != nullptr;
    return on;
}

bool guest_ptr(uint32_t p) { return p >= 0x02000000u && p < 0x02400000u; }

// The widget for an id, from the screen's layout: `n` widgets at
// layout+0xC. 0 if the layout does not hold it.
uint32_t quick_find_widget(uint32_t layout, int id) {
    const unsigned n = bus_read_u8(layout + 2u);
    for (unsigned i = 0; i < n && i < 128u; ++i) {
        const uint32_t w = bus_read_u32(layout + 0xCu + 4u * i);
        if (!guest_ptr(w)) continue;
        const uint32_t rec = bus_read_u32(w + 4u);
        if (guest_ptr(rec) && bus_read_u16(rec + 2u) == (uint32_t)id) return w;
    }
    return 0;
}

// Would the cartridge's pen path deliver a click on this widget now? The
// tests FUN_020E489C/FUN_020E3EAC and the callback make, in the order the
// static trace gives them. nullptr means yes.
const char* quick_widget_ok(uint32_t w) {
    const uint32_t rec = bus_read_u32(w + 4u);
    if (!guest_ptr(rec)) return "no record";
    if (bus_read_u32(w + 8u) != (kHudButtonCb | 1u)) return "not a HUD button";
    const uint8_t f = bus_read_u8(rec);
    const uint32_t grp = bus_read_u32(rec + 0x14u);   // FUN_020EA4B0
    if ((f & 2u) || (guest_ptr(grp) && bus_read_u8(grp + 1u)))
        return "hidden";
    if ((f & 1u) && bus_read_u32(w + 0x10u) == 0u) return "disabled";
    // A button that tracks drags answers a release with 9, not 1, unless it
    // was pressed; none of these has been seen to, and it is not faked.
    if (bus_read_u8(rec + 0x2Fu)) return "drag-tracking button";
    return nullptr;
}

// The tests on the view itself, before any button's.
const char* quick_screen_gate(uint32_t screen) {
    if (screen != bus_read_u32(kViewSlot)) return "not the battle view";
    if (bus_read_u32(screen) != kViewVtable) return "not the battle view";
    const uint32_t layout = bus_read_u32(screen + 0x1Cu);
    if (!guest_ptr(layout)) return "no layout";
    const uint8_t lf = bus_read_u8(layout);
    if ((lf & 5u) != 5u) return "layout not live";
    if (((lf >> 1) & 1u) == bus_read_u8(0x02151344u)) return "layout not live";
    // The UI manager at 021581A4 (FUN_020E489C): +1 switches pen handling off
    // altogether, and that holds here too. +8 says who owns the press in
    // progress: 1 a widget, 2 the field (the rest of that press skips the
    // widgets), 0 none. 2 only routes PEN events, and a shortcut is not one:
    // it runs while the player holds a press or drags a box on the field
    // (run t383l first refused it there). 1 is refused, because the held
    // button's own release would act on it a second time.
    if (bus_read_u8(0x021581A5u)) return "UI input off";
    if (bus_read_u32(0x021581ACu) == 1u) return "a button is held";
    if (bus_read_u32(0x0214BBA8u) != 5u) return "not in play";
    if (bus_read_u8(screen + 0x22Cu)) return "HUD suspended by the mission";
    return nullptr;
}

const char* quick_gate(uint32_t screen, int action, uint32_t* w_out,
                       int* id_out) {
    if (const char* why = quick_screen_gate(screen)) return why;
    const uint32_t layout = bus_read_u32(screen + 0x1Cu);
    // Mouse 4's brick is whichever of red, yellow and white is shown.
    static const int kIds[3][3] = {{628, 0, 0}, {605, 0, 0}, {606, 608, 607}};
    const char* why = "not shown";
    for (int k = 0; k < 3 && kIds[action][k]; ++k) {
        const uint32_t w = quick_find_widget(layout, kIds[action][k]);
        if (!w) continue;
        why = quick_widget_ok(w);
        if (!why) {
            *w_out = w;
            *id_out = kIds[action][k];
            return nullptr;
        }
    }
    return why;
}

// ---- docs/385: ITEM N OF THE SECOND BRICK'S LIST ----
//
// A number key N picks item N of the list the second HUD brick opens -- red
// for a builder, yellow for a hero -- so with Space Criminal Leader selected,
// pressing 4 arms Hotwire.
// Requests 11..19 are item 1..9 of the list that Mouse 4's brick opens.
//
// THE LISTS (FUN_020DD414, the layout pass, and FUN_020DA6BC, the toggle).
// List g is at screen+0xBC+0x30*g; its items are an array of POINTERS at
// list+0x10, each to an entry {widget*, payload}. The layout lays a list's
// shown items out left to right in array order -- a widget whose record has
// bit 1 set is skipped -- over 9 entries for g0 (blue), 20 for g1 (white),
// 31 for g2 (yellow) and 20 each for g3 and g4 (red), and the two red rows
// count only entries with a payload (the blueprint). g4 is the red list's
// SECOND ROW: opening list 3 opens drawer 4 too when the red element is two
// rows high (0x30). So item N is counted along g, then g4 for red.
//
// THE ITEM'S CALLBACK is FUN_020DC298, the bricks' own: the widget id selects
// the case. 648..656 are a builder's buildings (FUN_020D97EC finds the entry
// in g3, else g4; its payload goes to 0214BB00, input action 0xB, the list
// closes); 677..707 are the heroes' spells (FUN_020E62C4 finds it in g2; its
// byte goes to 021575B8, input action 0xF, the list closes). A disabled item
// plays the error sound and says why, as a tap on it would. But the callback
// acts only while its list is the open one (FUN_020DA2B8 -> FUN_020DA3E0:
// screen+0x26C == g, with 3 and 4 counted as one), so a closed list is opened
// first with its brick's callback -- the player's own two taps -- and the item
// follows when that returns, or on a later update if the list is not shown
// yet (a few updates, then it gives up).
constexpr int kQuickItem = 10;
// docs/398: 21..29 are item 1..9 of the BLUE brick's list (g0: ids 614..623,
// which FUN_020DA2B8 gives to list 0). A number key becomes one of these when
// the option is on and the blue list is the open one (screen+0x26C == 0; 5 is
// none, FUN_020DA6BC). The choice is made here and recorded as made, so a
// recording replays the same whatever the setting.
constexpr int kQuickBlueItem = 20;

struct QuickItem {
    int n = 0;                 // 1..9 while one is wanted
    int group = -1;
    uint32_t screen = 0;
    int updates = 0;           // left to wait for it
};
QuickItem g_qitem;

int quick_list_of(int brick_id) {
    return brick_id == 606 ? 3 : brick_id == 608 ? 2 : brick_id == 607 ? 1 : -1;
}

bool quick_list_open(uint32_t screen, int g) {
    const uint32_t open = bus_read_u32(screen + 0x26Cu);
    return open == (uint32_t)g || (g >= 3 && open - 3u < 2u);
}

// Item n (1-based) of list g as the layout shows it; 0 if there is none.
uint32_t quick_list_item(uint32_t screen, int g, int n) {
    static const struct { uint32_t items; unsigned count; bool payload; }
        kList[5] = {{0xCCu, 9u, false}, {0xFCu, 20u, false},
                    {0x12Cu, 31u, false}, {0x15Cu, 20u, true},
                    {0x18Cu, 20u, true}};
    const int rows[2] = {g, g == 3 ? 4 : -1};
    int k = 0;
    for (const int r : rows) {
        if (r < 0) continue;
        const uint32_t arr = bus_read_u32(screen + kList[r].items);
        if (!guest_ptr(arr)) continue;
        for (unsigned i = 0; i < kList[r].count; ++i) {
            const uint32_t e = bus_read_u32(arr + 4u * i);
            if (!guest_ptr(e)) continue;
            const uint32_t w = bus_read_u32(e);
            if (!guest_ptr(w)) continue;
            if (kList[r].payload && bus_read_u32(e + 4u) == 0u) continue;
            const uint32_t rec = bus_read_u32(w + 4u);
            if (!guest_ptr(rec) || (bus_read_u8(rec) & 2u)) continue;
            // A row not shown (the red list's second, when it is one row).
            const uint32_t grp = bus_read_u32(rec + 0x14u);
            if (guest_ptr(grp) && bus_read_u8(grp + 1u)) continue;
            if (++k == n) return w;
        }
    }
    return 0;
}
}  // namespace

// The frontend's request (UI thread). A demo being replayed owns the input.
extern "C" void lb_quick_request(int action) {
    const bool ok = (action >= 0 && action <= 2) ||
                    (action > kQuickItem && action <= kQuickItem + 9);
    if (!ok || lb_demo_playing()) return;
    g_quick_req.store(action);
}
// A replayed `q` event (lb_demo_tick): run it in this frame, as recorded.
extern "C" void lb_keyboard_type(int ch);   /* docs/396, below */
extern "C" void lb_quick_replay(int action) {
    // docs/396: a key typed on one of the game's keyboards (0x10000 | char).
    if (action >= 0x10000) {
        lb_keyboard_type(action);
        return;
    }
    if ((action >= 0 && action <= 2) ||
        (action > kQuickItem && action <= kQuickItem + 9) ||
        (action > kQuickBlueItem && action <= kQuickBlueItem + 9))
        g_quick_req.store(action);
}
extern "C" int lb_setting_quick_blue(void);   /* lb_overlay.cpp */

namespace {
// The update's registers, kept for when the callback returns.
void quick_save() {
    g_qc.r[0] = g_cpu.R[0];
    g_qc.r[1] = g_cpu.R[1];
    g_qc.r[2] = g_cpu.R[2];
    g_qc.r[3] = g_cpu.R[3];
    g_qc.r12 = g_cpu.R[12];
    g_qc.lr = g_cpu.R[14];
    g_qc.sp = g_cpu.R[13];
}

// Enter widget w's callback as a tail call that returns into 020DD708, below
// the update's own stack (g_qc.sp, from quick_save).
void quick_enter(uint32_t w, int id, uint32_t screen, int action) {
    // The PenEvent the widget manager would pass for a click: vtable, touch
    // (E+0xC = 1), release (E+0x18 = 1), and the widget's centre, which
    // nothing on this path reads.
    const uint32_t ev = (g_qc.sp - 0x40u) & ~7u;
    const uint32_t rec = bus_read_u32(w + 4u);
    const int cx = ((int16_t)bus_read_u16(rec + 6u) +
                    (int16_t)bus_read_u16(rec + 0xAu)) / 2;
    const int cy = ((int16_t)bus_read_u16(rec + 4u) +
                    (int16_t)bus_read_u16(rec + 8u)) / 2;
    bus_write_u32(ev + 0x00u, kPenEventVtable);
    bus_write_u32(ev + 0x04u, 0u);
    bus_write_u32(ev + 0x08u, 0u);
    bus_write_u32(ev + 0x0Cu, 1u);
    bus_write_u32(ev + 0x10u, 0u);
    bus_write_u32(ev + 0x14u, 0u);
    bus_write_u32(ev + 0x18u, 1u);
    bus_write_u16(ev + 0x1Cu, (uint16_t)cx);
    bus_write_u16(ev + 0x1Eu, (uint16_t)cy);
    g_qc.sp_call = ev - 8u;
    g_qc.screen = screen;
    g_qc.action = action;
    g_qc.id = id;
    g_qc.active = true;
    g_cpu.R[0] = w;
    g_cpu.R[1] = ev;
    g_cpu.R[2] = screen;
    g_cpu.R[3] = 1u;
    g_cpu.R[13] = g_qc.sp_call;
    g_cpu.R[14] = kViewUpdate | 1u;   // back here, in Thumb
    g_cpu.R[15] = kHudButtonCb;
    runtime_dispatch_with_exchange(kHudButtonCb | 1u);
}

// TEST AID (docs/385), in the spirit of LB_CHEATS and LB_UNLOCKS: two heroes
// of one player, for Prioritize Own Hero Spells, without a hotwire drive.
// LB_TEST_ADOPT_FILE names a path. Whenever that file appears holding
// "HHLL,OWNER" -- a unit's two-byte handle (e+0xDC, e+0xDD) in hex and an
// owner -- the file is deleted and the unit given to OWNER through the game's
// own Entity::SetOwner (020598C4, ARM -- the function Hotwire calls), entered
// from the view update the way a shortcut's callback is. A poke of the owner
// byte is not the same thing: it ended the match in a "Victory!" (run t385q).
constexpr uint32_t kSetOwnerFn = 0x020598C4u;   // arm

bool test_adopt_due(uint32_t* unit, uint32_t* owner) {
    static const char* file = std::getenv("LB_TEST_ADOPT_FILE");
    static unsigned tick = 0;
    if (!file || (++tick % 30u) != 0u) return false;
    FILE* f = std::fopen(file, "rb");
    if (!f) return false;
    unsigned h = 0, o = 0;
    const int n = std::fscanf(f, "%x,%u", &h, &o);
    std::fclose(f);
    std::remove(file);
    if (n < 1) return false;
    const uint32_t handle = h & 0xFFFFu, to = n >= 2 ? o : 0u;
    const uint32_t tbl = bus_read_u32(0x02155028u);
    const uint32_t h0 = handle >> 8, h1 = handle & 0xFFu;
    uint32_t e = 0;
    if (guest_ptr(tbl) && (int32_t)h0 < (int32_t)bus_read_u32(tbl + 0xCu)) {
        e = bus_read_u32(bus_read_u32(tbl + 8u) + 4u * h0);
        if (!guest_ptr(e) || bus_read_u8(e + 0xDDu) != h1) e = 0;
    }
    std::fprintf(stderr, "[lb_test] adopt %04X for owner %u: %s\n", handle, to,
                 e ? "Entity::SetOwner" : "no such unit");
    std::fflush(stderr);
    *unit = e;
    *owner = to;
    return e != 0;
}

// The wanted item, if its list is open and shows it: its callback is entered
// (true). Otherwise it waits, or gives up.
bool quick_item_step(uint32_t screen) {
    QuickItem& q = g_qitem;
    const char* why = screen != q.screen ? "not the battle view"
                                         : quick_screen_gate(screen);
    uint32_t w = 0;
    if (!why && !quick_list_open(screen, q.group)) why = "its list closed";
    if (!why) {
        w = quick_list_item(screen, q.group, q.n);
        why = w ? quick_widget_ok(w) : "not in the list";
        if (why && --q.updates > 0) return false;   // it may show next update
    }
    const int id = w ? (int)bus_read_u16(bus_read_u32(w + 4u) + 2u) : 0;
    if (quick_log())
        std::fprintf(stderr, "[lb_quick] item %d of list %d: %s%s (widget %d)\n",
                     q.n, q.group, why ? "declined, " : "clicked",
                     why ? why : "", id);
    const int action = kQuickItem + q.n;
    q.n = 0;
    if (why) return false;
    quick_enter(w, id, screen, action);
    return true;
}
}  // namespace

void view_update_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kViewUpdate);
    if (g_qc.active) {
        if (g_cpu.R[13] == g_qc.sp_call) {
            g_qc.active = false;
            if (quick_log())
                std::fprintf(stderr, "[lb_quick] widget %d's callback "
                             "returned; the view update runs\n", g_qc.id);
            // docs/385: the list a number key opened -- its item now, with
            // the same saved registers, if the list already shows it.
            if (g_qitem.n && quick_item_step(g_qc.screen)) return;
            // The update, as it was called.
            g_cpu.R[0] = g_qc.r[0];
            g_cpu.R[1] = g_qc.r[1];
            g_cpu.R[2] = g_qc.r[2];
            g_cpu.R[3] = g_qc.r[3];
            g_cpu.R[12] = g_qc.r12;
            g_cpu.R[14] = g_qc.lr;
            g_cpu.R[13] = g_qc.sp;
            if (original) original();
            return;
        }
        // Some other entry while it runs (not seen): leave the call alone
        // unless the view it was made for is gone.
        if (bus_read_u32(kViewSlot) != g_qc.screen) g_qc.active = false;
    } else {
        int want = g_quick_req.exchange(-1);
        const uint32_t vs = g_cpu.R[0];
        // docs/398: a live number key with the blue list open picks from it.
        if (want > kQuickItem && want <= kQuickItem + 9 && !lb_demo_playing() &&
            lb_setting_quick_blue() && !quick_screen_gate(vs) &&
            quick_list_open(vs, 0))
            want = want - kQuickItem + kQuickBlueItem;
        if (want > kQuickBlueItem) {
            const int n = want - kQuickBlueItem;
            const char* why = quick_screen_gate(vs);
            if (!why && !quick_list_open(vs, 0)) why = "the blue list is closed";
            if (why) {
                if (quick_log())
                    std::fprintf(stderr, "[lb_quick] blue item %d: declined, %s\n",
                                 n, why);
            } else {
                lb_demo_note_quick(want);
                g_qitem.n = n;
                g_qitem.group = 0;
                g_qitem.screen = vs;
                g_qitem.updates = 6;
                quick_save();
                if (quick_item_step(vs)) return;
            }
        } else if (want > kQuickItem) {
            // docs/385: item N of the second brick's list.
            const uint32_t screen = g_cpu.R[0];
            uint32_t w = 0;
            int id = 0;
            const char* why = quick_gate(screen, 2, &w, &id);
            const int g = why ? -1 : quick_list_of(id);
            if (!why && g < 0) why = "no list";
            if (why) {
                if (quick_log())
                    std::fprintf(stderr, "[lb_quick] item %d: declined, %s\n",
                                 want - kQuickItem, why);
            } else {
                lb_demo_note_quick(want);
                g_qitem.n = want - kQuickItem;
                g_qitem.group = g;
                g_qitem.screen = screen;
                g_qitem.updates = 6;
                quick_save();
                if (quick_list_open(screen, g)) {
                    if (quick_item_step(screen)) return;
                } else {
                    if (quick_log())
                        std::fprintf(stderr, "[lb_quick] item %d: opening "
                                     "list %d (widget %d)\n",
                                     g_qitem.n, g, id);
                    quick_enter(w, id, screen, 2);
                    return;
                }
            }
        } else if (want >= 0) {
            const uint32_t screen = g_cpu.R[0];
            uint32_t w = 0;
            int id = 0;
            const char* why = quick_gate(screen, want, &w, &id);
            if (quick_log())
                std::fprintf(stderr, "[lb_quick] %s: %s%s (widget %d)\n",
                             want == 0 ? "Deselect"
                                       : want == 1 ? "blue brick"
                                                   : "red/yellow/white brick",
                             why ? "declined, " : "clicked",
                             why ? why : "", id);
            if (!why) {
                lb_demo_note_quick(want);
                g_qitem.n = 0;   // a brick or Deselect overrides a waiting item
                quick_save();
                quick_enter(w, id, screen, want);
                return;
            }
        } else if (g_qitem.n) {
            // docs/385: still waiting for the item's list to show it.
            quick_save();
            if (quick_item_step(g_cpu.R[0])) return;
        } else {
            uint32_t unit = 0, owner = 0;
            if (test_adopt_due(&unit, &owner)) {
                // Entity::SetOwner(unit, &owner byte), returning here.
                quick_save();
                const uint32_t arg = (g_qc.sp - 0x40u) & ~7u;
                bus_write_u8(arg, (uint8_t)owner);
                g_qc.sp_call = arg - 8u;
                g_qc.screen = g_cpu.R[0];
                g_qc.action = -1;
                g_qc.id = -1;
                g_qc.active = true;
                g_cpu.R[0] = unit;
                g_cpu.R[1] = arg;
                g_cpu.R[13] = g_qc.sp_call;
                g_cpu.R[14] = kViewUpdate | 1u;
                g_cpu.R[15] = kSetOwnerFn;
                runtime_dispatch_with_exchange(kSetOwnerFn);
                return;
            }
        }
    }
    if (original) original();
}

// ---- docs/385: PRIORITIZE OWN HERO SPELLS -----------------------------------
//
// The spells in the menu varied when several heroes were selected -- e.g. the
// Space Criminal Leader and a hotwired Wizard -- seemingly with the heroes'
// positions when the drag passed over them.
//
// WHY IT VARIES. FUN_020DE000 picks the unit that drives the HUD from the
// selection (the list at 02158024: +4 handles, two bytes each, +8 count). For
// each selected unit that FUN_0208727C resolves, that FUN_0205A118(e, 0xB)
// says is not in state 0xB, and whose owner byte (e+0xE4) FUN_020875F0 and
// FUN_02085D58 accept as the local player's, it ranks the blueprint's category
// (bp+0x5C) -- builder 1, hero 2, some buildings 3 and 4, the base 5, all
// else 0 -- and keeps the FIRST unit of the highest rank. Every hero is rank
// 2, so of two heroes the one earlier in the selection wins, and a drag box
// orders the selection by where the units stand. The winner's handle
// (e+0xDC/+0xDD) goes to 02157599, and a hero (category 0) goes to
// FUN_020DC004 (T; r0 the view, r1 the unit; its one caller is the BL at
// 020DE1B6), which fills the yellow list with that hero's spells.
//
// THE CHANGE, in front of FUN_020DC004: if the unit is a hero that is not this
// player's own -- the army record's hero, 02154E40+8, the Customize Army pick
// that Free Play fields -- and the selection also holds the own hero passing
// the same tests, that hero drives instead: its handle to 02157599, r1 to it.
// The tests, read rather than called (no nested guest calls):
//   * FUN_0208727C: the handle resolves (FUN_0205C9B4: the unit table at
//     [02155028], [+8][h0] with +0xDD == h1, h0 < [+0xC]); the unit's
//     [[e+0xE0]+4]+8 byte is 0; e+0x227 is set; and e+0x114 resolves to no
//     unit (FUN_0205B814).
//   * FUN_0205A118(e, 0xB): neither [e+0xF4]+4 nor [e+0xF8]+4 is 0xB.
//   * The owner tests read only the byte at e+0xE4 (FUN_02086CA4, FUN_02085D58),
//     so the own hero is taken only with the same byte as the unit that won,
//     which passed them.
//   * Its category is 0, the winner's, so the rank is the same.
// Only the choice among heroes the game itself would accept changes. It is a
// client-side pick -- whose spell a press arms -- and recordings carry it
// (ownhero=).
constexpr uint32_t kHeroHud = 0x020DC004u;       // thumb
constexpr uint32_t kHudSelection = 0x02158024u;
constexpr uint32_t kHudDriver = 0x02157599u;
constexpr uint32_t kUnitTable = 0x02155028u;

extern "C" int lb_demo_play_ownhero(void);   /* src/lb_demo.cpp */

namespace {
uint32_t unit_resolve(uint32_t handle_addr) {
    const uint32_t tbl = bus_read_u32(kUnitTable);
    if (!guest_ptr(tbl)) return 0;
    const uint32_t h0 = bus_read_u8(handle_addr);
    const uint32_t h1 = bus_read_u8(handle_addr + 1u);
    if ((int32_t)h0 >= (int32_t)bus_read_u32(tbl + 0xCu)) return 0;
    const uint32_t arr = bus_read_u32(tbl + 8u);
    if (!guest_ptr(arr)) return 0;
    const uint32_t e = bus_read_u32(arr + 4u * h0);
    if (!guest_ptr(e) || bus_read_u8(e + 0xDDu) != h1) return 0;
    return e;
}

uint32_t unit_blueprint(uint32_t e) {
    const uint32_t o = bus_read_u32(e + 0x160u);
    if (!guest_ptr(o)) return 0;
    const uint32_t bp = bus_read_u32(o + 8u);
    return guest_ptr(bp) ? bp : 0;
}

// FUN_0208727C and FUN_0205A118(e, 0xB), as the pick applies them.
bool hero_pick_eligible(uint32_t e) {
    const uint32_t a = bus_read_u32(e + 0xE0u);
    if (!guest_ptr(a)) return false;
    const uint32_t b = bus_read_u32(a + 4u);
    if (!guest_ptr(b) || bus_read_u8(b + 8u) != 0) return false;
    if (bus_read_u8(e + 0x227u) == 0) return false;
    if (unit_resolve(e + 0x114u) != 0) return false;
    for (const uint32_t off : {0xF4u, 0xF8u}) {
        const uint32_t s = bus_read_u32(e + off);
        if (guest_ptr(s) && bus_read_u32(s + 4u) == 0xBu) return false;
    }
    return true;
}

bool hero_log() {
    static const bool on = std::getenv("LB_HEROLOG") != nullptr;
    return on;
}
}  // namespace

void hero_hud_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kHeroHud);
    const int played = lb_demo_play_ownhero();
    const bool on = played >= 0 ? played == 1 : lb_setting_own_hero_spells() != 0;
    const uint32_t e = g_cpu.R[1];
    const uint32_t bp = guest_ptr(e) ? unit_blueprint(e) : 0;
    if (on && bp && bus_read_u8(bp + 0x5Cu) == 0) {
        const uint32_t own = bus_read_u16(0x02154E40u + 8u);
        const uint32_t idx = bus_read_u16(bp + 4u);
        if (idx != own) {
            const uint32_t handles = bus_read_u32(kHudSelection + 4u);
            const uint32_t n = bus_read_u32(kHudSelection + 8u);
            uint32_t pick = 0;
            for (uint32_t i = 0; guest_ptr(handles) && i < n && i < 64u; ++i) {
                const uint32_t c = unit_resolve(handles + 2u * i);
                if (!c || c == e) continue;
                const uint32_t cbp = unit_blueprint(c);
                if (!cbp || bus_read_u8(cbp + 0x5Cu) != 0) continue;
                if (bus_read_u16(cbp + 4u) != own) continue;
                if (bus_read_u8(c + 0xE4u) != bus_read_u8(e + 0xE4u)) continue;
                if (!hero_pick_eligible(c)) continue;
                pick = c;
                break;
            }
            if (pick) {
                bus_write_u8(kHudDriver, bus_read_u8(pick + 0xDCu));
                bus_write_u8(kHudDriver + 1u, bus_read_u8(pick + 0xDDu));
                g_cpu.R[1] = pick;
            }
            if (hero_log())
                std::fprintf(stderr, "[lb_hero] the HUD's hero: the game "
                             "picked blueprint %u; own hero %u %s\n",
                             idx, own,
                             pick ? "selected too: it drives the HUD"
                                  : "not selected (or not eligible)");
        }
    }
    if (original) original();
}

// ---- docs/384: CUSTOM AND RANDOM OPPONENTS, AND BANS ------------------------
//
// Free Play's opponent is one button on OpponentSetup whose frame index is the
// choice: a u16 at [[this+0x60]+4]+0x24, 0x38..0x3D for King, Wizard, Pirate,
// Imperial, Earth, Alien. FUN_020C0E7C (T; r0 this, r1 delta) cycles it mod 6
// and sets the redraw bit (rec[0] |= 4); FUN_020C0374 (T; one caller, the Start
// handler at 020C0F6E) maps it to faction 124..129.
//
// The game half-built a custom opponent: its choice table has "Custom Army
// 1/2/3" (factions 132..134) cut to six, and the Start handler FUN_020C0EB0
// already builds a custom opponent -- it copies army record (id - 132) onto its
// stack (sp+0x18) and calls the roster builder FUN_02000E78 (A) with it
// (BLX at 020C0FAA, lr 020C0FAE|1). Records 1 and 2 do not exist -- they alias
// profile fields the save writes -- so nothing is written there: the army is
// put into that STACK COPY, in a pre-hook on FUN_02000E78 that acts only for
// that one call.
//
// Bans ride on the same builder: FUN_020F5168's multiplayer start rebuilds
// every player's roster, on every machine, from a 20-byte record on its stack
// (BL at 020F5428, lr 020F542C). The pre-hook checks that record against the
// HOST's ban list (lb_teams_enforce_bans) and, if a pick is banned, makes it
// King's default army before the builder reads it -- changed to King once the
// game starts, the same decision on every machine.
constexpr uint32_t kRosterBuild = 0x02000E78u;          // arm
constexpr uint32_t kRosterFromEnemy = 0x020C0FAEu;      // lr & ~1
constexpr uint32_t kRosterFromMatch = 0x020F542Cu;      // lr
constexpr uint32_t kEnemyMap = 0x020C0374u;             // thumb
constexpr uint32_t kEnemyCycle = 0x020C0E7Cu;           // thumb

extern "C" int lb_demo_play_enemy8(void);                        /* lb_demo.cpp */
extern "C" void lb_demo_note_army(const unsigned char rec[20]);
extern "C" int lb_demo_take_army(unsigned char rec[20]);

namespace {
unsigned char g_enemy_rec[20];
bool g_enemy_pending = false;

// The eight-way selector is live in a session and in a recording that says
// `enemy8=1`; an older recording cycles mod 6, as it was recorded.
bool enemy8_live() { return lb_demo_play_enemy8() != 0; }

uint32_t enemy_button_rec(uint32_t self) {
    const uint32_t w = bus_read_u32(self + 0x60u);
    if (!guest_ptr(w)) return 0u;
    const uint32_t rec = bus_read_u32(w + 4u);
    return guest_ptr(rec) ? rec : 0u;
}
}  // namespace

void enemy_set_choice(uint32_t rec, int next);   // docs/392, below
void opponent_open_watch(uint32_t rec, int choice);   // docs/393, below

void enemy_cycle_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kEnemyCycle);
    const uint32_t rec = enemy8_live() ? enemy_button_rec(g_cpu.R[0]) : 0u;
    if (!rec) {
        if (original) original();
        return;
    }
    int choice = lb_teams_enemy_choice((int)bus_read_u16(rec + 0x24u));
    if (choice < 0) choice = 0;
    int next = (choice + (int32_t)g_cpu.R[1]) % 8;
    if (next < 0) next += 8;
    enemy_set_choice(rec, next);
    if (quick_log())
        std::fprintf(stderr, "[lb_enemy] selector %d -> %d\n", choice, next);
    lb_override_return();
}

// docs/392: THE OPENING CHOICE. OpponentSetup::v08 (0x020C0764) builds the
// screen's widgets on its layout, whose portrait record holds frame 0x39,
// the Wizard (UI/FE/OpponentSetup.bin record 4, +0x24); nothing else sets
// it, and the frame is the whole of the choice (FUN_020C0374 reads only
// it). v08's last call, FUN_020C03B4 at 0x020C093A, comes after every widget
// pointer is stored, so a pre-hook there sees [[screen+0x60]+4], and setting
// the frame gives exactly the state the arrows would reach. The other caller
// (0x020C010C, the opener reusing a live screen) is left alone: there the
// pointers are the old layout's, and the game keeps the last pick.
constexpr uint32_t kOppSetupLabel = 0x020C03B4u;    // thumb
constexpr uint32_t kOppSetupFromV08 = 0x020C093Fu;  // its lr from v08
extern "C" int lb_demo_play_default_opponent(void);   /* lb_demo.cpp */
void opponent_setup_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kOppSetupLabel);
    if (g_cpu.R[14] == kOppSetupFromV08) {
        const int played = lb_demo_play_default_opponent();
        // A replay of a recording before sets=2 keeps the cartridge's Wizard.
        const int want = lb_demo_playing()
                             ? (played >= 0 ? played : 1)
                             : lb_setting_default_opponent();
        const uint32_t rec = enemy_button_rec(g_cpu.R[0]);
        if (rec && want != 1 && want >= 0 && want < 8 &&
            (want < 6 || enemy8_live()) &&
            bus_read_u16(rec + 0x24u) == 0x39u) {
            enemy_set_choice(rec, want);
            if (want >= 6) opponent_open_watch(rec, want);   // docs/393
            std::fprintf(stderr, "[lb_enemy] opponent screen opens on choice "
                         "%d (frame 0x%02X), not the Wizard\n", want,
                         (unsigned)bus_read_u16(rec + 0x24u));
            std::fflush(stderr);
        }
    }
    if (original) original();
}

// docs/392: put choice `next` (0..7) on the portrait button record `rec`.
void enemy_set_choice(uint32_t rec, int next) {
    // THE TWO NEW CHOICES ARE 24x24 FRAMES IN A 56x56 BUTTON, and a button
    // draws its frame at its rect's corner (run t384e: the crown sat in the
    // portrait's top-left). The rect is this record's s16 y0,x0,y1,x1 at +4
    // (docs/371), so for choices 6 and 7 it is moved to the centred 24x24,
    // and put back exactly when a faction's portrait returns. The button
    // stays tappable (its centre), and the arrows are other widgets.
    {
        // Keyed on the rect itself, not a flag, because re-entering the
        // screen reloads the layout at full size.
        static uint16_t full[4] = {0, 0, 0, 0};
        const int width = (int16_t)bus_read_u16(rec + 0xAu) -
                          (int16_t)bus_read_u16(rec + 6u);
        const bool shrunk = width == 24 && full[3] != 0;
        // docs/390: only the 24x24 fallback frames need it; the portraits
        // are 56x56 like the factions'.
        const bool small =
            lb_teams_enemy_frame_small(lb_teams_enemy_frame(next)) != 0;
        if (small && width != 24) {
            for (int i = 0; i < 4; ++i)
                full[i] = (uint16_t)bus_read_u16(rec + 4u + 2u * (uint32_t)i);
            const int y0 = (int16_t)full[0], x0 = (int16_t)full[1];
            const int y1 = (int16_t)full[2], x1 = (int16_t)full[3];
            const int cy = y0 + ((y1 - y0) - 24) / 2, cx = x0 + ((x1 - x0) - 24) / 2;
            bus_write_u16(rec + 4u, (uint16_t)cy);
            bus_write_u16(rec + 6u, (uint16_t)cx);
            bus_write_u16(rec + 8u, (uint16_t)(cy + 24));
            bus_write_u16(rec + 0xAu, (uint16_t)(cx + 24));
        } else if (!small && shrunk) {
            for (int i = 0; i < 4; ++i)
                bus_write_u16(rec + 4u + 2u * (uint32_t)i, full[i]);
        }
    }
    bus_write_u16(rec + 0x24u, (uint16_t)lb_teams_enemy_frame(next));
    bus_write_u8(rec, (uint8_t)(bus_read_u8(rec) | 4u));   // redraw
}

// docs/393: THE OPENING CUSTOM OR RANDOMIZER WAS THE 24x24 FRAME. With the
// default freeplay opponent set to Random, the screen still opened on the old
// smaller tile instead of the LEGO shop icon. The portraits (docs/390) are
// used only once the faction atlas has been pasted, and the opponent screen
// uploads that atlas AFTER OpponentSetup::v08 finishes -- run t392n logged
// "opens on choice" before "portraits pasted" -- so the opening choice got
// the fallback, and nothing put the portrait on afterwards (the arrows do,
// which is why only the opening was affected). So the opening record is
// watched, and given the portrait the first frame one is available. It lets
// go when the player picks something else, when the portrait is already up,
// or after ten seconds.
namespace {
uint32_t g_opp_open_rec = 0u;
int g_opp_open_choice = -1;
int g_opp_open_frames = 0;
}  // namespace

void opponent_open_watch(uint32_t rec, int choice) {
    g_opp_open_rec = rec;
    g_opp_open_choice = choice;
    g_opp_open_frames = 0;
}

void opponent_open_portrait_tick(void) {
    if (!g_opp_open_rec) return;
    const uint32_t rec = g_opp_open_rec;
    const int frame = (int)bus_read_u16(rec + 0x24u);
    // Still this screen's record, still the opening choice, still small: the
    // centred 24x24 rect enemy_set_choice gave it.
    const int width = (int16_t)bus_read_u16(rec + 0xAu) -
                      (int16_t)bus_read_u16(rec + 6u);
    if (++g_opp_open_frames > 600 || !guest_ptr(rec) ||
        lb_teams_enemy_choice(frame) != g_opp_open_choice ||
        !lb_teams_enemy_frame_small(frame) || width != 24) {
        g_opp_open_rec = 0u;
        return;
    }
    if (lb_teams_enemy_frame_small(lb_teams_enemy_frame(g_opp_open_choice)))
        return;   // the portraits are not in place yet
    enemy_set_choice(rec, g_opp_open_choice);
    std::fprintf(stderr, "[lb_enemy] opening choice %d now drawn as its "
                 "portrait (frame 0x%02X)\n", g_opp_open_choice,
                 (unsigned)bus_read_u16(rec + 0x24u));
    std::fflush(stderr);
    g_opp_open_rec = 0u;
}

void enemy_map_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kEnemyMap);
    const uint32_t rec = enemy8_live() ? enemy_button_rec(g_cpu.R[0]) : 0u;
    const int choice =
        rec ? lb_teams_enemy_choice((int)bus_read_u16(rec + 0x24u)) : -1;
    if (choice != 6 && choice != 7) {
        if (original) original();
        return;
    }
    // A replay fields the army its recording fielded; a session draws it
    // (Random) or takes the Team Builder's (Custom) and records it.
    if (!(lb_demo_playing() && lb_demo_take_army(g_enemy_rec))) {
        lb_teams_enemy_army(choice, g_enemy_rec);
        lb_demo_note_army(g_enemy_rec);
    }
    g_enemy_pending = true;
    std::fprintf(stderr, "[lb_enemy] %s opponent: hero %u builder %u (base "
                 "faction %u)\n", choice == 6 ? "Custom" : "Random",
                 (unsigned)(g_enemy_rec[8] | (g_enemy_rec[9] << 8)),
                 (unsigned)(g_enemy_rec[10] | (g_enemy_rec[11] << 8)),
                 (unsigned)g_enemy_rec[0x13]);
    std::fflush(stderr);
    g_cpu.R[0] = 0x85u;   // "Custom Army 2", the slot the game left unused
    lb_override_return();
}

void roster_build_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kRosterBuild);
    const uint32_t lr = g_cpu.R[14] & ~1u;
    // Both records are on their caller's STACK (sp+0x18, sp+0x24), and the
    // stack is in DTCM, not main RAM, so guest_ptr() is the wrong test: the
    // first build refused every one (run t384c). The bus maps DTCM; the lr
    // test is what says the pointer is the record.
    const uint32_t rec = g_cpu.R[1];
    if (lr == kRosterFromEnemy && g_enemy_pending && rec != 0u) {
        for (uint32_t i = 0; i < 20u; ++i)
            bus_write_u8(rec + i, g_enemy_rec[i]);
        g_enemy_pending = false;
        std::fprintf(stderr, "[lb_enemy] army written for faction id %u "
                     "(object %08X; its roster is +0x38)\n",
                     (unsigned)bus_read_u8(g_cpu.R[0] + 0x30u),
                     (unsigned)g_cpu.R[0]);
        std::fflush(stderr);
    } else if (lr == kRosterFromMatch && rec != 0u) {
        unsigned char r[20];
        for (uint32_t i = 0; i < 20u; ++i) r[i] = (unsigned char)bus_read_u8(rec + i);
        char why[192];
        if (lb_teams_enforce_bans(r, why, (int)sizeof why)) {
            for (uint32_t i = 0; i < 20u; ++i) bus_write_u8(rec + i, r[i]);
            std::fprintf(stderr, "[lb_bans] player %d switched to King at the "
                         "start: banned %s\n",
                         (int)bus_read_u8(g_cpu.R[0] + 0x30u) - 0x84, why);
            std::fflush(stderr);
        }
    }
    if (original) original();
}

// docs/385: A CUSTOM OR RANDOM OPPONENT IS NAMED AFTER ITS HERO. In a
// randomizer match the bot's team string follows not its builder's team
// (e.g. Earth, Wizard) but its hero's name (e.g. Alien Queen, Gemma, Ghost,
// King).
//
// The Start handler names the opponent right after building its roster:
//
//   020C0FAE  adds r4,#0x31 ; ldrb r0,[r4]   the new faction's base (+0x31)
//   020C0FB2  blx  FUN_020029DC              that base's faction
//   020C0FB8  ldr  r1,[r0,#0x28] ; ldrh r1,[r1,#6]   its name id
//   020C0FBA  add  r0,sp,#0x2C
//   020C0FBE  blx  FUN_0203E1D4              String(sp+0x2C, id)   lr 020C0FC3
//
// and FUN_02045858(setup, 1, sp+0x2C) makes that the opponent's name. The
// base is the builder's faction (docs/384), hence "Earth". This branch is the
// custom one (FUN_0200239C: factions 132..134), which only this port's Custom
// and Random reach, so at that return address the id is swapped for the
// hero's: blueprint +0x06 of the hero in the army record the roster was just
// built from -- the caller's sp+0x18, hero at +8 (the stack is in DTCM, so it
// is found by lr, not by range). FUN_0203E1D4 is ARM and a plain constructor:
// every other call is a pure tail call.
//
// FOUR NAMES ARE SHORTENED. The score box fits about 17 characters, and on the
// Victory screen "Space Criminal Leader" ran ~35 px past its frame (run
// t385r). All four long names are shortened -- Brickbeard, Broadside,
// Space Police, Space Criminal:
//   * Space Police Captain (129) and Space Criminal Leader (133) take the
//     game's own strings 553 and 557, "Space Police" and "Space Criminal"
//     (the next ids after the heroes'). While the Enhancements tab renames the
//     leader (docs/356 rewrites 556 to Kranxx or Plisken), 556 is kept: it is
//     short, and it is what the player chose to call him.
//   * Captain Brickbeard (40) and Governor Broadside (60) have no string of
//     their own, so the word after the last space of the game's own name is
//     used: FUN_0203E1D4 is `FUN_020F1C88(dest, table[id])`, a copy from a C
//     string, so this calls FUN_020F1C88 with a pointer into the name,
//     returning where FUN_0203E1D4 would have. It works in any language.
// Recordings: oppname=1 is the full hero name (the first docs/385 build),
// oppname=2 this; absent, the base's name.
constexpr uint32_t kStringFromId = 0x0203E1D4u;   // arm
constexpr uint32_t kNameFromEnemy = 0x020C0FC3u;  // lr: Thumb, after the BLX
constexpr uint32_t kStringAssign = 0x020F1C88u;   // arm: (String*, const char*)

extern "C" int lb_demo_play_oppname(void);   /* src/lb_demo.cpp */

// String `id`'s text, looked up as FUN_0203E0FC does -- [[[0215378C+0xC]+8]
// + id*4] -- which is the lookup FUN_0203E1D4 is about to make anyway.
uint32_t loc_text_addr(uint32_t id) {
    const uint32_t file = bus_read_u32(0x0215378Cu + 0x0Cu);
    if (!guest_ptr(file)) return 0u;
    const uint32_t table = bus_read_u32(file + 8u);
    if (!guest_ptr(table)) return 0u;
    const uint32_t at = bus_read_u32(table + 4u * id);
    return guest_ptr(at) ? at : 0u;
}

// ---- docs/396: TYPING ON THE GAME'S OWN KEYBOARDS ---------------------------
//
// The profile-name keyboard (User::CreateNewProfile) and the cheat-code one
// (User::CheatKeyboardScreen) are two copies of one design, read from their
// virtual_08 (020B6D4C, 020B4B14) and their button callbacks (020B71A4,
// 020B4FDC, Thumb):
//   * 29 key widgets, ids 0x23D..0x259, kept at screen + id*4 - 0x89C; the
//     text being typed is a String at screen+0x44, shown by the widget at
//     +0x50; +0x54 is the OK button (id 0x23C); 0x147 is Back.
//   * every key has the same callback, stored in W+8 for button widgets,
//     which (on a click) appends the key's label, or for 0x258 erases one,
//     0x250 toggles the case (name keyboard), 0x259 adds a space. At most 10
//     characters for a name, 7 for a code.
//   * a key's label is a LOC string id from an 8-byte-per-key table:
//     02128220/0212821C (name, +0x40 clear/set), 02128138/02128134 (code).
// So a typed character becomes the key whose label is that character, and
// the key is pressed by running its own callback with the PenEvent of a
// click -- the docs/383 mechanism, in front of these screens' update
// (ScreenType::virtual_09, 020E1BF4, which ~30 screens share and which only
// their vtables reach), as a tail call that returns into it. The game does
// the typing, the limits, the case and the sound exactly as for a tap.
constexpr uint32_t kScreenUpdate = 0x020E1BF4u;   // thumb

namespace {
struct KeyboardKind {
    uint32_t vtable, callback, table_lower, table_upper;
};
const KeyboardKind kKeyboards[2] = {
    {0x0214C328u, 0x020B4FDCu, 0x02128138u, 0x02128134u},   // cheat codes
    {0x0214C4F8u, 0x020B71A4u, 0x02128220u, 0x0212821Cu},   // profile name
};

// Typed keys, the frontend's thread to the guest's update: printable ASCII,
// '\b' erase, '\r' OK. The two run on one thread today; the ring keeps it
// correct if that changes.
std::atomic<unsigned> g_kb_head{0}, g_kb_tail{0};
char g_kb_ring[64];
std::atomic<unsigned> g_kb_seen{0};   // VBlank count of the last keyboard update
unsigned g_kb_vblanks = 0;

struct KeyCall {
    bool active = false;
    uint32_t r[4] = {0, 0, 0, 0};
    uint32_t r12 = 0, lr = 0, sp = 0, sp_call = 0, screen = 0;
};
KeyCall g_kc;
}  // namespace

// docs/397: A RENAME IS SAVED BY THE GAME ITSELF. The rename (docs/396) only
// changed the name in RAM, and the game writes it at its next save -- which a
// player who renames and quits never reaches, so the save kept the old name.
// The record is bit-packed (SaveGame writes field after field through
// FUN_0204BA48/FUN_0204BAB0) and its checksum is unknown (docs/97, docs/176),
// so the port does not write the file. It runs the game's own save instead:
// FUN_0204FA6C(0), the very call User::MainMenu's virtual_12 makes after the
// 12/25 unlock (ARM: SaveGame FUN_0204FA88 with R0 passed through, then
// FUN_0204B538(0)).
//
// It runs on the MAIN MENU only, in front of User::MainMenu::virtual_09
// (020BF230, vtable 0214D00C, reached only through the vtable), as a tail
// call that returns there -- the docs/383 mechanism. The main menu is where a
// profile is certainly loaded and the game itself saves; the shared menu
// update (020E1BF4) would not do, because MainMenu's own update calls it
// directly, past any override. A rename asked for anywhere else waits there.
constexpr uint32_t kMainMenuUpdate = 0x020BF230u;   // thumb
constexpr uint32_t kMainMenuVtable = 0x0214D00Cu;
constexpr uint32_t kSaveProfile = 0x0204FA6Cu;      // ARM
static bool g_profile_save_pending = false;
static KeyCall g_save_call;

extern "C" void lb_request_profile_rename(const char* name);
void main_menu_update_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kMainMenuUpdate);
    // TEST AID: LB_TEST_RENAME=<name> asks for the rename once, the first
    // time the main menu is up, as the menu's button does.
    static bool test_renamed = false;
    if (!test_renamed) {
        test_renamed = true;
        if (const char* s = std::getenv("LB_TEST_RENAME")) lb_request_profile_rename(s);
    }
    if (g_save_call.active && g_cpu.R[13] == g_save_call.sp_call) {
        g_save_call.active = false;   // the save returned here
        std::fprintf(stderr, "[lb_name] the game saved the profile (result %u)\n",
                     (unsigned)g_cpu.R[0]);
        g_cpu.R[0] = g_save_call.r[0];
        g_cpu.R[1] = g_save_call.r[1];
        g_cpu.R[2] = g_save_call.r[2];
        g_cpu.R[3] = g_save_call.r[3];
        g_cpu.R[12] = g_save_call.r12;
        g_cpu.R[14] = g_save_call.lr;
        g_cpu.R[13] = g_save_call.sp;
        if (original) original();
        return;
    }
    const uint32_t net = bus_read_u32(0x0215B7BCu);   // 0 or -1: no session
    if (g_profile_save_pending && !g_save_call.active &&
        guest_ptr(g_cpu.R[0]) && bus_read_u32(g_cpu.R[0]) == kMainMenuVtable &&
        (net == 0u || net == 0xFFFFFFFFu)) {
        g_profile_save_pending = false;
        g_save_call.r[0] = g_cpu.R[0];
        g_save_call.r[1] = g_cpu.R[1];
        g_save_call.r[2] = g_cpu.R[2];
        g_save_call.r[3] = g_cpu.R[3];
        g_save_call.r12 = g_cpu.R[12];
        g_save_call.lr = g_cpu.R[14];
        g_save_call.sp = g_cpu.R[13];
        g_save_call.sp_call = (g_save_call.sp - 0x40u) & ~7u;
        g_save_call.active = true;
        g_cpu.R[0] = 0u;
        g_cpu.R[13] = g_save_call.sp_call;
        g_cpu.R[14] = kMainMenuUpdate | 1u;   // back here, in Thumb
        g_cpu.R[15] = kSaveProfile;
        runtime_dispatch_with_exchange(kSaveProfile);
        return;
    }
    if (original) original();
}

namespace {

const KeyboardKind* keyboard_of(uint32_t screen) {
    if (!guest_ptr(screen)) return nullptr;
    const uint32_t vt = bus_read_u32(screen);
    for (const KeyboardKind& k : kKeyboards)
        if (vt == k.vtable) return &k;
    return nullptr;
}

// The shown key widget for a typed character, or 0.
uint32_t keyboard_key_for(uint32_t screen, const KeyboardKind& k, char c) {
    auto shown = [&](uint32_t w) {
        if (!guest_ptr(w)) return false;
        const uint32_t rec = bus_read_u32(w + 4u);
        return guest_ptr(rec) && !(bus_read_u8(rec) & 2u);
    };
    if (c == '\r') {
        const uint32_t w = bus_read_u32(screen + 0x54u);
        return shown(w) ? w : 0u;
    }
    const int want = c == '\b' ? 0x258 : c == ' ' ? 0x259 : -1;
    const bool upper = bus_read_u8(screen + 0x40u) != 0;
    for (int id = 0x23D; id <= 0x259; ++id) {
        const uint32_t w = bus_read_u32(screen + (uint32_t)id * 4u - 0x89Cu);
        if (!shown(w)) continue;
        if (want >= 0) {
            if (id == want) return w;
            continue;
        }
        if (id == 0x250 || id >= 0x258) continue;   // case, erase, space
        const uint32_t loc = bus_read_u32((upper ? k.table_upper : k.table_lower) +
                                          (uint32_t)(id - 0x23D) * 8u);
        const uint32_t text = loc_text_addr(loc);
        if (!text) continue;
        const int ch = bus_read_u8(text);
        if (ch && bus_read_u8(text + 1u) == 0 &&
            std::tolower(ch) == std::tolower((unsigned char)c))
            return w;
    }
    return 0u;
}
}  // namespace

extern "C" void lb_quick_replay(int action);
// The frontend's request. A replay owns the input; a recording keeps it.
extern "C" void lb_keyboard_type(int ch) {
    if (lb_demo_playing() && ch < 0x10000) return;
    ch &= 0xFF;
    const unsigned h = g_kb_head.load();
    if (h - g_kb_tail.load() >= sizeof g_kb_ring) return;
    g_kb_ring[h % sizeof g_kb_ring] = (char)ch;
    g_kb_head.store(h + 1u);
}
// True while one of the two keyboards is the screen being updated.
extern "C" int lb_keyboard_screen_active(void) {
    return g_kb_vblanks - g_kb_seen.load() < 10u ? 1 : 0;
}

void screen_update_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kScreenUpdate);
    ++g_kb_vblanks;   // (counts updates; only the difference matters)
    if (g_kc.active) {
        if (g_cpu.R[13] == g_kc.sp_call) {   // the key's callback returned
            g_kc.active = false;
            g_cpu.R[0] = g_kc.r[0];
            g_cpu.R[1] = g_kc.r[1];
            g_cpu.R[2] = g_kc.r[2];
            g_cpu.R[3] = g_kc.r[3];
            g_cpu.R[12] = g_kc.r12;
            g_cpu.R[14] = g_kc.lr;
            g_cpu.R[13] = g_kc.sp;
            if (original) original();
            return;
        }
        if (!keyboard_of(g_kc.screen)) g_kc.active = false;
    } else if (const KeyboardKind* k = keyboard_of(g_cpu.R[0])) {
        g_kb_seen.store(g_kb_vblanks);
        // TEST AID: LB_TEST_TYPE=<text> types it once, the first time a
        // keyboard is up ('\' then 'b' or 'r' for erase or OK), because the
        // debug server can send keys but not text.
        static bool test_typed = false;
        if (!test_typed) {
            test_typed = true;
            if (const char* s = std::getenv("LB_TEST_TYPE")) {
                for (; *s; ++s) {
                    if (s[0] == '\\' && (s[1] == 'b' || s[1] == 'r')) {
                        lb_keyboard_type(s[1] == 'b' ? '\b' : '\r');
                        ++s;
                    } else {
                        lb_keyboard_type(*s);
                    }
                }
                std::fprintf(stderr, "[lb_kb] LB_TEST_TYPE queued\n");
            }
        }
        const uint32_t screen = g_cpu.R[0];
        // The pen path's own gates (docs/383): UI input off, a button held.
        const bool gated = bus_read_u8(0x021581A5u) || bus_read_u32(0x021581ACu) == 1u;
        unsigned t = g_kb_tail.load();
        if (!gated && t != g_kb_head.load()) {
            const char c = g_kb_ring[t % sizeof g_kb_ring];
            g_kb_tail.store(t + 1u);
            const uint32_t w = keyboard_key_for(screen, *k, c);
            if (w) {
                lb_demo_note_quick(0x10000 | (unsigned char)c);
                g_kc.r[0] = g_cpu.R[0];
                g_kc.r[1] = g_cpu.R[1];
                g_kc.r[2] = g_cpu.R[2];
                g_kc.r[3] = g_cpu.R[3];
                g_kc.r12 = g_cpu.R[12];
                g_kc.lr = g_cpu.R[14];
                g_kc.sp = g_cpu.R[13];
                // The click's PenEvent, as quick_enter builds it.
                const uint32_t ev = (g_kc.sp - 0x40u) & ~7u;
                const uint32_t rec = bus_read_u32(w + 4u);
                const int cx = ((int16_t)bus_read_u16(rec + 6u) +
                                (int16_t)bus_read_u16(rec + 0xAu)) / 2;
                const int cy = ((int16_t)bus_read_u16(rec + 4u) +
                                (int16_t)bus_read_u16(rec + 8u)) / 2;
                bus_write_u32(ev + 0x00u, kPenEventVtable);
                bus_write_u32(ev + 0x04u, 0u);
                bus_write_u32(ev + 0x08u, 0u);
                bus_write_u32(ev + 0x0Cu, 1u);
                bus_write_u32(ev + 0x10u, 0u);
                bus_write_u32(ev + 0x14u, 0u);
                bus_write_u32(ev + 0x18u, 1u);
                bus_write_u16(ev + 0x1Cu, (uint16_t)cx);
                bus_write_u16(ev + 0x1Eu, (uint16_t)cy);
                g_kc.sp_call = ev - 8u;
                g_kc.screen = screen;
                g_kc.active = true;
                g_cpu.R[0] = w;
                g_cpu.R[1] = ev;
                g_cpu.R[2] = screen;
                g_cpu.R[3] = 1u;
                g_cpu.R[13] = g_kc.sp_call;
                g_cpu.R[14] = kScreenUpdate | 1u;   // back here, in Thumb
                g_cpu.R[15] = k->callback;
                runtime_dispatch_with_exchange(k->callback | 1u);
                return;
            }
        }
    }
    if (original) original();
}

void string_from_id_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kStringFromId);
    const int played = lb_demo_play_oppname();
    const int mode = played >= 0 ? played : 2;   // 0 base, 1 full, 2 short
    if (g_cpu.R[14] == kNameFromEnemy && mode != 0) {
        const uint32_t hero = bus_read_u16(g_cpu.R[13] + 0x18u + 8u);
        int id = lb_teams_unit_name_id((int)hero);
        uint32_t text = 0;   // a pointer into a name, instead of an id
        if (id > 0 && mode == 2) {
            if (hero == 129u) {
                id = 553;                                   // Space Police
            } else if (hero == 133u) {
                if (!lb_setting_criminal_leader_name()) id = 557;   // Space Criminal
            } else if (hero == 40u || hero == 60u) {         // Brickbeard, Broadside
                const uint32_t at = loc_text_addr((uint32_t)id);
                for (uint32_t i = 0; at && i < 64u; ++i) {
                    const uint8_t c = bus_read_u8(at + i);
                    if (!c) break;
                    if (c == ' ' && bus_read_u8(at + i + 1u)) text = at + i + 1u;
                }
            }
        }
        if (id > 0) {
            char shown[64] = "";
            for (uint32_t i = 0; text && i < sizeof shown - 1; ++i) {
                shown[i] = (char)bus_read_u8(text + i);
                if (!shown[i]) break;
            }
            std::fprintf(stderr, "[lb_enemy] the opponent is named after its "
                         "hero %u (%s): %s%s (not the base's name id %u)\n",
                         (unsigned)hero, lb_teams_unit_name((int)hero),
                         text ? "the last word of its name, " : "name id ",
                         text ? shown : std::to_string(id).c_str(),
                         (unsigned)g_cpu.R[1]);
            std::fflush(stderr);
            if (text) {
                // FUN_020F1C88(dest, text), returning to FUN_0203E1D4's caller.
                g_cpu.R[1] = text;
                g_cpu.R[15] = kStringAssign;
                runtime_dispatch_with_exchange(kStringAssign);
                return;
            }
            g_cpu.R[1] = (uint32_t)id;
        }
    }
    if (original) original();
}

// ---- docs/387: FORCE TILESET, AND EACH LEVEL'S WORLD ------------------------
//
// A display option, 'Force Tileset', chooses a level type overriding the
// current one where possible [Off/Castle/Pirates/Mars/Random].
//
// HOW A MAP NAMES ITS TILESET (docs/387, verified in the disassembly). There is
// no number: the .map's TERR chunk holds the name as char[32] at file offset
// 0x0B -- "KingTileset", "PirateTileset" or "MarsTileset" in all 122 maps.
// FUN_020360B0 (load state 5) hands the decompressed buffer to the map parser
// 0x020A369C (Thumb, r0 map, r1 buffer, reached only by dispatch through the
// thunk 0x020A3210), whose LoadTerrain copies the name to map+0x18. Everything
// after reads that: BP/<World>Tiles.tbp, <World>Tileset.NCGR/.NCLR,
// FUN_020A44C0's world index (the minimap's world colours, the pickup sprites,
// the palette cycles) and lb_music's heap scan. So a pre-hook here that writes
// the chosen name into the buffer makes every one of them agree.
//
// GAMEPLAY IS THE MAP'S, NOT THE TILESET'S. Terrain classes (plane A), the
// tree/resource mask (block D), walls and bridges come from the map or fixed
// indices; the tileset holds only pixels, colours and tile entries, and
// nothing simulated reads it. The picture is coherent -- the three worlds'
// tile tables are laid out in parallel -- except a map's own detail
// compositions (BP/DetailTiles_<map>.tbp, about 8% of cells) and a few base
// entries one world leaves blank; see the detail fix below.
//
// LOAD-TIME ONLY: a change applies from the next battle. Single player only
// for now: the files' compressed sizes differ, so the heap would differ
// between machines, which a two-instance match has not yet shown harmless.
// Recordings carry the mode and the seed of Random (tileset=, pickseed=).
constexpr uint32_t kMapParse = 0x020A369Cu;   // thumb

extern "C" int lb_demo_play_tileset(void);    /* src/lb_demo.cpp */
extern "C" unsigned lb_demo_pick_seed(void);
extern "C" int lb_net_guest_state(void);      /* src/lb_net.cpp */

namespace {
const char* const kTilesetName[3] = {"KingTileset", "PirateTileset",
                                     "MarsTileset"};
int g_level_world = -1;        // 0 King, 1 Pirate, 2 Mars, as loaded
int g_level_src_world = -1;    // docs/392: the world the map itself names
unsigned g_level_ordinal = 0;  // maps parsed since boot
bool g_level_forced = false;   // this level's tileset is not its own
bool g_detail_done = false;    // its tables have been patched
uint32_t g_sent_addr[2] = {0u, 0u};   // one patched entry in A, one in B
uint8_t g_sent_bytes[2][12];
}  // namespace

// This level's world as loaded (after any forcing), -1 before the first.
extern "C" int lb_level_world(void) { return g_level_world; }

// A per-level draw for Random options: stable within a level, different for
// the next, and the same on a replay of a recording that starts at boot.
extern "C" unsigned lb_level_pick(unsigned salt) {
    uint32_t h = lb_demo_pick_seed() ^ (g_level_ordinal * 0x9E3779B9u) ^
                 (salt * 0x85EBCA6Bu);
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

void map_parse_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kMapParse);
    const uint32_t buf = g_cpu.R[1];
    if (guest_ptr(buf) && bus_read_u8(buf) == 'M' && bus_read_u8(buf + 1u) == 'A' &&
        bus_read_u8(buf + 2u) == 'P' && bus_read_u8(buf + 3u) == 'T' &&
        bus_read_u8(buf + 4u) == 'E' && bus_read_u8(buf + 5u) == 'R' &&
        bus_read_u8(buf + 6u) == 'R') {
        ++g_level_ordinal;
        g_level_forced = false;
        char name[33] = {0};
        for (uint32_t i = 0; i < 32u; ++i)
            name[i] = (char)bus_read_u8(buf + 0x0Bu + i);
        int world = -1;
        for (int w = 0; w < 3; ++w)
            if (std::strcmp(name, kTilesetName[w]) == 0) world = w;
        g_level_src_world = world;   // docs/392: the map's own, for stand-ins
        const int played = lb_demo_play_tileset();
        const int mode = played >= 0 ? played : lb_setting_force_tileset();
        const int net = lb_net_guest_state();
        if (mode > 0 && world >= 0 && (played >= 0 || net == 0 || net == -1)) {
            const int want = mode == 4 ? (int)(lb_level_pick(0x544C53u) % 3u)  // 'TLS'
                                       : mode - 1;
            if (want != world) {
                const char* to = kTilesetName[want];
                const size_t n = std::strlen(to);
                for (uint32_t i = 0; i < 32u; ++i)
                    bus_write_u8(buf + 0x0Bu + i, i < n ? (uint8_t)to[i] : 0u);
                std::fprintf(stderr, "[lb_tileset] level %u: %s forced to %s%s\n",
                             g_level_ordinal, name, to,
                             mode == 4 ? " (Random)" : "");
                std::fflush(stderr);
                world = want;
                g_level_forced = true;
                g_detail_done = false;
                g_sent_addr[0] = g_sent_addr[1] = 0u;
            }
        } else if (mode > 0 && world >= 0) {
            std::fprintf(stderr, "[lb_tileset] level %u: not forced -- "
                         "multiplayer keeps each map's own tileset\n",
                         g_level_ordinal);
            std::fflush(stderr);
        }
        g_level_world = world;
    }
    if (original) original();
}

// ---- docs/387: A FORCED TILESET'S DETAIL CELLS AND BLANK SLOTS --------------
//
// Two things draw wrong under a foreign tileset (docs/387, measured over all
// 122 maps):
//   * a map's own detail compositions, BP/DetailTiles_<map>.tbp (table B,
//     *(map+0x33C)), are tile numbers into ITS world's NCGR -- 8.3% of cells,
//     which draw as fragments and black squares under another;
//   * a few base metatiles (table A, *(map+0x338)) hold a blank tile in the
//     forced world.
// Both are pictures only: the decoder (Terrain_DecodeMetatile 0x020A3478) is
// their only reader. So, once per forced level and before the terrain is
// drawn, table A's blank slots take a fixed stand-in of the same role, and
// each table-B entry k takes a copy of the table-A metatile most common
// around the cells that use k (8 neighbours; a detail neighbour votes once
// its own index is settled; ties to the lowest index; up to 10 passes).
// Per index rather than per cell, because plane E is not ours to edit; over
// all maps it picks what a per-cell fill would for 80% of detail cells and
// looks as good or better (docs/387's renders).
//
// WHEN: a pre-hook on Terrain_FillWindow (0x020A3234, Thumb; r0 the map;
// reached only by dispatch from 0x020396D0), the only caller of the decoder.
// The tables arrive asynchronously after the parse, so the patch waits until
// table A holds its 5,120 entries; and because a reloaded table of the same
// size can land at the same address, one patched entry in each table is
// checked on every fill and the patch is redone if it no longer holds.
constexpr uint32_t kFillWindow = 0x020A3234u;   // thumb
constexpr uint32_t kDetailSplit = 0x0214B9D2u;  // u16: plane E's A/B split (440)

extern "C" void lb_widemap_terrain_changed(void);   /* src/lb_widemap.cpp */

namespace {
struct SlotStandIn { int world; uint16_t slot, with; int src = -1; };
// docs/387: the forced world, the blank slot, the stand-in of the same role
// (a plateau interior, base ground, grass).
//
// docs/392: and eight slots that are not blank but draw another role in the
// forced world, all on maps from Mars (remapped worlds showed some
// mismatched tiles, e.g. extra water, a well cap).
// A data pass over all 122 maps found every table-A
// index a world uses is at least 95% one terrain role (plane A, the tree
// mask, mines) and that the worlds' tables agree on every index two worlds
// both use; these are the ones only Mars maps use. Mars 118 (red-plate
// grass, 3,942 cells) drew Pirate flower blocks; 98, a plateau mound, drew a
// King tree piece; 160/165/170 (speckled ground) drew grass, flowers or a
// rock; 205 (ground) drew a King or Pirate CLOUD piece, so it is keyed on a
// Mars source (`src`), as are the rest. Match by role: Mars to King 98.3% ->
// 99.2%, Mars to Pirate 97.0% -> 99.2%.
constexpr SlotStandIn kStandIns[] = {
    {1, 44, 89}, {1, 128, 89}, {1, 98, 89}, {1, 165, 96}, {1, 170, 96},
    {2, 44, 89}, {2, 128, 89},
    {0, 118, 116},
    {0, 98, 89, 2}, {0, 205, 200, 2}, {0, 160, 190, 2}, {0, 165, 190, 2},
    {0, 170, 190, 2},
    {1, 118, 116, 2}, {1, 205, 200, 2}, {1, 160, 96, 2},
};
bool stand_in_applies(const SlotStandIn& s) {
    return s.world == g_level_world && (s.src < 0 || s.src == g_level_src_world);
}

uint32_t metatile_entry(uint32_t table, uint32_t i) { return table + 2u + 12u * i; }

void copy_entry(uint32_t dst, uint32_t src) {
    for (uint32_t b = 0; b < 12u; b += 2u) bus_write_u16(dst + b, bus_read_u16(src + b));
}

void keep_sentinel(int which, uint32_t addr) {
    g_sent_addr[which] = addr;
    for (uint32_t b = 0; b < 12u; ++b) g_sent_bytes[which][b] = bus_read_u8(addr + b);
}

bool sentinels_hold() {
    for (int s = 0; s < 2; ++s) {
        if (!g_sent_addr[s]) continue;
        for (uint32_t b = 0; b < 12u; ++b)
            if (bus_read_u8(g_sent_addr[s] + b) != g_sent_bytes[s][b]) return false;
    }
    return true;
}

// True when the tables were there to patch.
bool patch_forced_tables(uint32_t map) {
    const uint32_t A = bus_read_u32(map + 0x338u);
    if (!guest_ptr(A) || bus_read_u16(A) != 5120u) return false;   // not loaded yet
    const int w = bus_read_u8(map + 0x14u), h = bus_read_u8(map + 0x15u);
    const uint32_t E = bus_read_u32(map + 0x30u);
    if (w <= 0 || h <= 0 || !guest_ptr(E)) return false;
    const uint32_t split = bus_read_u16(kDetailSplit);
    if (split == 0u || split > 5120u) return false;

    auto stand_in = [&](uint32_t v) -> uint32_t {
        for (const SlotStandIn& s : kStandIns)
            if (stand_in_applies(s) && s.slot == v) return s.with;
        return v;
    };
    // Table A's blank and mismatched slots. No stand-in's `with` is another's
    // `slot`, so each copies an unpatched entry.
    int slots = 0;
    g_sent_addr[0] = g_sent_addr[1] = 0u;
    for (const SlotStandIn& s : kStandIns) {
        if (!stand_in_applies(s)) continue;
        copy_entry(metatile_entry(A, s.slot), metatile_entry(A, s.with));
        if (!slots) keep_sentinel(0, metatile_entry(A, s.slot));
        ++slots;
    }

    // Table B, per index.
    std::vector<uint16_t> e((size_t)w * (size_t)h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            e[(size_t)y * w + x] = (uint16_t)bus_read_u16(E + 2u * (uint32_t)(y * w + x));
    int max_k = -1, detail_cells = 0;
    for (uint16_t v : e)
        if (v >= split) {
            ++detail_cells;
            if ((int)(v - split) > max_k) max_k = (int)(v - split);
        }
    const uint32_t B = bus_read_u32(map + 0x33Cu);
    // docs/392: THE TERRAIN CLASS VOTES TOO. Plane A (u8[w*h] at *(map+0x2E8):
    // 0 grass, 2 ground, 3 water, 5 plateau, read by the passability code)
    // is the cell's role. The plain vote took the most common neighbour,
    // which beside a shore is often water or a cliff edge: 13.5% of King,
    // 17.9% of Pirate and 7.7% of Mars detail cells drew another role, the
    // "extra water". Now a neighbour votes only if its class is the
    // majority class of the cells using that detail index; an index none of
    // whose neighbours qualifies falls back to the plain vote. Role match
    // with the stand-ins: about 99.8% for every pair.
    //
    // AND INTERIOR NEIGHBOURS FIRST. A role match is not a picture match: a
    // grass-to-ground transition piece is class 0 too, so a detail patch in
    // the middle of grass that borders ground in a few places could take the
    // transition piece -- the stray strips and notches of runs t392t/u (Exile,
    // mp05, Mars forced to King and to Pirate). So the first tier counts only
    // table-A neighbours that are themselves INTERIOR (all 8 of their own
    // neighbours in their class); then the class tier, then every neighbour,
    // as before. An offline replica of this function (which reproduces the
    // live "203 entries for 730 cells") over all 122 maps:
    // interior detail cells drawn with an edge piece, Mars->King 6.8% -> 2.2%,
    // Mars->Pirate 6.9% -> 2.5%, King->Pirate 5.7% -> 1.7%, Pirate->King 6.1%
    // -> 1.4%, and Exile's strips gone.
    const uint32_t P = bus_read_u32(map + 0x2E8u);
    std::vector<uint8_t> cls, inner;
    if (guest_ptr(P) && guest_ptr(P + (uint32_t)(w * h) - 1u)) {
        cls.resize((size_t)w * (size_t)h);
        for (size_t i = 0; i < cls.size(); ++i)
            cls[i] = bus_read_u8(P + (uint32_t)i);
        inner.assign(cls.size(), 1u);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t c = cls[(size_t)y * w + x];
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= w || ny >= h)
                            continue;
                        if (cls[(size_t)ny * w + nx] != c)
                            inner[(size_t)y * w + x] = 0u;
                    }
            }
    }
    int filled = 0;
    if (max_k >= 0 && guest_ptr(B) && (int)bus_read_u16(B) > max_k) {
        // Each detail index's majority class.
        std::vector<int> want_cls((size_t)max_k + 1u, -1);
        if (!cls.empty()) {
            std::vector<std::array<int, 256>> hist((size_t)max_k + 1u);
            for (auto& hst : hist) hst.fill(0);
            for (size_t i = 0; i < e.size(); ++i)
                if (e[i] >= split) ++hist[e[i] - split][cls[i]];
            for (int k = 0; k <= max_k; ++k) {
                int best = -1, best_n = 0;
                for (int c = 0; c < 256; ++c)
                    if (hist[(size_t)k][(size_t)c] > best_n) {
                        best_n = hist[(size_t)k][(size_t)c];
                        best = c;
                    }
                want_cls[(size_t)k] = best;
            }
        }
        std::vector<int> choice((size_t)max_k + 1u, -1);
        for (int pass = 0; pass < 30; ++pass) {
            // Passes 0-9 count only interior table-A neighbours of the
            // index's class, 10-19 every neighbour of its class, and 20-29
            // settle what is left with every neighbour.
            const bool by_inner = pass < 10 && !cls.empty();
            const bool by_class = pass < 20 && !cls.empty();
            std::vector<std::vector<uint16_t>> votes((size_t)max_k + 1u);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const uint16_t v = e[(size_t)y * w + x];
                    if (v < split || choice[v - split] >= 0) continue;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy) continue;
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            const size_t ni = (size_t)ny * w + nx;
                            if (by_class && (int)cls[ni] != want_cls[v - split])
                                continue;
                            const uint16_t n = e[ni];
                            if (by_inner && (n >= split || !inner[ni])) continue;
                            if (n < split)
                                votes[v - split].push_back((uint16_t)stand_in(n));
                            else if (choice[n - split] >= 0)
                                votes[v - split].push_back((uint16_t)choice[n - split]);
                        }
                }
            bool changed = false;
            for (int k = 0; k <= max_k; ++k) {
                if (choice[k] >= 0 || votes[k].empty()) continue;
                std::vector<uint16_t>& vk = votes[k];
                std::sort(vk.begin(), vk.end());
                int best = vk[0], best_n = 0;
                for (size_t i = 0; i < vk.size();) {
                    size_t j = i;
                    while (j < vk.size() && vk[j] == vk[i]) ++j;
                    if ((int)(j - i) > best_n) { best_n = (int)(j - i); best = vk[i]; }
                    i = j;
                }
                choice[k] = best;
                changed = true;
            }
            if (!changed) {
                if (by_inner) { pass = 9; continue; }    // on to the class tier
                if (by_class) { pass = 19; continue; }   // on to the fallback
                break;
            }
        }
        for (int k = 0; k <= max_k; ++k) {
            if (choice[k] < 0) continue;
            copy_entry(metatile_entry(B, (uint32_t)k),
                       metatile_entry(A, (uint32_t)choice[k]));
            if (!filled) keep_sentinel(1, metatile_entry(B, (uint32_t)k));
            ++filled;
        }
    }
    std::fprintf(stderr, "[lb_tileset] detail fix: %d detail entries refilled "
                 "for %d detail cells, %d blank base slot(s) stood in\n", filled,
                 detail_cells, slots);
    std::fflush(stderr);
    lb_widemap_terrain_changed();
    return true;
}
}  // namespace

// docs/388: EVERY REFILL IS ALSO THE BAND'S. Task_View_StreamBG1 (0x020396AC)
// calls this only when the view's +0x66 byte is set: View_CommitCamera sets
// it when the camera crosses a cell, FUN_02039DC0 when something asks for a
// redraw -- a wall retile, which rewrites plane E under a camera that has not
// moved -- and Task_View_CommitBGs clears it. lb_widemap's band was cached on
// the camera alone, so a wall built outside the native 256 columns stayed
// invisible until a zoom changed the band's bounds (walls that were built
// could not be seen until a zoom-in).
void fill_window_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kFillWindow);
    lb_widemap_terrain_changed();
    if (g_level_forced) {
        const uint32_t map = g_cpu.R[0];
        if (guest_ptr(map) && (!g_detail_done || !sentinels_hold()))
            g_detail_done = patch_forced_tables(map);
    }
    if (original) original();
}

// ---- docs/392: "PATCH THE GLITCH (SPECIALS ONLY)" ------------------------------
//
// func_0207B73C(?, caster handle, order, target handle) is the cast validator
// every CastSpellCommand passes through, on every machine, with the target
// the cast will act on -- including the target the glitch handed it (the
// tap keeps the last entity under the stylus, see kHotwireAnything). So a
// Hotwire (order effect id 32) whose target is not a transport or a special
// is refused. Vanilla targets are exactly categories 5 and 6, so legitimate
// hotwires pass. The target is resolved as func_0205C9B4 does: pool
// [0x02155028], index h[0] below its count, generation h[1] == entity+0xDD.
//
// THE REFUSAL IS ONE CALL OUT, IN THE VALIDATOR'S ONLY CALLER. The first
// build put it on func_0207B73C itself, and that cannot coexist with Hotwire
// Anything: the farm-allowance NOPs (kHotwireAnything, 0x0207B7F0..F8) are
// INSIDE func_0207B73C, a byte-patched function runs in Tier 3 because it
// fails the recompiler's content check, and lb_dispatch_original hands an
// override the lifted body, which predates the patch (docs/376). So the
// override would have silently put the farm check back.
//
// func_0207B9C0(out, ?, caster handle, order, [sp] target handle) is that
// caller (its bl at 0207B9F8 is the only call of func_0207B73C in the
// image). When the validator says 0 it stores the null handle -- the one
// func_02087580 -> func_02087454 points at, literal 0x0208745C -- to *out and
// returns. The override does exactly that, before anything else runs; the
// function's first call (func_02087580) only reads that pointer.
constexpr uint32_t kCastSpell = 0x0207B9C0u;       // arm
constexpr uint32_t kNullHandleLit = 0x0208745Cu;   // func_02087454's literal
void cast_spell_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kCastSpell);
    const uint32_t out = g_cpu.R[0], order = g_cpu.R[3];
    if (lb_setting_patch_hotwire_glitch() && out && order &&
        bus_read_u8(order) == 0x20u) {
        const uint32_t target = bus_read_u32(g_cpu.R[13]);   // the 5th argument
        const uint32_t pool = bus_read_u32(kEntityPoolPtr);
        const uint32_t index = check_in_ram(target) ? bus_read_u8(target) : ~0u;
        uint32_t e = 0u;
        if (pool >= 0x02000000u && pool < 0x02400000u &&
            index < bus_read_u32(pool + 0xCu)) {
            e = bus_read_u32(bus_read_u32(pool + 8u) + 4u * index);
            if (e && bus_read_u8(e + 0xDDu) != bus_read_u8(target + 1u)) e = 0u;
        }
        const uint8_t cat = e ? entity_category(e) : 0xFFu;
        const uint32_t nul = bus_read_u32(kNullHandleLit);
        if (e && cat != kCategoryTransport && cat != 6u && check_in_ram(nul)) {
            static int said = 0;
            if (said < 5) {
                ++said;
                std::fprintf(stderr, "[lb_hotwire] glitch patch: hotwire on a "
                             "category-%u target refused\n", (unsigned)cat);
                std::fflush(stderr);
            }
            bus_write_u8(out, bus_read_u8(nul));
            bus_write_u8(out + 1u, bus_read_u8(nul + 1u));
            lb_override_return();
            return;
        }
    }
    if (original) original();
}

// ---- docs/391: THE MOD MAPS JOIN THE GAME'S MISSION LIST ---------------------
//
// FUN_02045E50 (Thumb, once, from User::LanguageSelect::v08) registers the 30
// multiplayer maps: for each 28-byte entry of the table at 0x02147DDC (stem,
// players, name id, description id, 3, unlock blueprint, world) it allocates a
// 0x168-byte mission, constructs it and pushes it onto the registry vector at
// 0x021540E8. Its loop bound is 30 (cmp r7,#0x1e at 0x02045EB8).
//
// The mod maps (lb_mods.cpp) go through the same loop. When the stock pass
// returns -- into this override, because the entry set LR here -- the first N
// entries of the table and their "mpNN" stems are overwritten with the mod
// maps ("z/NN", the same five bytes), a frame like the function's own is laid
// on the stack with this override as its return address, and the loop is
// entered at 0x02045E58 with r5 at the table and r7 = 30 - N: N more passes.
// When that returns here too, the table and the stems are put back and the
// caller gets its return. Every mission is built by the game's own code.
// docs/394: the level loader, FUN_02035FAC (ARM). r0 is the map's name as a
// String (u32 handle, u32 length with the NUL, char* at +8); it builds
// "Maps/" (0x02144BB0) + name + ".map" (0x02144BB8) and loads it. Defined
// with the LOC helpers it needs, after the Plantation rename.
constexpr uint32_t kMapLoad = 0x02035FACu;
void map_load_observer(void);
constexpr uint32_t kRegisterMissions = 0x02045E50u;   // thumb
constexpr uint32_t kRegisterLoop = 0x02045E58u;
constexpr uint32_t kMissionTable = 0x02147DDCu;
constexpr uint32_t kMissionRegistry = 0x021540E8u;   // vector: +4 data, +8 count
constexpr uint32_t kMapListFill = 0x020C48B0u;        // thumb
extern "C" int lb_mods_count(void);                   /* lb_mods.cpp */
extern "C" int lb_mods_players(int k);
extern "C" int lb_mods_world(int k);
extern "C" unsigned lb_mods_name_id(int k);
extern "C" unsigned lb_mods_desc_id(int k);
extern "C" const char* lb_mods_name(int k);
namespace {
struct MissionReg {
    int phase = 0;                 // 0 idle, 1 stock pass, 2 mod pass, 3 done
    uint32_t sp = 0, lr = 0;
    int n = 0;
    uint8_t table[30 * 28];
    uint8_t stems[30][5];
    uint32_t stem_at[30];
    uint32_t mission[30];          // the registered mod missions
    uint32_t mode[30];             // their +0x10, while the MP list hides them
    bool hidden = false;
} g_mreg;
}  // namespace

void register_missions_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kRegisterMissions);
    const int n = std::min(lb_mods_count(), 30);
    if (n <= 0 || g_mreg.phase == 3) {
        if (original) original();
        return;
    }
    if (g_mreg.phase == 0) {
        g_mreg.sp = g_cpu.R[13];
        g_mreg.lr = g_cpu.R[14];
        g_mreg.n = n;
        g_mreg.phase = 1;
        g_cpu.R[14] = kRegisterMissions | 1u;   // the stock pass returns here
        if (original) original();
        return;
    }
    if (g_cpu.R[13] != g_mreg.sp) {   // not our return: leave it alone
        if (original) original();
        return;
    }
    if (g_mreg.phase == 1) {
        for (int i = 0; i < n * 28; ++i)
            g_mreg.table[i] = bus_read_u8(kMissionTable + (uint32_t)i);
        for (int k = 0; k < n; ++k) {
            const uint32_t e = kMissionTable + 28u * (uint32_t)k;
            const uint32_t s = bus_read_u32(e);
            g_mreg.stem_at[k] = s;
            char stem[6];
            std::snprintf(stem, sizeof stem, "z/%02d", k + 1);
            for (uint32_t i = 0; i < 5u; ++i) {
                g_mreg.stems[k][i] = bus_read_u8(s + i);
                bus_write_u8(s + i, (uint8_t)stem[i]);
            }
            bus_write_u32(e + 0x04u, (uint32_t)lb_mods_players(k));
            bus_write_u32(e + 0x08u, lb_mods_name_id(k));
            bus_write_u32(e + 0x0Cu, lb_mods_desc_id(k));
            bus_write_u32(e + 0x10u, 3u);
            bus_write_u32(e + 0x14u, 239u);   // Map_01's collectable: The Pond's
            bus_write_u32(e + 0x18u, (uint32_t)lb_mods_world(k));
        }
        // The frame `add sp,#0x14; pop {r4-r7,pc}` unwinds, returning here.
        const uint32_t frame = g_mreg.sp - 20u;
        bus_write_u32(frame + 0u, g_cpu.R[4]);
        bus_write_u32(frame + 4u, g_cpu.R[5]);
        bus_write_u32(frame + 8u, g_cpu.R[6]);
        bus_write_u32(frame + 12u, g_cpu.R[7]);
        bus_write_u32(frame + 16u, kRegisterMissions | 1u);
        g_cpu.R[13] = frame - 0x14u;
        g_cpu.R[5] = kMissionTable;
        g_cpu.R[6] = 0u;
        g_cpu.R[7] = 30u - (uint32_t)n;
        g_mreg.phase = 2;
        g_cpu.R[15] = kRegisterLoop;
        runtime_dispatch_with_exchange(kRegisterLoop | 1u);
        return;
    }
    // The mod pass returned: put the table back, note the missions, return.
    for (int i = 0; i < g_mreg.n * 28; ++i)
        bus_write_u8(kMissionTable + (uint32_t)i, g_mreg.table[i]);
    for (int k = 0; k < g_mreg.n; ++k)
        for (uint32_t i = 0; i < 5u; ++i)
            bus_write_u8(g_mreg.stem_at[k] + i, g_mreg.stems[k][i]);
    const uint32_t data = bus_read_u32(kMissionRegistry + 4u);
    const uint32_t count = bus_read_u32(kMissionRegistry + 8u);
    for (int k = 0; k < g_mreg.n; ++k)
        g_mreg.mission[k] =
            count >= (uint32_t)g_mreg.n
                ? bus_read_u32(data + 4u * (count - (uint32_t)g_mreg.n + (uint32_t)k))
                : 0u;
    g_mreg.phase = 3;
    std::fprintf(stderr, "[lb_mods] %d mod map(s) registered after the stock "
                 "30 (registry now %u missions); first \"%s\" at %08X\n",
                 g_mreg.n, (unsigned)count, lb_mods_name(0), g_mreg.mission[0]);
    std::fflush(stderr);
    g_cpu.R[14] = g_mreg.lr;
    lb_override_return();
}

// The mod maps' names are strings 3064.. of every language file (lb_mods.cpp),
// but the game's own fixup, FUN_0203E0B4, turns only the first 0xBF8 (3,064,
// the literal at 0x0203E0F8) of a loaded file's offsets into pointers. The
// rest are finished here, once a frame, the same way: a raw offset (below
// main RAM) plus the file's base. The file is [0x0215378C+0xC], its table
// at +8; a language change loads a new one and this finds it.
void mod_names_tick(void) {
    const int n = lb_mods_count();
    if (n <= 0) return;
    const uint32_t file = bus_read_u32(0x0215378Cu + 0x0Cu);
    if (file < 0x02000000u || file >= 0x02400000u ||
        bus_read_u32(file) != 0x474E414Cu)   // "LANG"
        return;
    const uint32_t table = bus_read_u32(file + 8u);
    if (table != file + 0x0Cu) return;       // not fixed up yet
    const uint32_t first = lb_mods_name_id(0);
    if (bus_read_u32(table + 4u * first) >= 0x02000000u) return;   // done
    for (uint32_t id = first; id < first + 2u * (uint32_t)n; ++id) {
        const uint32_t v = bus_read_u32(table + 4u * id);
        if (v && v < 0x02000000u) bus_write_u32(table + 4u * id, file + v);
    }
}

// The map list is SkirmishFactionSelect's, for Free Play (byte +0xA7 = 0) and
// for a multiplayer host (1). A joiner looks the host's map up among its own
// missions and writes through a null pointer when it is missing
// (FUN_020F5168), so until both machines are shown to hold the same maps the
// multiplayer list does not offer them: its filter (FUN_0204AFF8) keeps a
// mission only while +0x10 < 3.
void map_list_fill_override(void) {
    void (*original)(void) = lb_dispatch_original(0, kMapListFill);
    if (g_mreg.phase == 3) {
        const bool mp = bus_read_u8(g_cpu.R[0] + 0xA7u) != 0;
        if (mp != g_mreg.hidden) {
            for (int k = 0; k < g_mreg.n; ++k) {
                const uint32_t m = g_mreg.mission[k];
                if (!m) continue;
                if (mp) {
                    g_mreg.mode[k] = bus_read_u32(m + 0x10u);
                    bus_write_u32(m + 0x10u, 3u);
                } else {
                    bus_write_u32(m + 0x10u, g_mreg.mode[k]);
                }
            }
            g_mreg.hidden = mp;
        }
    }
    if (original) original();
}

// MUST BE SORTED ASCENDING BY ADDRESS. This is not a style preference.
//
// runtime_arm.cpp:270 lookup_in() is a binary search over the bank:
//
//     unsigned lo = 0, hi = len;
//     while (lo < hi) { unsigned mid = (lo + hi) >> 1u;
//         if (table[mid].addr < pc) lo = mid + 1u; else hi = mid; }
//
// so an out-of-order entry does not merely misplace itself -- it silently makes
// OTHER entries unfindable, and the failure is a quiet no-op with no log line.
//
// This table was unsorted: kSetHp (0x0205E7F4) sat at index 3, after kHealTick
// (0x02078770). Tracing all six addresses through the search by hand, THREE of
// the six were unreachable -- kSetHp, kProjectileDamage (0x0206E408; since
// docs/369 the boost sits on kApplyDamage instead) and kHealTick -- which
// is exactly "Attack Boost Reaches Ranged Units" and both halves of "Fix
// Hotwired Healing", the two the panel badged [read, not wired].
//
// docs/143 attributed that to the recompiler never dispatching to gameplay
// addresses. That diagnosis is WRONG and should be read with this note beside
// it: there are zero direct lifted-to-lifted calls in port/generated (10,810
// function definitions, 0 matches for `lb_arm9_afunc_XXXXXXXX()`), every
// transfer goes through runtime_dispatch, and all six addresses were always
// structurally reachable. The feature was dead because of one misplaced line
// here, not because of anything in ndsrecomp.
const NdsDispatchEntry kOverrideTable[] = {
    /* ARM. The roster builder. A pre-hook for two callers only: the Free Play
       opponent's (a Custom/Random army into its stack copy) and the
       multiplayer start's (bans). Otherwise a pure tail call. docs/384. */
    {kRosterBuild, 0u, roster_build_override, nullptr},    /* 0x02000E78 */
    /* ARM. THE DECOMPILATION'S FIRST GRADED FUNCTION, not a patch. With
       LB_SHADOW unset this delegates to the cartridge and only counts; with
       LB_SHADOW=native it runs src/battles/registry.c instead. docs/333. */
    {kRegistryFind, 0u, registry_find_shadow, nullptr},    /* 0x02016A00 */
    /* ARM. The level loader ("Maps/" + name + ".map"): an observer that notes
       the map's name for the master server's log (docs/394). */
    {kMapLoad, 0u, map_load_observer, nullptr},            /* 0x02035FAC */
    /* ARM. Runs immediately after the placement painter, from its only
       caller. Harvests the band the painter has just drawn, puts the native
       buffer back, and restores the camera -- all BEFORE the cartridge's own
       function, which reads the camera itself. Item #4. */
    {kFpPost, 0u, fp_post_override, nullptr},              /* 0x02036FBC */
    /* ARM. The placement-footprint painter. One frame in three it shifts the
       guest's camera by whole metatiles so the guest paints a BAND instead of
       its own window; the hook above tidies up. Item #4. */
    {kFpPaint, 0u, fp_paint_override, nullptr},            /* 0x020370B4 */
    /* ARM, not thumb. Runs the original, then notes that the guest is showing
       its selection box this frame and blanks the tilemap it drew it into. */
    {kMarquee, 0u, marquee_observer, nullptr},             /* 0x0203B7FC */
    /* ARM. The placement cell painter: an observer that notes each piece the
       wall-line painter hands it, so the band can draw the culled ones
       (docs/392). */
    {kFpCell, 0u, fp_cell_override, nullptr},              /* 0x0203CBB8 */
    /* ARM. String from a LOC id: at the Start handler's opponent name, a
       Custom or Random opponent's hero; otherwise a pure tail call.
       docs/385. */
    {kStringFromId, 0u, string_from_id_override, nullptr}, /* 0x0203E1D4 */
    /* Thumb. The mission list's registration: the mod maps after the stock
       30, through the game's own loop (docs/391). A pure tail call with no
       mods folder. */
    {kRegisterMissions, 1u, register_missions_override, nullptr}, /* 0x02045E50 */
    /* Generic VRAM transfer. Passes everything through untouched except the
       one source that is the guest's selection box, which it blanks first. */
    {kGfxTransfer, 0u, gfx_transfer_observer, nullptr},    /* 0x0204E0E0 */
    /* ARM. An image's pixels on their way to VRAM: the faction atlas gets
       the Custom and Random portraits (docs/390). Otherwise a pure tail
       call. */
    {kImageXfer, 0u, image_xfer_override, nullptr},        /* 0x0204E1D0 */
    {kSetOwner, 0u, set_owner_override, nullptr},          /* 0x020598C4 */
    /* GRADED, NOT PATCHED. docs/333. */
    {kU16IsSet, 0u, u16_is_set_shadow, nullptr},           /* 0x0205E638 */
    {kSetHp, 0u, set_hp_override, nullptr},                /* 0x0205E7F4 */
    /* GRADED, NOT PATCHED. docs/333. */
    {kListFind, 0u, list_find_shadow, nullptr},            /* 0x0206AC2C */
    /* ARM. Adds the attack boost to a ranged hit in front of the cartridge's
       applier, only on the ranged path (lr 0x0206E44C) and only with the
       setting on; otherwise a pure tail call. Replaces the old override on
       func_0206E408, whose nested guest call was not re-entrant. docs/369. */
    {kApplyDamage, 0u, apply_damage_observer, nullptr},    /* 0x0206E970 */
    {kHealTick, 0u, heal_tick_override, nullptr},          /* 0x02078770 */
    {kHotWireTick, 0u, hotwire_tick_override, nullptr},    /* 0x02078D5C */
    /* ARM. The cast validator's only caller: "Patch the glitch" refuses a
       hotwire on a non-transport, non-special the way a 0 from the validator
       does (docs/392). Otherwise a pure tail call. Not on func_0207B73C: the
       Hotwire Anything NOPs are inside it. */
    {kCastSpell, 0u, cast_spell_override, nullptr},        /* 0x0207B9C0 */
    /* Observer only, and only speaks with LB_GATHERLOG=1. docs/357. */
    {kTileGather, 0u, tile_gather_observer, nullptr},      /* 0x0207E9B4 */
    /* Substitutes the faction-aligned side at MATCH LOAD, before the
       music starts, so freeplay opens on the right calm/battle pair. */
    {kSetMusicSide, 1u, set_music_side_override, nullptr}, /* 0x02088980 */
    /* Thumb. The map parser: Force Tileset's name, and each level's world;
       otherwise a pure tail call. docs/387. */
    /* Thumb. Terrain_FillWindow: a forced level's detail cells and blank
       slots, patched once before its terrain draws. docs/387. */
    {kFillWindow, 1u, fill_window_override, nullptr},      /* 0x020A3234 */
    {kMapParse, 1u, map_parse_override, nullptr},          /* 0x020A369C */
    /* Control for the one below: docs/260 puts this ON the click path.
       Observer only, LB_CLICKDISPATCH=1. */
    {kTouchRect, 1u, touch_rect_observer, nullptr},        /* 0x020A8794 */
    /* Observer only, and only speaks with LB_ORDERLOG=1. See docs/238. */
    {kMoveIssue, 1u, move_issue_observer, nullptr},        /* 0x020A8F28 */
    /* Observer only, and only speaks with LB_CLICKDISPATCH=1. Item #4. */
    {kClickDispatch, 1u, click_dispatch_observer, nullptr}, /* 0x020AA8D4 */
    /* Sign-extends the drag rect's three register arguments before the guest
       normalises it, so a left-band corner stops inverting the rectangle. */
    {kSelRect, 1u, sel_rect_override, nullptr},            /* 0x020AA8E4 */
    /* Observer only, and only speaks with LB_MARKERLOG=1. */
    /* Thumb. Free Play's opponent mapper: Custom and Random answer 133 and
       queue their army; the six factions are a pure tail call. docs/384. */
    /* Thumb. User::MainMenu::virtual_09: a requested rename is saved by the
       game's own save in front of it; otherwise a pure tail call. docs/397. */
    {kMainMenuUpdate, 1u, main_menu_update_override, nullptr}, /* 0x020BF230 */
    {kEnemyMap, 1u, enemy_map_override, nullptr},          /* 0x020C0374 */
    /* Thumb. Its cycler: eight ways instead of six, live and in recordings
       that say enemy8=1; otherwise a pure tail call. docs/384. */
    {kOppSetupLabel, 1u, opponent_setup_override, nullptr}, /* 0x020C03B4 */
    {kEnemyCycle, 1u, enemy_cycle_override, nullptr},      /* 0x020C0E7C */
    /* Thumb. The map list's fill: a multiplayer list does not offer the mod
       maps (docs/391). */
    {kMapListFill, 1u, map_list_fill_override, nullptr},   /* 0x020C48B0 */
    {kMarkerPlace, 1u, marker_place_observer, nullptr},    /* 0x020DAAE4 */
    /* Thumb. The hero HUD's setup: with Prioritize Own Hero Spells, the
       player's own selected hero drives it; otherwise a pure tail call.
       docs/385. */
    {kHeroHud, 1u, hero_hud_override, nullptr},            /* 0x020DC004 */
    /* Thumb. WorldViewScreen::virtual_09, the battle view's update. Runs a
       pending battle shortcut's button callback in front of it, as a tail
       call that returns here; otherwise a pure tail call. docs/383. */
    {kViewUpdate, 1u, view_update_override, nullptr},      /* 0x020DD708 */
    /* Thumb. ScreenType::virtual_09, the shared menu-screen update. On the
       two keyboards, presses a typed key's own button in front of it; a pure
       tail call everywhere else. docs/396. */
    {kScreenUpdate, 1u, screen_update_override, nullptr},  /* 0x020E1BF4 */
    /* Thumb. The widget rectangle test, answered for where the port drew the
       widget; a pure tail call unless lb_hud_hit_live(). docs/371. */
    {kWidgetRect, 1u, widget_rect_observer, nullptr},      /* 0x020EA1A4 */
    /* Thumb. Hands the placement checkmark the point docs/372's widened rule
       gives it; every other call is a pure tail call. */
    {kWidgetSetPos, 1u, widget_setpos_observer, nullptr},  /* 0x020EA4D8 */
    /* GRADED, NOT PATCHED. docs/333. */
    {kSplitArray, 0u, split_array_shadow, nullptr},        /* 0x020EF1E0 */
    /* ARM. Steady pacing: NitroMain's two clock reads count VBlanks; every
       other call runs the cartridge's clock. docs/351. */
    {kGetMs, 0u, get_ms_override, nullptr},                /* 0x020F2648 */
    /* ARM. Byte fill and copy into the null page: named and declined.
       docs/369 (the combat crash). */
    /* ARM. GRADED, NOT PATCHED. The signed divide every `/` and `%` in the
       game calls -- the ARM7TDMI has no divide instruction. docs/333. */
    /* GRADED, NOT PATCHED. The soft-float trio. docs/333. */
    {kFAdd, 0u, fadd_shadow, nullptr},                     /* 0x0211F324 */
    {kFMul, 0u, fmul_shadow, nullptr},                     /* 0x0211F748 */
    {kFSub, 0u, fsub_shadow, nullptr},                     /* 0x0211F934 */
    {kDivMod, 0u, divmod_shadow, nullptr},                 /* 0x0211FDB4 */
};

// Compile-time guard so this cannot regrow. A runtime check would report the
// fault after shipping; this refuses to build. Every future entry must be
// inserted in address order or the build stops here with this message.
static_assert(kRosterBuild < kRegistryFind &&
              kSelRect < kMainMenuUpdate && kMainMenuUpdate < kEnemyMap &&
              kEnemyMap < kOppSetupLabel &&
              kOppSetupLabel < kEnemyCycle &&
              kEnemyCycle < kMarkerPlace && kMarkerPlace < kHeroHud &&
              kHeroHud < kViewUpdate && kViewUpdate < kScreenUpdate &&
              kScreenUpdate < kWidgetRect &&
              kRegistryFind < kMapLoad && kMapLoad < kFpPost &&
              kFpPost < kFpPaint && kFpPaint < kMarquee &&
              kMarquee < kFpCell && kFpCell < kStringFromId &&
              kStringFromId < kRegisterMissions &&
              kRegisterMissions < kGfxTransfer &&
              kEnemyCycle < kMapListFill && kMapListFill < kMarkerPlace &&
              kGfxTransfer < kImageXfer && kImageXfer < kSetOwner &&
              kSetOwner < kSetHp && kSetHp < kApplyDamage &&
              kApplyDamage < kHealTick && kHealTick < kHotWireTick &&
              kHotWireTick < kCastSpell && kCastSpell < kTileGather &&
              kTileGather < kSetMusicSide &&
              kSetMusicSide < kFillWindow && kFillWindow < kMapParse &&
              kMapParse < kTouchRect &&
              kTouchRect < kMoveIssue &&
              kMoveIssue < kClickDispatch && kClickDispatch < kSelRect &&
              kSelRect < kMarkerPlace && kMarkerPlace < kWidgetRect &&
              kWidgetRect < kWidgetSetPos && kWidgetSetPos < kSplitArray &&
              kSplitArray < kGetMs &&
              kGetMs < kFAdd &&
              kFAdd < kFMul && kFMul < kFSub && kFSub < kDivMod &&
              kSetOwner < kU16IsSet && kU16IsSet < kSetHp &&
              kSetHp < kListFind && kListFind < kApplyDamage,
              "kOverrideTable must be sorted ascending by address: "
              "runtime_arm.cpp lookup_in() binary-searches it, and an "
              "out-of-order entry silently hides other entries.");
const NdsDispatchEntry* const kOverrides = kOverrideTable;
constexpr unsigned kOverrideCount =
    (unsigned)(sizeof(kOverrideTable) / sizeof(kOverrideTable[0]));

// ── Make All Builders The Same ─────────────────────────────────────────────
//
// The setting makes all builders' stats -- damage, speed, costs, payouts and
// so on -- match those of the Imperial builder, the community's gold-standard
// builder.
//
// This one is DATA, not code, so it needs no dispatch override. BP/Entities.ebp
// is a 'BPNZ' magic followed by fixed records; unit blueprints are 0x7C bytes
// and record N starts at 4 + N*0x7C. docs/data/entities.tsv has all 551 records
// and the six builders are indices 2, 22, 42, 62, 82 and 102 -- King, Wizard,
// Pirate, Imperial, Earth, Alien -- which the offsets in that table confirm
// (index 2 -> 252, index 62 -> 7692).
//
// The loader is func_02001F3C and it fixes the file up IN PLACE in main RAM, so
// the block can be found by its magic and edited directly.
//
// IDENTITY FIELDS ARE NOT COPIED. Name, asset path, string ids, faction and
// category stay per-faction: the setting means the same STATS, not six
// Imperial engineers.

constexpr uint32_t kRecordSize = 0x7Cu;
constexpr uint32_t kFirstRecord = 4u;
constexpr int kImperialBuilder = 62;
constexpr int kOtherBuilders[] = {2, 22, 42, 82, 102};
// docs/396: the setting chooses WHICH builder the others match (it was the
// Imperial one only): 1..6 = King, Wizard, Pirate, Imperial, Earth, Alien,
// the records below in that order. 0 is off.
constexpr int kBuilders[6] = {2, 22, 42, 62, 82, 102};

struct StatField {
    uint32_t off;
    uint8_t width;
    const char* name;
};

// Offsets within a UnitBlueprint record, from tools/lb_blueprints.py.
constexpr StatField kStatFields[] = {
    {0x0Cu, 4, "speed"},         {0x10u, 4, "field_10"},
    {0x15u, 1, "move_class"},    {0x5Eu, 2, "cost"},
    {0x60u, 2, "build_time"},    {0x62u, 2, "health"},
    {0x64u, 2, "mana"},          {0x68u, 1, "melee_damage"},
    {0x6Au, 1, "melee_spread"},  {0x6Cu, 1, "resource_yield"},
    {0x6Du, 1, "attack_interval"}, {0x6Fu, 1, "attack_range"},
    {0x70u, 1, "field_70"},      {0x71u, 1, "interaction_range"},
};

// The Imperial builder's shipped stats, from docs/data/entities.tsv. These are
// a FINGERPRINT, not a source of values: the patch copies whatever is actually
// in memory, and uses these only to prove it found the blueprint block before
// writing anything. Scanning RAM for a four-byte magic can land on anything.
constexpr uint16_t kImperialCost = 55u;
constexpr uint16_t kImperialBuildTime = 150u;
constexpr uint16_t kImperialHealth = 160u;
constexpr uint32_t kImperialSpeed = 375u;

uint32_t g_blueprint_base = 0u;   // guest address of the 'BPNZ' magic
int g_builders_patched = 0;       // docs/396: the source applied, 1..6; 0 none
// docs/396: bumped by a game reset (the menu's Reset): every one-shot patch
// site compares it with the generation it last applied in and starts over,
// because the reset reloaded the stock ARM9 image under it.
unsigned g_boot_gen = 0u;

uint32_t record_addr(uint32_t base, int index) {
    return base + kFirstRecord + (uint32_t)index * kRecordSize;
}

// Does `base` really point at Entities.ebp? Check the Imperial builder's four
// most distinctive stats rather than trusting the magic alone.
bool blueprint_block_valid(uint32_t base) {
    const uint32_t imp = record_addr(base, kImperialBuilder);
    return bus_read_u16(imp + 0x5Eu) == kImperialCost &&
           bus_read_u16(imp + 0x60u) == kImperialBuildTime &&
           bus_read_u16(imp + 0x62u) == kImperialHealth &&
           bus_read_u32(imp + 0x0Cu) == kImperialSpeed;
}

uint32_t find_blueprint_block() {
    // Main RAM only: 0x02000000..0x023FFFFF. Stop at the first candidate that
    // passes the fingerprint.
    for (uint32_t a = 0x02000000u; a < 0x02400000u - kRecordSize * 128u; a += 4u) {
        if (bus_read_u32(a) != 0x5A4E5042u) continue;   // 'BPNZ' little-endian
        if (blueprint_block_valid(a)) return a;
    }
    return 0u;
}

// docs/386: the five builders' copied fields as the game loaded them, taken at
// the blueprints' discovery before anything is applied, so turning the option
// off (or changing the balance pack under it) can put them back.
struct BuilderByte { uint32_t addr; uint8_t loaded; };
std::vector<BuilderByte> g_builders_loaded;

void snapshot_same_builders() {
    g_builders_loaded.clear();
    for (int which : kBuilders) {   // docs/396: all six; any can be overwritten
        const uint32_t dst = record_addr(g_blueprint_base, which);
        for (const StatField& f : kStatFields)
            for (uint32_t b = 0; b < f.width; ++b)
                g_builders_loaded.push_back({dst + f.off + b,
                                             (uint8_t)bus_read_u8(dst + f.off + b)});
    }
}

// docs/386: THE BLOCK IS NOT FINISHED WHEN IT IS FOUND. The fingerprint
// (record 62) matches while later records are still being written: a
// snapshot taken at discovery held other bytes for records 145..199, and
// putting it back left 319 bytes that were not the game's (run t386a). The
// same race meant a pack applied at discovery lost its changes to those
// records to the loader -- the old "830 of 835 changed (5 already matched)"
// was measured on the half-loaded block; on the finished one all 835 change.
// So nothing is snapshot or applied until the span the edits reach has held
// still: an FNV-1a hash of it every 20 frames, three times equal.
bool g_bp_settled = false;
uint32_t g_bp_hash = 0u;
int g_bp_steady = 0;
unsigned g_bp_frames = 0u;

uint32_t blueprint_span_hash(uint32_t base) {
    uint32_t end = lb_balance_extent();
    const uint32_t builders_end =
        record_addr(0u, kOtherBuilders[4]) + kRecordSize;   // record 102's end
    if (builders_end > end) end = builders_end;
    uint32_t h = 2166136261u;
    for (uint32_t o = 0; o < end; o += 4u)
        h = (h ^ bus_read_u32(base + o)) * 16777619u;
    return h;
}

void revert_same_builders() {
    for (const BuilderByte& b : g_builders_loaded)
        if (bus_read_u8(b.addr) != b.loaded) bus_write_u8(b.addr, b.loaded);
}

// docs/396: from the chosen builder (1..6), as the game has it now -- so with
// a balance pack that is the PATCHED one, the pack being applied first.
void apply_same_builders(int from) {
    const uint32_t imp = record_addr(g_blueprint_base, kBuilders[from - 1]);
    for (int which : kBuilders) {
        if (which == kBuilders[from - 1]) continue;
        const uint32_t dst = record_addr(g_blueprint_base, which);
        for (const StatField& f : kStatFields) {
            switch (f.width) {
                case 4: bus_write_u32(dst + f.off, bus_read_u32(imp + f.off)); break;
                case 2: bus_write_u16(dst + f.off, bus_read_u16(imp + f.off)); break;
                default: bus_write_u8(dst + f.off, bus_read_u8(imp + f.off)); break;
            }
        }
    }
}

// ── Unit and building caps ─────────────────────────────────────────────────
//
// "Enhancement and host options should be able to raise building and unit caps
// beyond vanilla."
//
// docs/58 and docs/68: the allowance table is a BYTE ARRAY at 0x02126D94,
// indexed by BUILD-LIMIT CLASS rather than by blueprint category --
// func_020860A8 dispatches on the class through a nine-arm jump table, each arm
// counting a whole set of categories. The shipped bytes are:
//
//     01 14 04 07 0E 1D 14 08 00 05
//      0  1  2  3  4  5  6  7  8  9
//
// giving Heroes 1, Minifigures 20, Specials 4, Towers 7, Walls 20, Bridges 8,
// Gates 0. Classes 4, 5 and 9 are not named in docs/68, so they are not named
// here either.
//
// It lives in ARM9 static data, which is resident in main RAM, so the lifted
// code reads it through the bus and a write here is seen by the game.
//
// UNLIKE THE BLUEPRINT PATCH, THIS ONE IS REVERSIBLE. The shipped values are
// known exactly, so clearing an override restores the cartridge's own byte
// rather than leaving the change stuck until a reload.
//
// A byte is a byte: the ceiling is 255, and the UI says so.

constexpr uint32_t kAllowanceTable = 0x02126D94u;
constexpr int kAllowanceClasses = 10;
constexpr uint8_t kShippedAllowance[kAllowanceClasses] = {
    1, 20, 4, 7, 14, 29, 20, 8, 0, 5,
};

bool allowance_table_valid() {
    for (int i = 0; i < kAllowanceClasses; ++i) {
        const uint8_t live = bus_read_u8(kAllowanceTable + (uint32_t)i);
        // Accept either the shipped byte or a value this patch already wrote.
        if (live != kShippedAllowance[i] && lb_setting_cap(i) == 0) return false;
    }
    return true;
}

bool g_caps_announced = false;

void apply_caps() {
    if (!allowance_table_valid()) return;
    for (int i = 0; i < kAllowanceClasses; ++i) {
        int want = lb_setting_cap(i);
        if (want <= 0) want = kShippedAllowance[i];      // restore vanilla
        if (want > 255) want = 255;                       // it is one byte
        const uint32_t at = kAllowanceTable + (uint32_t)i;
        if (bus_read_u8(at) != (uint8_t)want) {
            bus_write_u8(at, (uint8_t)want);
            if (!g_caps_announced) {
                g_caps_announced = true;
                std::fprintf(stderr,
                             "[lb_patch] allowance table at 0x%08X is live; "
                             "class %d set to %d (shipped %d)\n",
                             kAllowanceTable, i, want, kShippedAllowance[i]);
            }
        }
    }
}

}  // namespace

// Called once per frame from the VBlank path (see port/runner/apply_hooks.py).
// Cheap when the toggle is off, which is the common case.
// The balance patcher needs the same block this file already locates and
// fingerprints, rather than scanning 4 MB of RAM a second time.
extern "C" unsigned int lb_patches_blueprint_base(void) {
    return (unsigned int)g_blueprint_base;
}
// docs/386: whether the builders are equalised in the game right now.
extern "C" int lb_same_builders_applied(void) { return g_builders_patched; }

// docs/386: WHEN THE BLUEPRINTS MAY CHANGE UNDER THE GAME. The rebalance
// setting takes effect immediately when the player is not in gameplay or a
// lobby. A unit reads its blueprint as it is made and as it
// acts, so a change in a battle would give one match two rule sets, and a
// lobby's machines must start from the same blueprints (each applies its own
// setting; nothing carries it). The same tests as Set Team's (docs/384):
//   * the network is idle (not hosting, in a lobby, joining or browsing);
//   * no battle view (registry slot 0x5C, which stays under the pause menu);
// and one more, because a battle is being set up before its view exists:
//   * no unit on the field (the entity pool, as lb_field walks it).
extern "C" int lb_net_guest_state(void);   /* src/lb_net.cpp */

extern "C" const char* lb_blueprint_edit_blocked(void) {
    const int net = lb_net_guest_state();
    if (net != 0 && net != -1)
        return "not while hosting, in a lobby or browsing games";
    const uint32_t view = bus_read_u32(kViewSlot);
    if (guest_ptr(view) && bus_read_u32(view + 0x1Cu) != 0u)
        return "not during a battle (the pause menu counts)";
    if (lb_field_entity_at(0, nullptr, nullptr, nullptr, nullptr))
        return "not while a battle is loading or ending";
    return nullptr;
}

// ---------------------------------------------------------------- cheats
//
// docs/171: the cartridge keeps one enable byte per cheat in a global flags
// block reached through *(u32*)0x02150A70, and func_020088D4's jump table reads
// flags[i] for case i across all eighteen cases 0..17. So writing flags[i] is
// the game's own switch, not forged state.
//
// Only CHANGES are written. Mirroring the overlay every frame would fight the
// cartridge's own cheat-code screen, which sets the same bytes; tracking the
// last value applied means the port only intervenes when the player actually
// moves a control here.
static const uint32_t kCheatFlagsPtr = 0x02150A70u;

// LB_3D_DUMP=<path.ppm> writes the rasterized 3D layer on its own, once.
// docs/156 tied engine A's BG0-3D to the units; if that holds, this image is
// the units with nothing else in it, which would make the sprite learner's
// terrain differencing unnecessary rather than merely imperfect.
static void dump_3d_layer_if_asked(void) {
    static int state = -1;
    static const char* path = nullptr;
    if (state < 0) {
        path = std::getenv("LB_3D_DUMP");
        state = path ? 1 : 0;
    }
    if (state != 1) return;
    // Wait for a LEVEL, not merely for the layer to be non-empty. The first
    // version fired on the first frame with 64 coloured pixels and caught the
    // army-customise screen, whose LEGO brick panels are themselves drawn in 3D
    // -- an image full of brown bricks that looked like evidence the units are
    // not 3D at all, and was nothing of the kind.
    static LbFieldInfo info;
    static bool located = false;
    if (!located) {
        if (!lb_field_locate(&info)) return;
        located = true;
    }
    int nonzero = 0;
    for (int y = 0; y < 192; ++y) {
        const uint32_t* row = nds_gpu3d_line(y);
        if (!row) return;
        for (int x = 0; x < 256; ++x) if (row[x] & 0x00FFFFFFu) ++nonzero;
    }
    if (nonzero < 64) return;
    state = 0;

    std::vector<uint8_t> rgb((size_t)256 * 192 * 3);
    std::vector<uint8_t> alpha((size_t)256 * 192);
    for (int y = 0; y < 192; ++y) {
        const uint32_t* row = nds_gpu3d_line(y);
        for (int x = 0; x < 256; ++x) {
            const uint32_t v = row[x];
            const size_t o = ((size_t)y * 256 + (size_t)x);
            rgb[o * 3 + 0] = (uint8_t)(v >> 16);
            rgb[o * 3 + 1] = (uint8_t)(v >> 8);
            rgb[o * 3 + 2] = (uint8_t)v;
            alpha[o] = (uint8_t)(v >> 24);
        }
    }
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fprintf(f, "P6\n256 192\n255\n");
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
    }
    char ap[512];
    std::snprintf(ap, sizeof ap, "%s.alpha.pgm", path);
    f = std::fopen(ap, "wb");
    if (f) {
        std::fprintf(f, "P5\n256 192\n255\n");
        std::fwrite(alpha.data(), 1, alpha.size(), f);
        std::fclose(f);
    }
    std::fprintf(stderr, "[lb_3d] wrote %s: %d of 49152 pixels carry colour\n",
                 path, nonzero);
}

extern "C" int lb_lag_dx(void);   /* docs/335, defined below */
extern "C" int lb_widemap_cam_pick_age(void);  /* lb_widemap.cpp */
extern "C" unsigned lb_gx_submitted(void);     /* gpu3d.cpp, docs/337 */
extern "C" int lb_widemap_cam_px(int* x, int* y);  /* lb_widemap.cpp */
/* Defined further down this file. Printed in the polylist header so a trace
   says whether the correction actually FIRES, and by how much -- not merely
   that the env var was set. */
extern "C" int lb_reshift_dx(void);            /* docs/337 */
// The demo recorder's frame counter (lb_demo.cpp), printed beside the trace's
// own so a camera sample can be joined to the keys held on that exact frame of
// a recording. The two count different things -- VBlanks against composited
// frames -- so neither can stand in for the other (docs/347).
extern "C" unsigned lb_demo_frame(void);

// THE SUB-TILE SCROLL PHASE, from the guest's own registers.
//
// docs/195 read this out of the cartridge: func_0202D1B4 (the BGxOFS setter)
// writes `-param_2 & mask`, and func_020396E0 calls it with
// `param_2 = 4 - (camx mod 8)` horizontally. Inverting that gives an invariant
// the ORIGINAL renderer satisfies by construction:
//
//     camx & 7 == (BG1HOFS + 4) & 7
//
// The band synthesis works entirely in TILES -- its "phase" is the position
// within a 3-tile metatile, not a pixel offset -- and relies on the BG
// renderer to apply BG1HOFS to the widened columns along with the native ones.
// That is an assumption, and it has never been checked while the camera was
// moving: the harness samples BG1HOFS only at settled pan-step boundaries,
// where the camera is tile-aligned and the register is trivially constant.
//
// Printing it per frame is what turns the assumption into a measurement --
// PROVIDED it is the right copy. BGxOFS is WRITE-ONLY on the DS: a debug read
// of 0x04000014 returns 0 on every frame forever, and gpu2d.cpp already says
// so ("those registers are write-only, and reading them back returned 0 while
// the picture plainly showed a non-zero offset"). Reading it that way produced
// a tidy and entirely false "the invariant holds on 58.7% of frames", which
// was a measurement of the constant 0 and nothing else.
//
// lb_bg_scroll() returns g_unit[engine].bgx[bg] -- the bias the renderer
// actually scrolls with, exported from gpu2d.cpp by apply_hooks.py.
extern "C" unsigned lb_bg_scroll(int engine, int bg, int vert);

// The guest's SwapBuffers count (lb_widemap.cpp). Differenced per frame it
// says whether the guest is updating its world every frame or every other,
// which is the question docs/340 leaves open.
extern "C" unsigned lb_band_promote_count(void);

// ---- LB_POLYLIST: the render list, as numbers ------------------------------
//
// The menu misalignment could not be settled from composited pixels, and two
// rounds of trying cost a wrong hypothesis each time. A centroid cannot tell
// "this element MOVED" from "this element is drawn TWICE, extra_y apart", and
// the 800x600 capture shows the second: the Story book appears at its correct
// row AND again one extra_y lower, with the orange ring around the lower copy.
// At surface height 192 the two coincide and the menu is correct.
//
// So the question is a question about the render list, not about pixels: does
// the list contain one book quad or two? This prints it -- every polygon's
// surface box, the same box converted back to the guest's own 256x192, its
// depth range and its texture id -- so that can be read off rather than
// inferred.
//
// LB_POLYLIST=<n> prints the list every n frames. 1 is every frame and is far
// too much; 120 is a readable cadence.
static void dump_polygon_list_if_asked(void) {
    static int every = -1;
    if (every < 0) {
        const char* s = std::getenv("LB_POLYLIST");
        every = s ? std::atoi(s) : 0;
    }
    if (every <= 0) return;
    // THE SHARED FRAME NUMBER, not a private one.
    //
    // This used to count its own vblanks while the layer dump counted its own
    // composites, and the two drifted -- so a tool pairing "polylist frame N"
    // with "lb_layers-...-N.pgm" was comparing different moments and could not
    // say so. It cost a wrong reading: a unit's polygon box landed on pixels
    // from another frame and the sprite looked entirely undrawn, while the same
    // dump held 3D content at rows no polygon in that list reached.
    //
    // lb_dump_frame is the number the composited frame and the whole-map render
    // already agree on, for exactly this reason. Read, never ticked: the
    // compositor owns the tick.
    const int me = (int)lb_dump_frame_get();
    if ((me % every) != 0) return;
    // ...and only ONCE for a given frame, since this runs every vblank and the
    // number now only changes when a frame is composited.
    static int last_me = -1;
    if (me == last_me) return;
    last_me = me;

    const int ow = (int)nds_gpu3d_output_width();
    const int oh = (int)nds_gpu3d_output_height();
    const int extra = lb_band_left(ow);
    const int extra_y = lb_band_top(oh);
    const uint32_t n = nds_gpu3d_render_polygon_count();
    // THE CAMERA GOES IN THE HEADER, because without it the dump cannot be
    // compared across frames. `surf_x + camx - band` is a stationary unit's
    // world position and must not move; see lb_widemap_cam_px.
    // THE RENDER PAIR, not the live one. These polygons were transformed last
    // frame; pairing them with this vblank's camera and band manufactures one
    // pan step of fake shear. `rcam`/`rband` came through GPU3D::VBlank with
    // the geometry itself.
    int cpx = 0, cpy = 0, rcx = 0, rcy = 0, ctx = 0, cty = 0;
    const int cam_ok = lb_widemap_cam_px(&cpx, &cpy);
    lb_band_render_cam(&rcx, &rcy);
    // THE CAMERA THE 2D LAYERS ARE DRAWN FROM, in tiles.
    //
    // choose_camera picks this by measuring candidate cameras against the
    // guest's own tilemap, so it is the one the TERRAIN matches. rcam is the
    // one the GEOMETRY was transformed against. On real hardware they are the
    // same frame; here the 3D comes from a snapshot of the frame the renderer
    // just finished while the 2D is decoded live from VRAM, so during motion
    // they can be a frame apart -- which is a unit sliding against the ground
    // under it, and is what "sprites become jittery" would be.
    //
    // Printing both is what turns that from an explanation into a measurement:
    // ctx*8 against rcam, on the same line, every frame.
    lb_widemap_cam_tile(&ctx, &cty);
    std::fprintf(stderr,
                 "[lb_polylist] frame %d surface %dx%d extra %d,%d "
                 "cam %d,%d rcam %d,%d chosen %d,%d rband %d,%d lagdx %d "
                 "campick %d subm %u rsdx %d hofs %u vofs %u prom %u "
                 "demo %u%s "
                 "-- %u polys\n",
                 me, ow, oh, extra, extra_y, cpx, cpy, rcx, rcy,
                 ctx * 8, cty * 8,
                 lb_band_render(), lb_band_render_y(), lb_lag_dx(),
                 lb_widemap_cam_pick_age(), lb_gx_submitted(), lb_reshift_dx(),
                 lb_bg_scroll(0, 1, 0), lb_bg_scroll(0, 1, 1),
                 lb_band_promote_count(), lb_demo_frame(),
                 cam_ok ? "" : " (STALE: no field geometry)", n);
    for (uint32_t i = 0; i < n; ++i) {
        NdsGpu3dPolygonSnapshot p{};
        if (!nds_gpu3d_render_polygon(i, &p)) continue;
        std::fprintf(stderr,
                     "[lb_polylist]  %3u sub %3u v%u  surf x %4d..%-4d "
                     "y %4d..%-4d  guest x %4d..%-4d y %4d..%-4d  "
                     "z %u..%u  tex %08X pal %08X\n",
                     i, p.submission_index, p.vertex_count,
                     p.min_x, p.max_x, p.min_y, p.max_y,
                     p.min_x - extra, p.max_x - extra,
                     p.min_y - extra_y, p.max_y - extra_y,
                     p.min_z, p.max_z, p.tex_param, p.tex_palette);
    }
}

// ---- the view's own dimensions (docs/183) ---------------------------------
//
// This is the cartridge's Set_View_Dimensions, and it is seven instructions:
//
//     0203339C  ldr ip, =020EEB1C     the dimension setter
//     020333A0  ldr r0, =02151F28     the view-dimension descriptor
//     020333A4  mov r1, #0x20         width  in tiles  <- the constant
//     020333A8  mov r2, #0x18         height in tiles
//     020333AC  bx  ip
//
// The setter does NOT merely record the width. It frees the old buffers, stores
// width and height, computes the fog mask's row stride as (width + 7) / 8, then
// ALLOCATES the mask at stride * height and a row-pointer table at height * 4,
// and fills the row table. Every structure derived from the view size is
// rebuilt from the new width.
//
// That retracts docs/164, which read the same setter as a stride assignment and
// concluded that widening it would overrun a 96-byte buffer nothing had grown.
// Eleven further instructions show the allocation. The overrun does not exist.
//
// Patching the immediate is therefore the whole change: raise it and the game
// sets up a wider view, sized correctly, through its own code path.
static const uint32_t kViewDimThunk = 0x020333A4u;   // the "mov r1, #0x20"
static const uint32_t kViewDimStock = 0xE3A01020u;   // mov r1, #32

static void apply_view_width(void) {
    const int px = lb_setting_view_width();
    if (px <= 256) return;
    // Tiles of 8 px, and the immediate field of "mov r1, #imm" is one byte.
    const uint32_t tiles = (uint32_t)(px / 8);
    if (tiles < 32u || tiles > 255u) return;

    // Only ever rewrite the instruction this port recognises. The overlay
    // holding it is not resident at boot -- reading 020333A4 before the front
    // end has loaded it returns unrelated bytes -- so this waits for the exact
    // stock encoding rather than assuming the address is code yet.
    const uint32_t cur = bus_read_u32(kViewDimThunk);
    const uint32_t want = 0xE3A01000u | tiles;
    if (cur == want) return;                 // already patched
    if (cur != kViewDimStock) return;        // not loaded, or not what we think

    bus_write_u32(kViewDimThunk, want);
    std::fprintf(stderr,
                 "[lb_fov] view width %d -> %u tiles (%u px); the setter "
                 "reallocates the fog mask to %u bytes\n",
                 32, tiles, tiles * 8u, ((tiles + 7u) / 8u) * 24u);
}

// ---- the view's own HEIGHT, which was never patched ------------------------
//
// Set_View_Dimensions passes BOTH dimensions and the port only ever rewrote
// one of them:
//
//     020333A4  mov r1, #0x20     width  in tiles   <- apply_view_width
//     020333A8  mov r2, #0x18     height in tiles   <- nothing
//
// The setter stores the height at +0x170, allocates the fog mask at
// stride * HEIGHT and the row-pointer table at height * 4, and fills the row
// table -- so the height is as much a part of the reallocation as the width
// is, and raising only the width gives a view that is wider but still 24 tiles
// tall.
//
// MEASURED, and this is what sent me here. With LB_ASPECTBOX=1 drawing the
// guest's own 256x192 into the widened surface, a freeplay capture shows the
// revealed terrain with PERFECTLY STRAIGHT top and bottom edges lying exactly
// on the box, while its right edge is ragged. A ragged edge is real
// exploration; a straight one on the native aspect is a clip. The camera had
// panned only horizontally, so rows outside the native 192 had never been in
// the game's view and were never marked -- the fog is square around the
// original aspect, which is the shape of the defect.
//
// This is the same asymmetry the entity gather rect had, where only the
// horizontal half was widened and "units above and below the band were never
// in the list". Fixing that one did not fix this one because they are separate
// rectangles: the gather rect decides what is SUBMITTED, the view dimensions
// decide what the game considers VISIBLE.
//
// Kept on its own switch. The width knob is env-only because docs/231 measured
// it degrading click selection, and until that is understood the height must
// not ride in on the same variable -- otherwise an A/B of the width is really
// an A/B of both and neither can be read.
static const uint32_t kViewDimHeight = 0x020333A8u;   // the "mov r2, #0x18"
static const uint32_t kViewDimHeightStock = 0xE3A02018u;  // mov r2, #24

// Static, not an export: it is read here and nowhere else, and an exported
// symbol with no caller outside its own translation unit is precisely what
// tools/lb_wiring.py exists to flag. lb_setting_view_width is extern "C"
// because it lives in lb_overlay.cpp and is read from this file.
static int lb_setting_view_height(void) {
    static int px = -1;
    if (px < 0) {
        const char* v = std::getenv("LB_VIEW_HEIGHT");
        px = v ? std::atoi(v) : 0;
    }
    return px;
}

static void apply_view_height(void) {
    const int px = lb_setting_view_height();
    if (px <= 192) return;
    const uint32_t tiles = (uint32_t)(px / 8);
    if (tiles < 24u || tiles > 255u) return;

    const uint32_t cur = bus_read_u32(kViewDimHeight);
    const uint32_t want = 0xE3A02000u | tiles;
    if (cur == want) return;                      // already patched
    if (cur != kViewDimHeightStock) return;       // not loaded, or not stock

    bus_write_u32(kViewDimHeight, want);
    std::fprintf(stderr,
                 "[lb_fov] view height %d -> %u tiles (%u px); the setter "
                 "reallocates the row table to %u bytes\n",
                 24, tiles, tiles * 8u, tiles * 4u);
}

// ---- entity cull width (units) ---------------------------------------------
//
// WHY UNITS DO NOT APPEAR IN THE WIDENED COLUMNS.
//
// The terrain and the fog were widened, and the 3D layer really is rendered at
// the full 448 columns -- but units kept not showing up out there, and the
// standing explanation ("units are 3D, and 3D already reaches the widened
// columns") only covered the DRAWING half.
//
// The other half is that the game never submits them. func_02038F28 gathers
// entities into a rectangle around the camera, and anything outside it is not
// gathered at all, so no amount of widening the renderer can make it appear.
//
// THE RECT IS IN METATILE COLUMNS AND ROWS, NOT PIXELS. Reading the whole
// function rather than the one immediate is what corrected this port's first
// attempt. func_020085a0 divides X by 24 and Y by 16 -- the metatile size --
// and hands both to func_020f2bbc, which stores X at +4 and Y at +5. That is
// what fixes which output byte is which; the two axes do NOT get the same
// margin, so guessing had a fifty-fifty chance of patching the wrong one:
//
//     02038FB0  E3500000  cmp   r0, #0        ; r0 = metatile column of camx
//     02038FB4  03A00000  moveq r0, #0
//     02038FB8  E2400001  subne r0, r0, #1    ; min X = col(camx) - 1
//     ...
//     02038FD0  E2822C01  add   r2, r2, #0x100
//     ...                                     ; max X = col(camx + 256) + 1
//
// (The +3 margin further down is on ROWS, which is the overhang for sprites
// tall enough to reach up into view from below the bottom edge.)
//
// SO THE FIRST FIX WAS HALF A FIX. The widened surface is 448 px and the
// guest's own view is columns 96..351 of it, so what is on screen spans
// camx-96 .. camx+352. Rewriting only the immediate at 02038FD0 -- which is
// all this port did at first, and all docs/198 ever A/B'd -- widens the RIGHT
// edge and leaves the left edge at the stock one-column margin, 24 px, when
// the view reaches 96 px out. Units in the LEFT widened column stayed culled.
//
// 96 px is exactly four metatile columns, so the left margin has to go from
// 1 to 5. It cannot be done by editing the literal alone: the stock block
// clamps at zero, and "col - 5" on a camera in the first four columns would
// underflow the byte to 0xFB and send the caller walking a nonsense range. So
// the whole three-instruction block is replaced, keeping a clamp:
//
//     E3500005  cmp   r0, #5
//     B3A00000  movlt r0, #0
//     A2400005  subge r0, r0, #5
//
// Only ever rewrites the exact stock encodings, and only while the widening is
// on, so a vanilla-width session is untouched and a mismatched build is left
// alone rather than being patched blind.
const uint32_t kCullImmAddr = 0x02038FD0u;
const uint32_t kCullStock = 0xE2822C01u;      // add r2, r2, #0x100

// The left-margin block at 02038FB0, replaced whole so the clamp survives.
const uint32_t kCullMinAddr = 0x02038FB0u;
// 12400001, NOT E2400001. The third instruction is `subne`, so its condition
// nibble is 1, not the unconditional E. Transcribing it as E2400001 meant the
// guard never matched and the left margin was never widened -- and because the
// only thing printed was the right edge's message, the run looked patched. The
// Hero stood at screen x -61, squarely inside the left widened column, and
// nothing was drawn there.
const uint32_t kCullMinStock[3] = {0xE3500000u, 0x03A00000u, 0x12400001u};
const uint32_t kCullMinWide[3] = {0xE3500005u, 0xB3A00000u, 0xA2400005u};

// ---- the VERTICAL twin, which never existed --------------------------------
//
// Only the horizontal half of the gather rect was ever widened. Disassembling
// the rect's construction at 0x02038F80 shows the two axes built side by side:
//
//     0x02038FD0  add   r2, r2, #0x100     camera x + 256   <- widened already
//     0x02038FE4  add   r3, r3, #0xC0      camera y + 192   <- was not
//
// and the same for the near corner, four instructions apart:
//
//     0x02038F9C  cmp   r0, #0             \
//     0x02038FA0  moveq r1, #0              |  y_min, margin 1  <- was not
//     0x02038FA4  subne r1, r0, #1         /
//     0x02038FB0  cmp   r0, #0             \
//     0x02038FB4  moveq r0, #0              |  x_min, margin 1  <- widened
//     0x02038FB8  subne r0, r0, #1         /
//
// So however tall the surface grew, the game still gathered entities for the
// DS's own 192 rows, and anything above or below that band was never in the
// list to be drawn or hit-tested. The symptom: units near the base were not
// visible at some window sizes -- the base sits outside the native
// vertical band once the window is tall enough, and its units vanish with it.
//
// 0xC0 is 192, the screen height, exactly as 0x100 is 256, the screen width.
const uint32_t kCullHeightAddr = 0x02038FE4u;
const uint32_t kCullHeightStock = 0xE28330C0u;   // add r3, r3, #0xC0

// The near-corner block for the ROW axis, three instructions at 0x02038F9C.
// Same shape as kCullMinStock but writing r1 from r0, so the register fields
// differ: moveq r1,#0 is 03A01000 and subne r1,r0,#1 is 12401001.
const uint32_t kCullRowAddr = 0x02038F9Cu;
const uint32_t kCullRowStock[3] = {0xE3500000u, 0x03A01000u, 0x12401001u};

// ARM data-processing immediates are an 8-bit value rotated right by an even
// amount, so not every width is expressible. Return 0 when it is not.
static uint32_t arm_imm12(uint32_t v) {
    for (uint32_t rot = 0; rot < 16u; ++rot) {
        const uint32_t r = ((v << (2u * rot)) | (v >> (32u - 2u * rot))) &
                           0xFFFFFFFFu;
        if (r <= 0xFFu) return (rot << 8) | r;
    }
    return 0u;
}

// The smallest ARM immediate that is at least `v`, with the value it encodes.
//
// WHY ROUNDING UP IS NEEDED AT ALL. An ARM data-processing immediate is an
// 8-bit value rotated by an even amount, so above 255 it can only express
// multiples of 4, then 16, and so on. The gather rect's right edge is
// 256 + (surface - 256) / 2, which lands on whatever the window happens to
// make it: a 946-wide surface gives 601, and 601 is not representable.
//
// arm_imm12 returned 0 for it and the caller's `if (imm && ...)` then skipped
// the write IN SILENCE. That is the same shape of fault as the gate this file
// already documents: at the old 512-px ceiling the band was small enough that
// the value usually happened to be representable, and raising the ceiling made
// unrepresentable values the common case -- so units at the outer edge of a
// wide band would have stopped being gathered, on some window sizes and not
// others, with nothing in the log to say why.
//
// Rounding UP rather than down, because the rect is a bound: a few pixels too
// wide gathers a couple of entities that are then culled by the renderer, while
// a few too narrow drops entities the player can see.
static uint32_t arm_imm12_ceil(uint32_t v, uint32_t* encoded) {
    for (uint32_t add = 0; add < 4096u; ++add) {
        const uint32_t imm = arm_imm12(v + add);
        if (imm) {
            if (encoded) *encoded = v + add;
            return imm;
        }
    }
    return 0u;
}

// ── WHERE PANNING STOPS: the camera clamp's two view-size immediates ──────
//
// The requirement: rather than jitter/shake the view at the edge of the game
// world, do what the vanilla game does and not attempt to pan beyond it.
//
// func_0203906C is the game's own camera clamp, and it computes
//
//     max_x = (max_tile_x + 1) * 24 - 248      248 at 0x020390C4
//     max_y = (max_tile_y + 1) * 16 - 192      192 at 0x020390D4
//     min   = min_tile * (24 | 16)
//
// from a tile-space world rect at +0x150/+0x158. 248 and 192 are the VIEW SIZE,
// and they are bare ARM immediates. Widening them makes the guest stop the
// camera where the WIDENED picture meets the world edge, by its own arithmetic.
//
// WHY THIS IS ENOUGH ON ITS OWN. func_02039038 (SetCameraPos) does not clamp --
// it sets a dirty byte at +0x5C. func_0203942C, the per-frame update, sees that
// byte and runs func_020398B8, which puts the current camera back through
// func_0203906C and stores the result. So every path that moves the camera --
// stylus drag, edge auto-pan, d-pad, minimap click, centre-on-unit, scripted
// pans -- settles at whatever bound THIS function computes, at worst one frame
// later. Two instructions cover all of them.
//
// WHY NOT THE WORLD RECT. An earlier attempt moved the tile rect at
// +0x150/+0x158 instead and SHRANK THE PLAYABLE WORLD: that rect is the map's
// own extent, read by A* path plotting (func_02056704), the placement/order
// predicate func_0205C034 with its eleven callers, four Sim spell virtuals, the
// centre-on-unit gate and the minimap's drawable extent. It is world state. The
// view size is not: func_0203906C and func_0203CED4 are its only consumers.
//
// ROUNDING UP IS THE SAFE DIRECTION. A larger view size yields a SMALLER
// maximum, so the camera stops sooner -- a few pixels of world unreached rather
// than a few pixels of black past the edge, which is the defect being fixed.
//
// THE -8 IS THE CARTRIDGE'S OWN MARGIN, kept rather than removed: stock is 248
// for a 256-wide view because func_020396E0 offsets the background by
// `4 - (cam_x mod 8)`. At gw == 256 this reproduces 248 exactly, so stock
// geometry is untouched. Y has no such offset and uses the height as-is.
//
// func_0203CED4 is a SECOND clamp with its own immediates, used by the
// level-start centring and by the load-time bound setter. It is patched here
// too, or a freshly loaded level would clamp to the old width and disagree with
// func_0203906C for as long as the player stayed put.
const uint32_t kCamClampW     = 0x020390C4u;   // mov r2,#0xF8  (248)
const uint32_t kCamClampWStock = 0xE3A020F8u;
const uint32_t kCamClampH     = 0x020390D4u;   // mov r2,#0xC0  (192)
const uint32_t kCamClampHStock = 0xE3A020C0u;
const uint32_t kCamClampBW    = 0x0203CF18u;   // sub r2,r2,#0xE8 (256-24)
const uint32_t kCamClampBWStock = 0xE24220E8u;
const uint32_t kCamClampBH    = 0x0203CF38u;   // sub r3,r3,#0xB0 (192-16)
const uint32_t kCamClampBHStock = 0xE24330B0u;

// The world's pixel extent, as last read from the bound rect. 0 until a level
// is up.
//
// WHY THE FRONTEND NEEDS IT. At a 1700 px window and pixel scale 1 the guest
// surface is 1700 px wide against a 1536 px world, and the 164 px difference
// is drawn as BLACK BARS either side of the map -- the camera bumping into
// black. Capping the camera clamp alone cannot fix that: the clamp decides
// where the camera stops, not how much surface there is to fill.
//
// Flooring the pixel scale does fix it. At scale >= win_w / world_w the
// surface is at most the world's width, so there is no off-world margin to
// draw, and the camera keeps a valid range for free. That is also what an RTS
// zoom-out limit is: you can zoom out until the whole map is in shot, and no
// further.
static int g_world_w = 0, g_world_h = 0;

extern "C" int lb_world_extent(int* w, int* h) {
    if (w) *w = g_world_w;
    if (h) *h = g_world_h;
    return (g_world_w > 0 && g_world_h > 0) ? 1 : 0;
}

static void apply_camera_view_size(void) {
    if (!lb_setting_enhanced()) return;
    // THE IMMEDIATE PATCH IS NOW OFF BY DEFAULT (docs/345); LB_CAMVIEW=1
    // re-enables it. The world-extent publication below still runs -- the
    // frontend caps the surface with it -- only the rewrite of the
    // cartridge's clamp constants stops.
    //
    // Why, measured at a 2749x1234 window (surface 1570x704, world
    // 1536x1024), panning down against the map edge:
    //
    //   this patch on   camera stops at y 336 = world - SURFACE height; the
    //                   band, still centred there, leaves the window's bottom
    //                   at 776 -- 248 px of map unreachable
    //   this patch off, with the stock clamp and the band's own edge slide
    //                   (band_split): camera reaches 832 = world - 192, the
    //                   band slides to 512, the window's bottom is 1024 --
    //                   exactly the map's -- and nothing writes the camera
    //
    // The patch assumed the camera is the top-left of the WHOLE surface. The
    // band assumes it is the top-left of the guest's 192 rows and slides the
    // overhang at an edge. Both cannot be true; the band's convention is the
    // one every renderer, the HUD and the touch mapping already use.
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_CAMVIEW");
        on = (v && v[0] == '1') ? 1 : 0;
    }

    int fw = 0, fh = 0, fx = 0, fy = 0, gw = 256, gh = 192;
    if (!lb_overlay_field_rect(&fx, &fy, &fw, &fh, &gw, &gh) || gw < 256)
        gw = 256;
    if (gh < 192) gh = 192;
    // Kept so the log can say what the window asked for AND what was used; the
    // difference between the two is the whole point of the cap below.
    const int gw_raw = gw, gh_raw = gh;

    // ── A VIEW WIDER THAN THE WORLD HAS NO CAMERA RANGE ──────────────────
    //
    // The guest computes, in func_0203906C:
    //
    //     min_x = min_tile_x * 24      max_x = max_tile_x * 24 + 24 - view_w
    //
    // so max_x >= min_x only while view_w <= (max_tile_x - min_tile_x + 1)*24,
    // which is the world's own width. Past that the interval inverts. The
    // guest applies the max first and the min second, so the min wins and the
    // camera pins to the world's left edge with black to the right of it --
    // a camera bound cap that prevents further movement, and the
    // 1706 px surface against a 1536 px map that apply_camera_bounds() already
    // describes.
    //
    // Capping the VIEW SIZE at the world size keeps the interval valid: at
    // equality max == min, so the camera pins with the whole world in shot,
    // which is the correct answer to "there is nothing to pan to".
    //
    // READ ONLY. The bound rect at +0x150/+0x158 is the WORLD EXTENT -- the
    // clamp, pathfinding, the minimap and the AI all read it -- and writing it
    // to constrain the camera was a regression: it trimmed the world size
    // inward. A port may move the
    // camera; it must not rewrite simulation state to do it. Nothing below
    // writes.
    int world_w = 0, world_h = 0;          // 0 = the rect was not readable
    // Published for the frontend, which sizes the guest surface a frame before
    // this runs and so uses the value last read. See lb_world_extent().
    if (const uint32_t cam_obj = lb_field_camera_obj()) {
        const uint32_t lo = cam_obj + 0x150u, hi = cam_obj + 0x158u;
        const int min_tx = (int)bus_read_u8(lo + 4u);
        const int min_ty = (int)bus_read_u8(lo + 5u);
        const int max_tx = (int)bus_read_u8(hi + 4u);
        const int max_ty = (int)bus_read_u8(hi + 5u);
        if (max_tx >= min_tx && max_ty >= min_ty) {
            world_w = (max_tx - min_tx + 1) * 24;
            world_h = (max_ty - min_ty + 1) * 16;
            g_world_w = world_w;
            g_world_h = world_h;
            if (gw > world_w) gw = world_w;
            if (gh > world_h) gh = world_h;
        }
    }
    const int want_gw = gw, want_gh = gh;

    // Patch off: put back anything a previous setting wrote, so the game's own
    // clamp is exactly the ROM's. Only a site that still carries this patch's
    // own base encoding is touched, so a different build is left alone.
    if (!on) {
        static const struct { uint32_t addr, stock, base; } kSites[4] = {
            {kCamClampW,  kCamClampWStock,  0xE3A02000u},
            {kCamClampH,  kCamClampHStock,  0xE3A02000u},
            {kCamClampBW, kCamClampBWStock, 0xE2422000u},
            {kCamClampBH, kCamClampBHStock, 0xE2433000u},
        };
        for (const auto& s : kSites) {
            // NOT the two words docs/373 owns while it is live: 020390C4 and
            // 020390D4 then hold its min margins, which have this patch's
            // `mov r2, #imm` base, and restoring them made the two functions
            // rewrite the clamp against each other every frame.
            if ((s.addr == kCamClampW || s.addr == kCamClampH) &&
                lb_camera_edge_live())
                continue;
            const uint32_t cur = bus_read_u32(s.addr);
            if (cur != s.stock && (cur & 0xFFFFF000u) == s.base)
                bus_write_u32(s.addr, s.stock);
        }
        (void)want_gw;
        (void)want_gh;
        return;
    }

    struct Site { uint32_t addr, stock, base, want_val; };
    uint32_t enc[4] = {0, 0, 0, 0};
    const uint32_t imm[4] = {
        arm_imm12_ceil((uint32_t)gw - 8u, &enc[0]),
        arm_imm12_ceil((uint32_t)gh,      &enc[1]),
        arm_imm12_ceil((uint32_t)gw - 24u, &enc[2]),
        arm_imm12_ceil((uint32_t)gh - 16u, &enc[3]),
    };
    const Site site[4] = {
        {kCamClampW,  kCamClampWStock,  0xE3A02000u, imm[0]},
        {kCamClampH,  kCamClampHStock,  0xE3A02000u, imm[1]},
        {kCamClampBW, kCamClampBWStock, 0xE2422000u, imm[2]},
        {kCamClampBH, kCamClampBHStock, 0xE2433000u, imm[3]},
    };
    // SAY WHEN THE VIEW SIZE CHANGES, WHETHER OR NOT THE WRITE HAPPENS.
    //
    // The two returns below are both silent, and both leave the PREVIOUS
    // window's bounds in force. That is indistinguishable from a clamp that
    // does not update when the window is resized -- the defect -- so the
    // size that was asked for is logged before anything can decline it.
    //
    // Also logged: whether the view is wider or taller than the WORLD. The
    // guest computes max = (tiles+1)*cell - view, so once the view exceeds the
    // world the maximum goes negative and drops below the minimum, and the
    // clamp has no valid interval to settle the camera in. No choice of
    // immediate fixes that; it needs the camera pinned and panning disabled.
    {
        // Keyed on the size the WINDOW asked for, not the capped one: two
        // different window sizes that both cap to the world would otherwise
        // log once between them, and the resize being diagnosed is exactly
        // when those differ.
        // ALSO KEYED ON THE WORLD SIZE, and the first version was not.
        //
        // apply_camera_view_size runs from the first frame, long before a level
        // exists, so both log lines a run produced said "world 0x0px -- no
        // cap": lb_field_camera_obj() is 0 until lb_field_camera() has located
        // it. The cap itself is fine and starts working once the level is up;
        // it was the LOG that could never show it, because the surface size had
        // stopped changing by then. Reporting a world size transition as well
        // is what makes the cap observable at all.
        static int last_gw = -1, last_gh = -1, last_world = -1;
        if (gw_raw != last_gw || gh_raw != last_gh || world_w != last_world) {
            last_gw = gw_raw;
            last_gh = gh_raw;
            last_world = world_w;
            // enc[], not imm[]: arm_imm12_ceil returns the ENCODING in imm and
            // the value it encodes in enc. Logging imm printed "3966" for a
            // 504 px view, which reads as a wild number rather than as a
            // rotation and a mantissa.
            std::fprintf(stderr,
                         "[lb_camview] view %dx%d -> using %dx%d; "
                         "encodes to %ux%u; world %dx%dpx%s\n",
                         gw_raw, gh_raw, want_gw, want_gh, enc[0], enc[1],
                         world_w, world_h,
                         world_w <= 0 ? "  (world rect unreadable -- no cap)"
                         : (gw_raw > world_w || gh_raw > world_h)
                             ? "  VIEW EXCEEDED WORLD -- capped, camera pins"
                             : "");
            std::fflush(stderr);
        }
    }
    // ALL FOUR OR NONE. A half-applied pair makes the two clamps disagree, and
    // the camera would then settle differently depending on which one ran last
    // -- exactly the two-authorities shake this work exists to remove.
    for (int i = 0; i < 4; ++i) {
        if (!site[i].want_val) {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr,
                             "[lb_camview] DECLINED: value %d is not an ARM "
                             "imm12; the previous window's bounds stay in "
                             "force\n", i == 0 ? gw - 8 : i == 1 ? gh
                                        : i == 2 ? gw - 24 : gh - 16);
                std::fflush(stderr);
            }
            return;                                    // unencodable
        }
        const uint32_t cur = bus_read_u32(site[i].addr);
        const uint32_t want = site[i].base | site[i].want_val;
        if (cur != want && cur != site[i].stock &&
            (cur & 0xFFFFF000u) != site[i].base)
            return;                                    // not this build
    }
    bool wrote = false;
    for (int i = 0; i < 4; ++i) {
        const uint32_t want = site[i].base | site[i].want_val;
        if (bus_read_u32(site[i].addr) != want) {
            bus_write_u32(site[i].addr, want);          // self-healing
            wrote = true;
        }
    }
    if (wrote) {
        std::fprintf(stderr,
                     "[lb_camview] clamp view size -> %ux%u (surface %dx%d); "
                     "every camera path settles here\n",
                     enc[0], enc[1], gw, gh);
        std::fflush(stderr);
    }
}

// ── THE CAMERA STOPS WHERE THE PICTURE STOPS (docs/373) ────────────────────
//
// The camera could be slow to start moving, as if carrying momentum from the
// original aspect's location. That is exactly what happens.
//
// Since docs/345 the cartridge's own clamp is stock: func_0203906C lets the
// camera (the top-left of the guest's native 256x192) reach
//
//     max_x = (max_tile_x + 1) * 24 - 248    max_y = (max_tile_y + 1) * 16 - 192
//     min   = min_tile * (24 | 16)
//
// and at a map edge the band slides instead (lb_widemap band_split): the
// picture stops at the world's edge while the camera goes on to the stock
// limit. Between the camera position where the band starts to slide and that
// limit, the camera moves and the picture does not -- (span)/2 px, 240 at a
// 736-wide surface. Panning back out of an edge first has to cross that zone
// with nothing on screen moving: "momentum from the original aspect's
// location".
//
// So the camera now stops where the band would start to slide, by the
// cartridge's own arithmetic. band_compute_at keeps the band centred while
//
//     camx - 4 >= span/2     and     map - (camx + 256) + 4 >= span - span/2
//     camy     >= span_y/2   and     map - (camy + 192)     >= span_y - span_y/2
//
// (the 4 is the BGxOFS bias it already allows for). The clamp's max side is
// a view-size subtraction, so a larger view size moves it. Its min side has no
// constant at all (min_tile * 24), so the two helper calls of each axis are
// rearranged. The stock sequence per axis is
//
//     0203909C..A8  FUN_0200E634(sp+0xC, &max.x, 24)    max.x += 24
//     020390BC..C8  FUN_02039134(sp+4,  &max.x, 248)    max.x -= 248
//
// and becomes one subtraction and one addition:
//
//     020390A4/A8   FUN_02039134(.., &max.x, V - 24)    max.x += 24 - V
//     020390C0..C8  FUN_0200E634(.., &min.x, c)         min.x += c
//
// with V = 252 + (span - span/2) and c = span/2 + 4 (y: V = 192 + (span_y -
// span_y/2), c = span_y/2). Both helpers are the cartridge's own fx32
// add/subtract; the first argument only receives a copy of the result, in a
// scratch slot nothing reads again. Immediates that ARM cannot encode round
// toward stopping SOONER -- a few pixels of world unreached, never black.
//
// Only the camera's range changes; the world rect (+0x150/+0x158), which the
// simulation reads, is never written -- the docs/281 lesson. func_0203CED4,
// the level-start clamp, is left stock: every later move goes through this
// function, and a start position in the old range only means the first move
// lands on the new bound, where the picture is the same.
//
// Enhanced, a widened surface, not with LB_CAMVIEW=1 (docs/345's older patch
// owns 020390C4); LB_CAMEDGE=0 turns it off alone; stock words back for a
// native view. A demo recorded without it replays with the stock range, since
// its taps were aimed at the camera it had (lb_demo.cpp, camedge=).
namespace {
struct CamEdgeSite { uint32_t addr, stock; };
constexpr CamEdgeSite kCamEdge[10] = {
    {0x020390A4u, 0xE3A02018u},   // mov r2, #24          -> #(Vx - 24)
    {0x020390A8u, 0xEBFF5561u},   // bl  FUN_0200E634     -> FUN_02039134
    {0x020390B4u, 0xE3A02010u},   // mov r2, #16          -> #(Vy - 16)
    {0x020390B8u, 0xEBFF555Du},   // bl  FUN_0200E634     -> FUN_02039134
    {0x020390C0u, 0xE28D1014u},   // add r1, sp, #0x14    -> #0x20 (min.x)
    {0x020390C4u, 0xE3A020F8u},   // mov r2, #248         -> #cx
    {0x020390C8u, 0xEB000019u},   // bl  FUN_02039134     -> FUN_0200E634
    {0x020390D0u, 0xE28D1018u},   // add r1, sp, #0x18    -> #0x24 (min.y)
    {0x020390D4u, 0xE3A020C0u},   // mov r2, #192         -> #cy
    {0x020390D8u, 0xEB000015u},   // bl  FUN_02039134     -> FUN_0200E634
};
constexpr uint32_t kCamEdgeCall[4] = {
    0xEB000021u,   // 020390A8: bl 02039134
    0xEB00001Du,   // 020390B8: bl 02039134
    0xEBFF5559u,   // 020390C8: bl 0200E634
    0xEBFF5555u,   // 020390D8: bl 0200E634
};
}  // namespace

extern "C" int lb_camera_edge_live(void) {
    if (!lb_setting_enhanced()) return 0;
    static int env = -1, camview = -1;
    if (env < 0) {
        const char* v = std::getenv("LB_CAMEDGE");
        env = (v && v[0] == '0') ? 0 : 1;
        const char* c = std::getenv("LB_CAMVIEW");
        camview = (c && c[0] == '1') ? 1 : 0;
    }
    if (!env || camview) return 0;
    return lb_demo_play_camedge() != 0 ? 1 : 0;
}

// The encodable immediate nearest `v` on the side that stops the camera
// sooner: up for both (a larger V lowers the max, a larger c raises the min).
static uint32_t cam_edge_imm(uint32_t v, uint32_t* used) {
    if (v == 0u) {                 // arm_imm12 answers 0 for "unencodable"
        *used = 0u;
        return 0u;                 // mov r2, #0
    }
    return arm_imm12_ceil(v, used);
}

// docs/393: AND ON THE CARTRIDGE'S CAMERA GRID. Build previews' checkmarks
// still did not align the way they do in vanilla. The cartridge keeps its camera on an 8-px grid: the d-pad steps
// it by 8 (FUN_020ABA30), a minimap tap masks it with 0xFFF8 (literal
// 0x0203CBA4), and every stock bound is a multiple of 8 (min*24, min*16,
// max*24 - 232/248, max*16 - 176/192). Its layers depend on that:
// View_SetupLevelBGs scrolls BG3, the placement footprint, ONCE to (4, 0),
// while Task_View_CommitBGs scrolls the terrain to (4 - camx%8, camy%16) --
// equal only on the grid. Off it, the footprint sits up to 7 px off the
// terrain, the checkmark (placed at the true pixel, func_02036FBC) off the
// footprint, and SpriteUI_MarkCells' (x-4)>>3 cell mask clears a cell beside
// the checkmark and leaves BG3 (priority 0) over its edge. The widened bounds
// above were span/2 + 4 and 252 + (span - span/2): min camx 244 at 2.0, 203
// at 2.25 (runs t393d/l), where a battle that starts against the map's edge
// rests. So each immediate is also rounded UP to a multiple of 8, stopping
// the camera at most 7 px sooner, never into black.
static uint32_t cam_edge_imm_grid(uint32_t v, uint32_t* used) {
    uint32_t a = (v + 7u) & ~7u;
    for (int guard = 0; guard < 64; ++guard) {
        const uint32_t enc = cam_edge_imm(a, used);
        if ((*used & 7u) == 0u) return enc;
        a = (*used + 8u) & ~7u;
    }
    return cam_edge_imm(v, used);   // unreachable for the values used here
}
extern "C" int lb_demo_play_camgrid(void);   /* lb_demo.cpp */

static void apply_camera_edge(void) {
    const int gw = (int)nds_gpu3d_output_width();
    const int gh = (int)nds_gpu3d_output_height();
    const int span = gw > 256 ? gw - 256 : 0;
    const int span_y = gh > 192 ? gh - 192 : 0;
    const bool want = lb_camera_edge_live() && (span > 0 || span_y > 0);
    uint32_t word[10];
    uint32_t vx = 248, vy = 192, cx = 0, cy = 0;
    if (want) {
        // An axis with no overhang keeps the stock range exactly: V = 248
        // (256 minus the cartridge's own 8 px) and no margin.
        // docs/393: on the 8-px grid, unless a replay was recorded before it.
        // V - 24 and V - 16 are what is encoded, and both offsets are
        // multiples of 8, so rounding the encoded value keeps V on the grid.
        auto imm = lb_demo_play_camgrid() != 0 ? cam_edge_imm_grid : cam_edge_imm;
        uint32_t used = 0;
        const uint32_t ax = imm(
            span > 0 ? (uint32_t)(252 + (span - span / 2) - 24) : 224u, &used);
        vx = used + 24;
        const uint32_t ay = imm(
            (uint32_t)(192 + (span_y - span_y / 2) - 16), &used);
        vy = used + 16;
        const uint32_t bx = imm(
            span > 0 ? (uint32_t)(span / 2 + 4) : 0u, &used);
        cx = used;
        const uint32_t by = imm((uint32_t)(span_y / 2), &used);
        cy = used;
        word[0] = 0xE3A02000u | ax;
        word[1] = kCamEdgeCall[0];
        word[2] = 0xE3A02000u | ay;
        word[3] = kCamEdgeCall[1];
        word[4] = 0xE28D1020u;
        word[5] = 0xE3A02000u | bx;
        word[6] = kCamEdgeCall[2];
        word[7] = 0xE28D1024u;
        word[8] = 0xE3A02000u | by;
        word[9] = kCamEdgeCall[3];
    } else {
        for (int i = 0; i < 10; ++i) word[i] = kCamEdge[i].stock;
    }
    // ALL TEN OR NONE: each site must hold the stock word or this function's
    // own encoding for it, which also waits out the boot window.
    for (int i = 0; i < 10; ++i) {
        const uint32_t cur = bus_read_u32(kCamEdge[i].addr);
        bool ok = cur == kCamEdge[i].stock;
        switch (i) {
            case 0: case 2: case 5: case 8:
                ok = ok || (cur & 0xFFFFF000u) == 0xE3A02000u; break;
            case 1: ok = ok || cur == kCamEdgeCall[0]; break;
            case 3: ok = ok || cur == kCamEdgeCall[1]; break;
            case 4: ok = ok || cur == 0xE28D1020u; break;
            case 6: ok = ok || cur == kCamEdgeCall[2]; break;
            case 7: ok = ok || cur == 0xE28D1024u; break;
            default: ok = ok || cur == kCamEdgeCall[3]; break;
        }
        if (!ok) return;          // not this build, or not in RAM yet
    }
    bool wrote = false;
    for (int i = 0; i < 10; ++i) {
        if (bus_read_u32(kCamEdge[i].addr) == word[i]) continue;
        bus_write_u32(kCamEdge[i].addr, word[i]);
        wrote = true;
    }
    if (wrote) {
        if (want)
            std::fprintf(stderr, "[lb_camedge] the camera stops where the "
                         "picture does: view size %ux%u, min + %u,%u "
                         "(surface %dx%d)\n", vx, vy, cx, cy, gw, gh);
        else
            std::fprintf(stderr, "[lb_camedge] stock camera range\n");
        std::fflush(stderr);
    }
    // LB_CAMEDGELOG=1: the band's camera, the band, and the picture's own
    // top-left in the world (camera minus band) -- what the player sees move --
    // whenever any of them changes, stamped with the VBlank count.
    static const bool log = std::getenv("LB_CAMEDGELOG") != nullptr;
    if (log) {
        int camx = 0, camy = 0;
        if (lb_widemap_cam_px(&camx, &camy)) {
            const int bx = lb_band_render(), by = lb_band_render_y();
            static int last[4] = {INT32_MIN, 0, 0, 0};
            if (last[0] != camx || last[1] != camy || last[2] != bx ||
                last[3] != by) {
                last[0] = camx; last[1] = camy; last[2] = bx; last[3] = by;
                std::fprintf(stderr, "[lb_camedgelog] vb %u cam %d,%d band "
                             "%d,%d picture %d,%d\n",
                             (unsigned)bus_read_u32(kVBlankCounter), camx, camy,
                             bx, by, camx - bx, camy - by);
            }
        }
    }
}

// LB_CULLAB=1 (docs/357): WHAT DRAWING THE BANDS COSTS THE GUEST.
//
// Alternates the widened gather rectangle and the cartridge's own every 300
// VBlanks, and reports the ARM9 instructions retired per VBlank under each.
// Guest instructions, not host time: the widened rectangle is a code patch,
// so its function runs in the Tier-3 interpreter while the stock one runs
// lifted, and host time would measure that difference too. The first 10
// VBlanks after each switch are left out. A measurement mode: in the stock
// periods the bands draw no units.
extern "C" uint64_t g_insn_count[2];   // src/io.h
bool cull_ab_on() {
    static const bool on = std::getenv("LB_CULLAB") != nullptr;
    return on;
}
bool cull_ab_stock_period() {
    return cull_ab_on() && ((bus_read_u32(kVBlankCounter) / 300u) & 1u);
}
void restore_cull_stock() {
    if (bus_read_u32(kCullImmAddr) != kCullStock)
        bus_write_u32(kCullImmAddr, kCullStock);
    if (bus_read_u32(kCullHeightAddr) != kCullHeightStock)
        bus_write_u32(kCullHeightAddr, kCullHeightStock);
    for (uint32_t i = 0; i < 3u; ++i) {
        if (bus_read_u32(kCullRowAddr + i * 4u) != kCullRowStock[i])
            bus_write_u32(kCullRowAddr + i * 4u, kCullRowStock[i]);
        if (bus_read_u32(kCullMinAddr + i * 4u) != kCullMinStock[i])
            bus_write_u32(kCullMinAddr + i * 4u, kCullMinStock[i]);
    }
}
void cull_ab_account() {
    if (!cull_ab_on()) return;
    static uint64_t last_insn = 0;
    static uint32_t last_vb = 0;
    static double sum[2] = {0.0, 0.0};
    static uint64_t n[2] = {0, 0};
    const uint32_t vb = bus_read_u32(kVBlankCounter);
    const uint64_t insn = g_insn_count[0];
    if (last_vb != 0u && vb == last_vb + 1u) {
        // The VBlank just finished ran under the mode set at the one before.
        const uint32_t prev = vb - 1u;
        const int stock = static_cast<int>((prev / 300u) & 1u);
        if (prev % 300u >= 10u) {
            sum[stock] += static_cast<double>(insn - last_insn);
            ++n[stock];
        }
    }
    last_vb = vb;
    last_insn = insn;
    if (vb % 1200u == 0u && n[0] && n[1]) {
        std::fprintf(stderr,
                     "[lb_cullab] ARM9 instructions per VBlank: widened "
                     "gather %.0f, stock gather %.0f (%.1f%% more), over "
                     "%llu and %llu VBlanks\n",
                     sum[0] / n[0], sum[1] / n[1],
                     100.0 * (sum[0] / n[0] - sum[1] / n[1]) / (sum[1] / n[1]),
                     (unsigned long long)n[0], (unsigned long long)n[1]);
        std::fflush(stderr);
        sum[0] = sum[1] = 0.0;
        n[0] = n[1] = 0;
    }
}

static void apply_cull_width(void) {
    cull_ab_account();
    // THE GATE WAS WRONG, AND IT SILENTLY TURNED THIS PATCH OFF.
    //
    // It read `if (!std::getenv("LB_WIDEMAP")) return;`, described as "same
    // switch as the widening". It stopped being the same switch: LB_WIDEMAP now
    // DEFAULTS ON when the variable is unset (lb_widemap.cpp's stage()), so
    // every ordinary session -- and every probe run -- left the
    // variable unset, took this early return, and ran with the cartridge's
    // stock 256-px gather rect.
    //
    // The consequence is exactly the symptom that had been under
    // investigation: entities outside the native 256 columns are never
    // GATHERED, so they cannot be drawn there and cannot be hit-tested there.
    // No amount of correctly delivering a touch coordinate into the widened
    // band can select a unit the game never put in its list.
    //
    // The gate now matches what actually decides the widening.
    if (!lb_setting_enhanced()) return;
    if (const char* v = std::getenv("LB_WIDEMAP"))
        if (v[0] == '0') return;
    // LB_CULLWIDE=0 turns off THIS patch and nothing else.
    //
    // A switch for one patch exists so it can be A/B'd against itself. When the
    // gate above was fixed, the in-band click control fell from 3 of 3 to 0 of
    // 3, and there was no way to tell whether widening the gather rect had
    // broken selection or whether the control had simply gone unstable again --
    // LB_WIDEMAP=0 could not answer it, because that turns off the whole
    // widening and changes the surface the probe measures against.
    if (const char* v = std::getenv("LB_CULLWIDE"))
        if (v[0] == '0') return;
    if (cull_ab_stock_period()) {
        restore_cull_stock();
        return;
    }

    // THE MARGINS COME FROM THE SURFACE, not from a constant.
    //
    // They were 352 and five metatile columns, both derived from a 448-wide
    // surface with 96 px either side of the guest's own 256. The surface has
    // been adaptive since docs/215 -- a 1600x900 window gives 392x212, which is
    // 68 px a side -- so the constants were describing a width the port no
    // longer uses. Too small and units at the outer edge of the band are culled
    // again; too large and the game walks a wider rect than it needs every
    // frame.
    int fw = 0, fh = 0, fx = 0, fy = 0, gw = 256, gh = 192;
    if (!lb_overlay_field_rect(&fx, &fy, &fw, &fh, &gw, &gh) || gw < 256)
        gw = 256;
    // THE WHOLE OVERHANG ON EACH SIDE, not half of it.
    //
    // This block rewrites GUEST CODE, and it compares what is there against
    // what it wants every frame so a reloaded overlay cannot silently undo it.
    // Sizing it to the band's CURRENT left half would therefore make it rewrite
    // three instructions every time the camera moves a pixel near a map edge,
    // where lb_band_left slides from half the overhang to none of it -- guest
    // code being patched at pan speed, and the log line printing with it.
    //
    // The gather rect only has to be a SUPERSET: an entity gathered and then
    // off screen is not drawn, and costs a walk of the list. So it is sized for
    // the worst case the band can reach, which is all of the overhang on one
    // side, and is then a constant for a given surface.
    const uint32_t band = (uint32_t)lb_band_span(gw);

    // RIGHT edge, in pixels past the camera; the stock +1 metatile margin past
    // it is kept as it is.
    const uint32_t right = 256u + band;
    // WRITTEN WHENEVER THE CODE IS NOT ALREADY WHAT IT SHOULD BE.
    //
    // Not once, and not only when the band changes. The overlay carrying this
    // code is reloaded between battles, which puts the stock encoding back, and
    // a one-shot patch would then be silently absent for the rest of the
    // session -- the same shape of fault as the gate above, and just as hard to
    // notice. Comparing against the desired encoding every frame is a couple of
    // reads, is self-healing, and only prints when it actually changes
    // something.
    uint32_t right_enc = right;
    const uint32_t imm = arm_imm12_ceil(right, &right_enc);
    const uint32_t want_right = 0xE2822000u | imm;
    const uint32_t cur = bus_read_u32(kCullImmAddr);
    if (imm && cur != want_right &&
        (cur == kCullStock || (cur & 0xFFFFF000u) == 0xE2822000u)) {
        bus_write_u32(kCullImmAddr, want_right);
        std::fprintf(stderr,
                     "[lb_cull] gather rect right edge camx+%u -> camx+%u px "
                     "(band %u, wanted %u)\n", 256u, right_enc, band, right);
        std::fflush(stderr);
    }
    // ---- the vertical axis, both edges ------------------------------------
    //
    // These sit BEFORE the left-edge block below, because that block ends in
    // `if (already) return;` -- anything added after it stops running the
    // moment the left edge is correct, which is most frames.
    if (gh < 192) gh = 192;
    // The whole vertical overhang, for the same reason as the horizontal one
    // above: the clamp can put all of it above the guest's rows at the map's
    // south edge, or all below at its north edge, and this rect is patched into
    // guest code and must not be rewritten as the camera moves.
    const uint32_t vband = (uint32_t)lb_band_span_y(gh);

    // BOTTOM edge, in pixels past the camera. The exact twin of the right edge.
    const uint32_t bottom = 192u + vband;
    uint32_t bottom_enc = bottom;
    const uint32_t vimm = arm_imm12_ceil(bottom, &bottom_enc);
    const uint32_t want_bottom = 0xE2833000u | vimm;
    const uint32_t vcur = bus_read_u32(kCullHeightAddr);
    if (vimm && vcur != want_bottom &&
        (vcur == kCullHeightStock || (vcur & 0xFFFFF000u) == 0xE2833000u)) {
        bus_write_u32(kCullHeightAddr, want_bottom);
        std::fprintf(stderr,
                     "[lb_cull] gather rect bottom edge camy+%u -> camy+%u px "
                     "(vband %u, wanted %u)\n", 192u, bottom_enc, vband, bottom);
        std::fflush(stderr);
    }

    // TOP edge, in whole metatile ROWS. A metatile is 3x2 tiles, so 16 px tall
    // against the 24 px the column axis uses -- the two margins are NOT the
    // same number for the same band.
    const uint32_t rows = vband ? ((vband + 15u) / 16u) + 1u : 1u;
    const uint32_t row_imm = arm_imm12(rows);
    const uint32_t rwide[3] = {0xE3500000u | row_imm,     // cmp   r0, #rows
                               0xB3A01000u,               // movlt r1, #0
                               0xA2401000u | row_imm};    // subge r1, r0, #rows
    bool rstock = true, rours = true, ralready = true;
    for (int i = 0; i < 3; ++i) {
        const uint32_t w = bus_read_u32(kCullRowAddr + (uint32_t)i * 4u);
        if (w != kCullRowStock[i]) rstock = false;
        if (w != rwide[i]) ralready = false;
    }
    rours = bus_read_u32(kCullRowAddr + 4u) == 0xB3A01000u &&
            (bus_read_u32(kCullRowAddr) & 0xFFFFF000u) == 0xE3500000u &&
            (bus_read_u32(kCullRowAddr + 8u) & 0xFFFFF000u) == 0xA2401000u;
    if (row_imm && !ralready && (rstock || rours)) {
        for (int i = 0; i < 3; ++i)
            bus_write_u32(kCullRowAddr + (uint32_t)i * 4u, rwide[i]);
        std::fprintf(stderr,
                     "[lb_cull] gather rect top edge camy-16 -> camy-%u px "
                     "(1 -> %u metatile rows)\n", rows * 16u, rows);
        std::fflush(stderr);
    } else if (!ralready) {
        // SAY SO, for the same reason the left edge does. The first attempt at
        // this patch printed only on success, so when it declined the run
        // showed a widened bottom edge and nothing about the top -- which reads
        // exactly like a patch that worked. Print what is actually there once,
        // so a mismatch cannot be mistaken for success.
        // NOT ON THE FIRST ATTEMPT. This runs from boot, before the ARM9
        // image is in RAM, and the first reads come back as whatever was
        // there -- AC4BE287 6502EBFE F12D0420 in one measured run, which
        // matches neither the stock block nor ours. The patch then succeeds
        // normally a moment later at the real surface size, in the SAME log:
        //
        //   [lb_cull] TOP EDGE NOT PATCHED: ... at 02038F9C
        //   [lb_cull] gather rect top edge camy-16 -> camy-400 px
        //
        // but the warning is one-shot and never retracts, so the log reads as
        // a permanent failure to patch the culling. That cost a full
        // investigation on 2026-09-20. Warn only after the patch has DECLINED
        // 60 times in a row, which a boot-order miss never reaches and a real
        // mismatch passes immediately.
        static unsigned rfail = 0u;
        static bool rsaid = false;
        if (++rfail >= 60u && !rsaid) {
            rsaid = true;
            std::fprintf(stderr,
                         "[lb_cull] TOP EDGE NOT PATCHED: %08X %08X %08X at "
                         "%08X (wanted %08X %08X %08X, rows %u)\n",
                         bus_read_u32(kCullRowAddr),
                         bus_read_u32(kCullRowAddr + 4u),
                         bus_read_u32(kCullRowAddr + 8u), kCullRowAddr,
                         rwide[0], rwide[1], rwide[2], rows);
            std::fflush(stderr);
        }
    }

    // LEFT edge, in whole metatile columns, since that is the unit this rect is
    // built in. 24 px per column, rounded UP so the outermost band column is
    // covered, and at least the stock 1.
    const uint32_t cols = band ? ((band + 23u) / 24u) + 1u : 1u;
    const uint32_t left_imm = arm_imm12(cols);
    const uint32_t wide[3] = {0xE3500000u | left_imm,      // cmp   r0, #cols
                              0xB3A00000u,                 // movlt r0, #0
                              0xA2400000u | left_imm};     // subge r0, r0, #cols
    bool stock = true;
    for (int i = 0; i < 3; ++i)
        if (bus_read_u32(kCullMinAddr + (uint32_t)i * 4u) != kCullMinStock[i])
            stock = false;
    // Same reasoning as the right edge: a block this function wrote before is
    // as good a starting point as the stock one.
    bool ours = bus_read_u32(kCullMinAddr + 4u) == 0xB3A00000u &&
                (bus_read_u32(kCullMinAddr) & 0xFFFFF000u) == 0xE3500000u &&
                (bus_read_u32(kCullMinAddr + 8u) & 0xFFFFF000u) == 0xA2400000u;
    bool already = true;
    for (int i = 0; i < 3; ++i)
        if (bus_read_u32(kCullMinAddr + (uint32_t)i * 4u) != wide[i])
            already = false;
    if (already) return;                       // nothing to do, say nothing
    if (left_imm && (stock || ours)) {
        for (int i = 0; i < 3; ++i)
            bus_write_u32(kCullMinAddr + (uint32_t)i * 4u, wide[i]);
        std::fprintf(stderr,
                     "[lb_cull] gather rect left edge camx-24 -> camx-%u px "
                     "(1 -> %u metatile columns)\n", cols * 24u, cols);
        std::fflush(stderr);
    } else {
        // SAY SO. A guard that declines in silence is indistinguishable from a
        // guard that fired, and that is exactly how a mistyped condition nibble
        // survived a build, a run and a write-up: the right edge printed its
        // message, so the rect looked patched while its left half never moved.
        // Printed once, and only when the words do not match, so a normal run
        // is unaffected and a mismatched one cannot be mistaken for success.
        // NOT ON THE FIRST ATTEMPT. This runs from boot, before the ARM9
        // image is in RAM, and the first reads come back as whatever was
        // there -- AC4BE287 6502EBFE F12D0420 in one measured run, which
        // matches neither the stock block nor ours. The patch then succeeds
        // normally a moment later at the real surface size, in the SAME log:
        //
        //   [lb_cull] TOP EDGE NOT PATCHED: ... at 02038F9C
        //   [lb_cull] gather rect top edge camy-16 -> camy-400 px
        //
        // but the warning is one-shot and never retracts, so the log reads as
        // a permanent failure to patch the culling. That cost a full
        // investigation on 2026-09-20. Warn only after the patch has DECLINED
        // 60 times in a row, which a boot-order miss never reaches and a real
        // mismatch passes immediately.
        static unsigned lfail = 0u;
        static bool said = false;
        if (++lfail >= 60u && !said) {
            said = true;
            std::fprintf(stderr,
                         "[lb_cull] LEFT EDGE NOT PATCHED: %08X %08X %08X at "
                         "%08X is not the stock block\n",
                         bus_read_u32(kCullMinAddr),
                         bus_read_u32(kCullMinAddr + 4u),
                         bus_read_u32(kCullMinAddr + 8u), kCullMinAddr);
            std::fflush(stderr);
        }
    }
}

// ---- EFFECTS AND PARTICLES: the cartridge's own screen cull (docs/350) -----
//
// The gather rect above decides which ENTITIES are drawn. Effects and
// particle systems never pass through it. Each one is tested against the DS
// screen by one of two helpers, in screen pixels after the camera has been
// subtracted, and a failed test returns before anything is submitted -- so in
// Enhanced an explosion, a spell, a construction effect or a particle system
// whose anchor lies in the left, right or bottom band is simply never drawn,
// while the unit it belongs to is. Disassembled from the ROM:
//
//   func_02016070, one anchor point. vtable slot 5 (the draw) of eighteen
//   Effect classes calls it and returns on 0: ArrowHighLight, ArrowVolley,
//   Artillery, ClusterBomb, CoconutStorm, CrabSwarm, DrawBox, EAttack,
//   FireBall, ForrestSpawn, Freeze, GenericExplosion, LightningBolt,
//   MonkeySwarm, Roar, SpaceLaser, ThunderHammer, Upgrade.
//
//     020160CC  cmp r5, #0       blt  reject      x >= 0
//     020160DC  cmp r5, #0x100   ble  accept      x <= 256
//     020160F0  cmp r0, #0xC0    movgt r0, #0     y <= 192   (no top bound)
//
//   func_02016110, a rect: accepted if any of its four corners is on the
//   screen, or if an edge spans it. The draw of BuildConstruction,
//   BridgeConstruction, HeroStartSpell, HealthBoostTrailStar and all ten
//   SpellIcons classes calls it, and so do the PARTICLE SYSTEMS on the list at
//   *(0x02151D14)+0x54: their draw 0202A3EC (render task 0x1F), and their
//   update 0202A058, which returns before EMITTING new particles when the
//   system's rect fails -- so a band system on the stock code neither draws
//   nor emits (docs/350; the one place this changes more than pixels). 32
//   compares against #0, #0x100 and #0xC0; x lives in r1/r3/r4/r5 (corner x,
//   each loaded by the ldrsh just before) and y in r0/r2.
//
// docs/211 concluded "there is no separate cull for effects" from a scan for a
// camera-x load PAIRED WITH A VIEW-WIDTH ADD, which is the shape of the gather
// rect. These tests have no such add -- they compare the already-subtracted
// screen position with literal 0, 256 and 192 -- so that scan could not see
// them. The eight effect classes with neither call (Flash, Jiggle, Teleport,
// Victory, and EarthQuake/HotWire/Tracking whose draw is a bare return) have no
// screen-bound compare at all.
//
// THE FIX IS THE CARTRIDGE'S OWN TEST ON A BIGGER SCREEN. Every bound becomes
// the widened surface's, sized for the whole overhang on each side, for the
// reason apply_cull_width gives: the band's split slides at map edges and guest
// code must not be rewritten as the camera moves. A test that is a superset is
// safe -- an effect accepted a little off the surface is clipped by the 3D
// engine exactly as the cartridge relies on at its own screen edge.
//
//   lower bound   cmp rN, #0       ->  cmn rN, #span    flags of rN + span, so
//                                                       the same lt/ge now
//                                                       means rN < -span
//   upper bound   cmp rN, #256/192 ->  cmp rN, #256+span / #192+span
//
// rounded UP to an ARM immediate. Every compare of one kind gets the same
// value, which keeps func_02016110's spanning tests consistent with its corner
// tests. What an accepted effect then does is unchanged: the draws submit it at
// that screen position, which the widened 3D renders where units already go.
namespace {
enum LbFxBound : uint8_t { kFxXLo, kFxXHi, kFxYLo, kFxYHi };
struct LbFxCmp {
    uint32_t addr;
    uint32_t stock;
    uint8_t kind;
};
const LbFxCmp kFxCull[] = {
    // func_02016070, the anchor point
    {0x020160CCu, 0xE3550000u, kFxXLo}, {0x020160DCu, 0xE3550C01u, kFxXHi},
    {0x020160F0u, 0xE35000C0u, kFxYHi},
    // func_02016110, the four corners: p0 = r1,r2  p1 = r3,r0  p2 = r4,r0
    // p3 = r5,r0
    {0x0201620Cu, 0xE3510000u, kFxXLo}, {0x02016218u, 0xE3510C01u, kFxXHi},
    {0x02016220u, 0xE3520000u, kFxYLo}, {0x02016228u, 0xE35200C0u, kFxYHi},
    {0x0201623Cu, 0xE3530000u, kFxXLo}, {0x02016244u, 0xE3530C01u, kFxXHi},
    {0x02016250u, 0xE3500000u, kFxYLo}, {0x02016258u, 0xE35000C0u, kFxYHi},
    {0x0201626Cu, 0xE3540000u, kFxXLo}, {0x02016274u, 0xE3540C01u, kFxXHi},
    {0x02016280u, 0xE3500000u, kFxYLo}, {0x02016288u, 0xE35000C0u, kFxYHi},
    {0x0201629Cu, 0xE3550000u, kFxXLo}, {0x020162A4u, 0xE3550C01u, kFxXHi},
    {0x020162B0u, 0xE3500000u, kFxYLo}, {0x020162B8u, 0xE35000C0u, kFxYHi},
    // ... and the edges that span the screen with no corner on it
    {0x020162C8u, 0xE3510000u, kFxXLo}, {0x020162D0u, 0xE3530C01u, kFxXHi},
    {0x020162D8u, 0xE35200C0u, kFxYHi}, {0x020162E4u, 0xE35000C0u, kFxYHi},
    {0x020162F4u, 0xE3550000u, kFxXLo}, {0x020162FCu, 0xE3540C01u, kFxXHi},
    {0x02016308u, 0xE35000C0u, kFxYHi}, {0x02016314u, 0xE35000C0u, kFxYHi},
    {0x02016324u, 0xE3510000u, kFxXLo}, {0x0201632Cu, 0xE3550C01u, kFxXHi},
    {0x02016334u, 0xE35200C0u, kFxYHi}, {0x02016340u, 0xE35000C0u, kFxYHi},
    {0x02016350u, 0xE3530000u, kFxXLo}, {0x02016358u, 0xE3540C01u, kFxXHi},
    {0x02016364u, 0xE35000C0u, kFxYHi}, {0x02016370u, 0xE35000C0u, kFxYHi},
};
constexpr unsigned kFxCullCount = sizeof kFxCull / sizeof kFxCull[0];
static_assert(kFxCullCount == 35u, "3 point compares + 32 rect compares");
}  // namespace

static void apply_fx_cull(void) {
    // The same gates as the gather rect, plus LB_FXCULL=0 to A/B this patch on
    // its own. Off means STOCK: a word this function widened earlier in the
    // session is put back, so switching to Vanilla restores the cartridge's
    // test rather than leaving a wide one behind.
    bool on = lb_setting_enhanced();
    if (const char* v = std::getenv("LB_WIDEMAP"))
        if (v[0] == '0') on = false;
    static const bool lb_fxcull_off = [] {
        const char* v = std::getenv("LB_FXCULL");
        return v && v[0] == '0';
    }();
    if (lb_fxcull_off) on = false;

    int fw = 0, fh = 0, fx = 0, fy = 0, gw = 256, gh = 192;
    if (!lb_overlay_field_rect(&fx, &fy, &fw, &fh, &gw, &gh) || gw < 256)
        gw = 256;
    if (gh < 192) gh = 192;
    const uint32_t span = on ? (uint32_t)lb_band_span(gw) : 0u;
    const uint32_t vspan = on ? (uint32_t)lb_band_span_y(gh) : 0u;

    // The four immediates, each rounded UP so every test stays a superset. A
    // zero span keeps the stock form (cmp rN, #0), which is also what "off"
    // writes back.
    uint32_t xlo_v = 0u, xhi_v = 256u, ylo_v = 0u, yhi_v = 192u;
    const uint32_t xlo = span ? arm_imm12_ceil(span, &xlo_v) : 0u;
    const uint32_t xhi = arm_imm12_ceil(256u + span, &xhi_v);
    const uint32_t ylo = vspan ? arm_imm12_ceil(vspan, &ylo_v) : 0u;
    const uint32_t yhi = arm_imm12_ceil(192u + vspan, &yhi_v);
    if ((span && !xlo) || !xhi || (vspan && !ylo) || !yhi) return;

    unsigned written = 0u, declined = 0u;
    for (unsigned i = 0; i < kFxCullCount; ++i) {
        const LbFxCmp& c = kFxCull[i];
        const uint32_t rn = c.stock & 0x000F0000u;
        const bool lower = c.kind == kFxXLo || c.kind == kFxYLo;
        const uint32_t lo_span = c.kind == kFxXLo ? span : vspan;
        const uint32_t lo_imm = c.kind == kFxXLo ? xlo : ylo;
        uint32_t want;
        if (lower)
            want = lo_span ? (0xE3700000u | rn | lo_imm)   // cmn rN, #span
                           : c.stock;                      // cmp rN, #0
        else
            want = 0xE3500000u | rn | (c.kind == kFxXHi ? xhi : yhi);
        const uint32_t cur = bus_read_u32(c.addr);
        if (cur == want) continue;
        // Only the stock word, or one this function wrote at another size --
        // the same register and either compare form -- is ever replaced. A
        // build whose code is not what this table says is left alone.
        const bool ours = (cur & 0xFFFFF000u) == (0xE3500000u | rn) ||
                          (cur & 0xFFFFF000u) == (0xE3700000u | rn);
        if (cur != c.stock && !ours) {
            ++declined;
            continue;
        }
        bus_write_u32(c.addr, want);
        ++written;
    }
    if (written) {
        if (span || vspan)
            std::fprintf(stderr,
                         "[lb_fxcull] effect/particle screen test widened: "
                         "x %d..%u, y %d..%u (was 0..256, ..192; %u of %u "
                         "compares written)\n",
                         -(int)xlo_v, xhi_v, -(int)ylo_v, yhi_v, written,
                         kFxCullCount);
        else
            std::fprintf(stderr,
                         "[lb_fxcull] effect/particle screen test back to "
                         "the cartridge's 0..256 x ..192 (%u compares)\n",
                         written);
        std::fflush(stderr);
    }
    // SAY SO when the code is not what the table expects -- but not on the
    // first frames, which run before the ARM9 image is in RAM (see the gather
    // rect's warning above for the investigation that cost).
    static unsigned fails = 0u;
    static bool said = false;
    if (!declined) {
        fails = 0u;
    } else if (++fails >= 60u && !said) {
        said = true;
        std::fprintf(stderr,
                     "[lb_fxcull] NOT PATCHED: %u of %u compares are neither "
                     "stock nor ours (first at %08X = %08X)\n",
                     declined, kFxCullCount, kFxCull[0].addr,
                     bus_read_u32(kFxCull[0].addr));
        std::fflush(stderr);
    }
}

// ---- UNIT AND BUILDING SOUNDS: the cartridge's own audible window ---------
//
// The defect: unit and building sounds did not play, and construction sounds
// were inconsistent.
//
// Every sound that belongs to an entity passes one test before it is started:
// func_0208938C (Thumb; r0 = the sound manager 0x02155464, r1 = the entity).
// Its three callers, and nothing else, start entity sounds:
//
//   func_02089270  the entity's own sequence archive (index 0 ONSELECT,
//                  1 ONDEATH, 2 ONSTRIKE, 3 ONHIT, 4 ONSPECIAL): the command
//                  acknowledgement func_020E3078, ProduceUnit's new unit
//                  (func_02072248), ConstructStructure / ConstructMultiple,
//                  DieEntityCommand, HarvestAction, func_0205079C/02050F3C
//   func_02089230  spells (sequence archives 182..215)
//   func_020890FC  collectibles (216)
//
// It turns the guest camera (view+0x5E/+0x60, func_02008570) into a map cell,
// 24x16 px (func_020085A0), and accepts the entity only if the entity's own
// cell (+0x128/+0x129, func_0205A00C) lies inside
//
//   x0 = cx >= 2 ? cx - 2 : 0         02089424  cmp  r0,#2      2802
//                                     02089428  subs r0,r0,#2   1E80
//   y0 = cy >= 2 ? cy - 2 : 0         02089432  cmp  r0,#2      2802
//                                     02089436  subs r0,r0,#2   1E80
//   x1 = min(cx + 13, map - 1)        02089448  adds r1,#13     310D
//   y1 = min(cy + 14, map - 1)        02089456  adds r1,#14     310E
//
// -- the DS's own 256x192 and about two cells around it. A refused entity is
// silent: func_02088998 -> func_02089AAC -> NNS_SndArcPlayerStartSeqArc
// (func_02107840, its only caller) is never reached. The interface sounds
// (sequence archives 0 and 1, func_020892B0 / func_02089314) have no position
// test and the music is a stream, which is the observed split exactly: menus
// and music play, units and buildings do not, and construction is heard only
// when the builder stands inside the guest's own 256x192.
//
// In Enhanced the picture is lb_band_left/right px wider than that 256 and
// lb_band_top/bottom px taller than its 192: at a 974x600 surface
// the window above is 16x17 cells of a ~41x38-cell picture, and at 1536x616
// it is 16 cells of a 64-cell-wide one. The gather rect (apply_cull_width)
// and the effect test (apply_fx_cull) were widened for exactly this reason;
// this test was not.
//
// THE FIX IS THE CARTRIDGE'S OWN TEST ON THE PORT'S PICTURE. Each margin
// grows by that side's band, rounded UP to whole cells, and the stock margin
// stays outside it: the window becomes the picture plus the cartridge's two
// cells. The split is the one lb_band_latch fixed for this vblank (it slides
// near map edges), so a margin changes only when a band crosses a whole cell.
// The test caches its window in the sound manager (+0x18/+0x19 low corner,
// +0x20/+0x21 high, +0x28/+0x29 the camera cell it was built for, +0x2C
// valid) and rebuilds it only when the camera's cell changes, so a change of
// margin clears +0x2C. The far corner is stored with strb, so cx + margin
// must stay below 256: the 127-cell cap guarantees it on a 96x96 map.
//
// Sound only: the three callers do nothing with the answer but start a
// sequence, so no simulation state depends on it. The guest does a little
// more work per pass, though, and the pass length is what the game's pacing
// reads (docs/351), so a recording made before this (no `snd=1` in its
// header) replays with the stock window and the exactness gate stays exact
// (docs/376). Patched, func_0208938C fails the
// recompiler's content validation and runs in Tier 3 -- a few calls a pass,
// the arrangement apply_fx_cull already runs under. It must NOT also be given
// a dispatch override: lb_dispatch_original hands back the lifted body, which
// predates the patch.
//
// Only the stock halfword, or one this function wrote, is ever replaced, all
// six or none. Off (Vanilla, LB_WIDEMAP=0, LB_SNDCULL=0, an older recording
// replaying, or no band) writes the stock encodings back. LB_SNDLOG=1 prints the window the cartridge
// cached against the picture's own cells whenever either changes.
namespace {
struct LbSndSite { uint32_t addr; uint16_t stock; uint16_t form; };
// `form` is the opcode byte every encoding this function writes there shares.
const LbSndSite kSndCull[6] = {
    {0x02089424u, 0x2802u, 0x2800u},   // cmp  r0,#2     -> cmp  r0,#ml
    {0x02089428u, 0x1E80u, 0x3800u},   // subs r0,r0,#2  -> subs r0,#ml
    {0x02089432u, 0x2802u, 0x2800u},   // cmp  r0,#2     -> cmp  r0,#mt
    {0x02089436u, 0x1E80u, 0x3800u},   // subs r0,r0,#2  -> subs r0,#mt
    {0x02089448u, 0x310Du, 0x3100u},   // adds r1,#13    -> adds r1,#mr
    {0x02089456u, 0x310Eu, 0x3100u},   // adds r1,#14    -> adds r1,#mb
};
constexpr uint32_t kSndMgr = 0x02155464u;          // FUN_02088454's answer
constexpr uint32_t kSndRectValid = kSndMgr + 0x2Cu;
}  // namespace

static void snd_window_log(void) {
    static const bool on = std::getenv("LB_SNDLOG") != nullptr;
    if (!on) return;
    int camx = 0, camy = 0, mw = 0, mh = 0;
    if (!lb_widemap_cam_px(&camx, &camy) || !lb_widemap_map_px(&mw, &mh))
        return;
    const int gw = (int)nds_gpu3d_output_width();
    const int gh = (int)nds_gpu3d_output_height();
    // The picture, in world pixels, clamped to the map, then in cells.
    int px0 = camx - (gw > 256 ? lb_band_left(gw) : 0);
    int px1 = camx + 255 + (gw > 256 ? lb_band_right(gw) : 0);
    int py0 = camy - (gh > 192 ? lb_band_top(gh) : 0);
    int py1 = camy + 191 + (gh > 192 ? lb_band_bottom(gh) : 0);
    px0 = std::max(px0, 0);
    py0 = std::max(py0, 0);
    px1 = std::min(px1, mw - 1);
    py1 = std::min(py1, mh - 1);
    if (px1 < px0 || py1 < py0) return;
    const int pc[4] = {px0 / 24, py0 / 16, px1 / 24, py1 / 16};
    // The window func_0208938C cached, exactly as it will test it.
    const int wc[4] = {(int)bus_read_u8(kSndMgr + 0x18u),
                       (int)bus_read_u8(kSndMgr + 0x19u),
                       (int)bus_read_u8(kSndMgr + 0x20u),
                       (int)bus_read_u8(kSndMgr + 0x21u)};
    const int valid = (int)bus_read_u8(kSndRectValid);
    static int last[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    const int now[9] = {pc[0], pc[1], pc[2], pc[3],
                        wc[0], wc[1], wc[2], wc[3], valid};
    bool changed = false;
    for (int i = 0; i < 9; ++i)
        if (now[i] != last[i]) { last[i] = now[i]; changed = true; }
    if (!changed) return;
    const int pic = (pc[2] - pc[0] + 1) * (pc[3] - pc[1] + 1);
    const int ox = std::min(pc[2], wc[2]) - std::max(pc[0], wc[0]) + 1;
    const int oy = std::min(pc[3], wc[3]) - std::max(pc[1], wc[1]) + 1;
    const int inside = (valid && ox > 0 && oy > 0) ? ox * oy : 0;
    std::fprintf(stderr,
                 "[lb_sndlog] audible cells x %d..%d y %d..%d (valid %d) | "
                 "picture cells x %d..%d y %d..%d | %d%% of the picture is "
                 "silent for unit/building sounds\n",
                 wc[0], wc[2], wc[1], wc[3], valid, pc[0], pc[2], pc[1], pc[3],
                 pic > 0 ? 100 * (pic - inside) / pic : 0);
    std::fflush(stderr);
}

// Whether the widened window is in force: Enhanced, not turned off, and not
// replaying a recording made with the stock window (lb_demo.cpp, snd=). The
// recorder writes this into a new recording's header.
extern "C" int lb_snd_cull_live(void) {
    if (!lb_setting_enhanced()) return 0;
    static const bool off = [] {
        const char* w = std::getenv("LB_WIDEMAP");
        const char* v = std::getenv("LB_SNDCULL");
        return (w && w[0] == '0') || (v && v[0] == '0');
    }();
    if (off) return 0;
    return lb_demo_play_snd() != 0 ? 1 : 0;
}

// LB_SNDSTART=1: which sounds the cartridge STARTED, by (archive, index).
//
// func_02089AAC starts a sequence only if its byte in the array at
// 0x02155878 (217 archives x 20 indices) is clear, then sets it; task 0x21
// clears the array once a pass. So the set bytes seen at a VBlank are the
// sounds started since the last clear. Archives 0 and 1 are the interface
// (no position test); every other archive is an entity's own, a spell
// (182..215) or a collectible (216), all behind func_0208938C. A key-on count
// cannot tell those apart; this can.
static void snd_start_log(void) {
    static const bool on = std::getenv("LB_SNDSTART") != nullptr;
    if (!on) return;
    constexpr uint32_t kStarted = 0x02155878u;
    constexpr int kArchives = 217, kIndices = 20;
    static uint8_t prev[kArchives * kIndices];
    for (int a = 0; a < kArchives; ++a) {
        for (int i = 0; i < kIndices; ++i) {
            const int k = a * kIndices + i;
            const uint8_t v = bus_read_u8(kStarted + (uint32_t)k);
            if (v && !prev[k]) {
                std::fprintf(stderr, "[lb_sndstart] vb %u archive %d index %d"
                             " (%s)\n", lb_dump_frame_get(), a, i,
                             a <= 1 ? "interface" : a >= 216 ? "collectible"
                             : a >= 182 ? "spell" : "entity");
                std::fflush(stderr);
            }
            prev[k] = v;
        }
    }
}

static void apply_snd_cull(void) {
    snd_window_log();
    snd_start_log();
    const bool on = lb_snd_cull_live() != 0;

    // The size lb_band_latch was given this vblank, so lb_band_left/top answer
    // with the latched split rather than the centred fallback.
    const int gw = (int)nds_gpu3d_output_width();
    const int gh = (int)nds_gpu3d_output_height();
    auto cells = [](int px, int cell) -> uint32_t {
        return px > 0 ? (uint32_t)((px + cell - 1) / cell) : 0u;
    };
    uint32_t l = 0u, r = 0u, t = 0u, b = 0u;
    if (on && gw > 256) {
        l = cells(lb_band_left(gw), 24);
        r = cells(lb_band_right(gw), 24);
    }
    if (on && gh > 192) {
        t = cells(lb_band_top(gh), 16);
        b = cells(lb_band_bottom(gh), 16);
    }
    const uint32_t ml = std::min<uint32_t>(2u + l, 127u);
    const uint32_t mt = std::min<uint32_t>(2u + t, 127u);
    const uint32_t mr = std::min<uint32_t>(13u + r, 127u);
    const uint32_t mb = std::min<uint32_t>(14u + b, 127u);
    const uint16_t want[6] = {
        (uint16_t)(0x2800u | ml),
        (uint16_t)(ml == 2u ? 0x1E80u : (0x3800u | ml)),
        (uint16_t)(0x2800u | mt),
        (uint16_t)(mt == 2u ? 0x1E80u : (0x3800u | mt)),
        (uint16_t)(0x3100u | mr),
        (uint16_t)(0x3100u | mb),
    };

    // ALL SIX OR NONE, and only over the stock halfword or this function's own
    // form -- which also waits out the boot frames before the ARM9 image is in
    // RAM (see the gather rect's warning for why that must stay quiet).
    unsigned declined = 0u;
    bool same = true;
    for (int i = 0; i < 6; ++i) {
        const uint16_t cur = bus_read_u16(kSndCull[i].addr);
        if (cur != want[i]) same = false;
        if (cur != kSndCull[i].stock && (cur & 0xFF00u) != kSndCull[i].form)
            ++declined;
    }
    static unsigned fails = 0u;
    static bool said_fail = false;
    if (declined) {
        if (++fails >= 60u && !said_fail) {
            said_fail = true;
            std::fprintf(stderr,
                         "[lb_sndcull] NOT PATCHED: %u of 6 sites are neither "
                         "stock nor ours (first at %08X = %04X)\n", declined,
                         kSndCull[0].addr,
                         (unsigned)bus_read_u16(kSndCull[0].addr));
            std::fflush(stderr);
        }
        return;
    }
    fails = 0u;
    if (same) return;
    for (int i = 0; i < 6; ++i)
        if (bus_read_u16(kSndCull[i].addr) != want[i])
            bus_write_u16(kSndCull[i].addr, want[i]);
    // The cached window was built with the old margins: have the next test
    // rebuild it (func_0208938C sets +0x2C again when it does).
    bus_write_u8(kSndRectValid, 0u);

    // Said when the surface changes or the patch turns on or off -- not every
    // time a sliding band crosses a cell near a map edge.
    const bool widened = ml != 2u || mt != 2u || mr != 13u || mb != 14u;
    static int said_w = -1, said_h = -1, said_on = -1;
    if (gw == said_w && gh == said_h && (widened ? 1 : 0) == said_on) return;
    said_w = gw;
    said_h = gh;
    said_on = widened ? 1 : 0;
    if (widened)
        std::fprintf(stderr,
                     "[lb_sndcull] unit/building sound window: camera cell "
                     "-%u..+%u x -%u..+%u (was -2..+13 x -2..+14), surface "
                     "%dx%d\n", ml, mr, mt, mb, gw, gh);
    else
        std::fprintf(stderr,
                     "[lb_sndcull] unit/building sound window back to the "
                     "cartridge's -2..+13 x -2..+14 cells\n");
    std::fflush(stderr);
}

// ---- A SPRITE'S DEPTH FOLLOWS ITS FEET ACROSS THE BANDS (docs/377) ---------
//
// Sprite overlap differed from vanilla at some resolutions: builders were
// drawn on top of trees/crystals they stand behind.
//
// Every world billboard's depth is its screen row: Unit_BuildBillboard
// (0203ABC8) stores z = 11 + feet row, and the quad emitter (0204C210) turns
// it into MTX_TRANS z, so lower on screen is nearer (depth 0x7FFE00 - z*0x2000,
// the ortho G3_OrthoW(0,192,0,256,-1024,1024) of 020FA694). But the sum is
// kept in a byte and clamped unsigned to a table value of 243:
//
//   0203B28C  ldrb  r0,[r0,#1]      ; max = 243 (table 0x02144C30)
//   0203B290  add   r1,r2,r1        ; 11 + feet row
//   0203B294  and   r1,r1,#0xff     ; 8-bit wrap
//   0203B298  cmp   r1,r0
//   0203B29C  movhs r1,r0           ; unsigned clamp to 243
//
// On the DS the feet row is -16..240, so this is invisible. In the widened
// picture the gather reaches rows far above 0 and below 191, and two seams
// appear. At the top (row -12 and above it) the sum wraps to 255 and clamps to
// 243, so a builder BEHIND a spell-grown tree or a building is drawn over it.
// At the bottom (rows 233..244 tie at 243, 245+ wrap to 0..) the order
// reverses the other way. Projectiles (020EBF34) and collectables (020EBB70)
// compute the same sum the same way.
//
// THE FIX IS THE CARTRIDGE'S OWN FORMULA WITHOUT THE BYTE (tier 1 of the
// sprite-depth options; every stock word checked against the ROM image). No wrap, a
// SIGNED clamp at 444, and the emitter reads the halfword signed, so z is
// 11 + feet row across the whole picture:
//   * rows -11..232 keep their DS values exactly;
//   * the top band goes negative (never below about -600, far inside the
//     ortho's -1024);
//   * everything stays behind the health bars (448) and the HUD (503 up), so
//     lb_hud's threshold is untouched.
// Flagged projectiles, which the cartridge forces to the maximum, get the new
// maximum. Siege and transport models already compute s16(11 + y) unwrapped
// (0203B554), so billboards and models now agree across the bands.
//
// Draw order only. entity+0x66 is written only at 0203B2A4 and sprite+0x56
// read only by the emitter, so there is no simulation effect. Every other
// emitter caller stores a z under 0x8000, so ldrsh reads theirs unchanged.
// Patched, the four functions fail content validation and run in Tier 3.
//
// When it applies: Enhanced, with a vertical band (a surface taller than
// 192). Without one the feet rows stay the DS's own and the stock words give
// the cartridge's exact order. Not while a recording made before this
// replays (lb_demo.cpp, depth=). LB_DEPTHWRAP=0 restores the stock words.
// ALL TEN OR NONE, and only over the stock word or ours.
namespace {
struct LbDepthSite { uint32_t addr; uint32_t stock; uint32_t ours; };
const LbDepthSite kDepthUnwrap[10] = {
    // Unit_BuildBillboard
    {0x0203B28Cu, 0xE5D00001u, 0xE3A00F6Fu},   // ldrb r0,[r0,#1] -> mov r0,#0x1BC
    {0x0203B294u, 0xE20110FFu, 0xE1A00000u},   // and r1,r1,#0xff -> nop
    {0x0203B29Cu, 0x21A01000u, 0xC1A01000u},   // movhs r1,r0     -> movgt r1,r0
    // projectile
    {0x020EC25Cu, 0xE20010FFu, 0xE1A01000u},   // and r1,r0,#0xff -> mov r1,r0
    {0x020EC264u, 0x15D01000u, 0x13A01F6Fu},   // ldrbne r1,[r0]  -> movne r1,#0x1BC
    {0x020EC26Cu, 0xE5D00000u, 0xE3A00F6Fu},   // ldrb r0,[r0]    -> mov r0,#0x1BC
    {0x020EC274u, 0x21A01000u, 0xC1A01000u},   // movhs r1,r0     -> movgt r1,r0
    // collectable (clamps signed first, then wraps)
    {0x020EBDF0u, 0xE5D06000u, 0xE3A06F6Fu},   // ldrb r6,[r0]    -> mov r6,#0x1BC
    {0x020EBE18u, 0xE20610FFu, 0xE1A01006u},   // and r1,r6,#0xff -> mov r1,r6
    // the shared quad emitter
    {0x0204C290u, 0xE1D805B6u, 0xE1D805F6u},   // ldrh r0,[r8,#0x56] -> ldrsh
};
}  // namespace

extern "C" int lb_depth_unwrap_live(void) {
    if (!lb_setting_enhanced()) return 0;
    static const bool off = [] {
        const char* v = std::getenv("LB_DEPTHWRAP");
        return v && v[0] == '0';
    }();
    if (off) return 0;
    return lb_demo_play_depth() != 0 ? 1 : 0;
}

static void apply_depth_unwrap(void) {
    const bool want = lb_depth_unwrap_live() != 0 &&
                      (int)nds_gpu3d_output_height() > 192;
    for (int i = 0; i < 10; ++i) {
        const uint32_t cur = bus_read_u32(kDepthUnwrap[i].addr);
        if (cur != kDepthUnwrap[i].stock && cur != kDepthUnwrap[i].ours) {
            static unsigned fails = 0;
            if (++fails == 600u) {
                std::fprintf(stderr, "[lb_depth] NOT PATCHED: %08X holds %08X, "
                             "neither stock nor ours\n", kDepthUnwrap[i].addr,
                             cur);
                std::fflush(stderr);
            }
            return;
        }
    }
    bool wrote = false;
    for (int i = 0; i < 10; ++i) {
        const uint32_t w = want ? kDepthUnwrap[i].ours : kDepthUnwrap[i].stock;
        if (bus_read_u32(kDepthUnwrap[i].addr) == w) continue;
        bus_write_u32(kDepthUnwrap[i].addr, w);
        wrote = true;
    }
    if (!wrote) return;
    std::fprintf(stderr, want
                 ? "[lb_depth] sprite depth is 11 + feet row across the "
                   "bands (no byte wrap, signed clamp 444)\n"
                 : "[lb_depth] stock sprite depth (byte, clamp 243)\n");
    std::fflush(stderr);
}

// ---- THE MONKEY CAGE THE CARTRIDGE NEVER SHOWS (docs/385) -------------------
//
// In the pirate-era levels caught monkeys use the same sprite as the caged
// space criminals, a minifig head sticking out of the cage. A unit caught
// by CollectMarkedUnitsObjective (Search For Scum mh2_3, Monkey Business
// pi3_1) is drawn by Unit_BuildBillboard with a fixed cell of
// Sprites/Projectiles.NCBR, and the code does mean to tell the two apart:
//
//   0203AD50  ldr   r0,[sb,#0x160]
//   0203AD54  ldr   r0,[r0,#8]      ; the unit's blueprint
//   0203AD58  ldrh  r0,[r0,#0x66]   ; +0x66: its PROJECTILE index
//   0203AD5C  cmp   r0,#0x99        ; 153: the Monkey's own index
//   0203AD60  moveq r6,#0x1A0       ; the monkey cage, sheet (0,104)
//   0203AD64  movne r6,#0x140       ; the minifig-head cage, sheet (0,80)
//
// It reads the wrong field. 153 is the Monkey's INDEX (+0x04, docs/11), while
// the Monkey is melee and its +0x66 is 0xFFFF, so no unit ever takes the
// branch and caught monkeys wear the minifig head. The monkey cage (frame
// 0x1A0, a brown head at the bars) ships in the sheet, unused. The fix reads
// +0x04: one word, checked against the ROM.
//
// Drawing only: r6 is the cell, split into sheet coordinates later in the
// same function. Patched, the function fails content validation and runs in
// Tier 3, like docs/377's words in it. The Enhancements tab's "Fix the
// Monkey Cage", on by default, this machine only. Not while a recording made
// before this replays (cage=); LB_CAGEFIX=0 restores the stock word.
namespace {
constexpr uint32_t kCageSite = 0x0203AD58u;
constexpr uint32_t kCageStock = 0xE1D006B6u;   // ldrh r0,[r0,#0x66]
constexpr uint32_t kCageOurs = 0xE1D000B4u;    // ldrh r0,[r0,#4]
}  // namespace

extern "C" int lb_demo_play_cage(void);   /* src/lb_demo.cpp */

// Live: the Enhancements tab's "Fix the Monkey Cage" (on by default), which a
// recording's header carries as cage=.
extern "C" int lb_cage_fix_live(void) {
    static const bool off = [] {
        const char* v = std::getenv("LB_CAGEFIX");
        return v && v[0] == '0';
    }();
    return !off && lb_setting_fix_monkey_cage() != 0 ? 1 : 0;
}

static void apply_cage_fix(void) {
    // A replay takes the recorder's choice, whatever this machine's is.
    const int played = lb_demo_play_cage();
    const bool want = played >= 0 ? played == 1 : lb_cage_fix_live() != 0;
    const uint32_t cur = bus_read_u32(kCageSite);
    if (cur != kCageStock && cur != kCageOurs) return;   // not the ARM9 we know
    const uint32_t w = want ? kCageOurs : kCageStock;
    if (cur == w) return;
    bus_write_u32(kCageSite, w);
    std::fprintf(stderr, want ? "[lb_cage] caught monkeys use the monkey cage "
                                "(0203AD58 reads the blueprint index)\n"
                              : "[lb_cage] stock cage choice\n");
    std::fflush(stderr);
}

// ---- THE MINIMAP'S CAMERA BOX, DRAWN BY THE CARTRIDGE AT THE VIEW'S SIZE -----
//
// docs/370. The top-screen camera square flickered back to vanilla size on
// the minimap while in motion.
//
// The box is the cartridge's own drawing. func_02025CF8 plots it into the
// minimap's 4bpp canvas (BG3 of the top screen, origin (136,40)), once per game
// update, from the camera at view+0x5E/+0x60 (read from the ROM):
//
//   02025D0C  add r2, r2, #12        camx + half a metatile, to round
//   02025D24  add r3, r3, #8         camy + half a metatile
//                                    -> metatile -> mt2px[] (95*j/(N-1))
//   02025D5C  ldrb r1, [r6,#0x8CC]   the size, cached at mm+0x8CC/+0x8CD,
//   02025D68  bne  02025DC0          computed once (the constructor sets 0xFF)
//   02025D6C  mov ip, #0xF8000       248.0: the view's width  -> /24 + 1 = 11
//   02025D70  mov r3, #0xC0000       192.0: the view's height -> /16     = 12
//                                    -> mt2px[11] x mt2px[12] = 16 x 18 px
//
// then clamps it into the world rect's part of the minimap and draws four
// lines. The port used to leave that alone: it found the 16x18 box in the
// top screen's pixels, painted it out and drew a wider one with ImGui from a
// fitted linear map (docs/180, 255, 347). But the minimap redraws only a
// quarter of its rows per update (24 of 96, rotating), so while the camera
// moves the drawn box is slices from up to four updates. Checked against the
// live camera, those frames fail the finder's 8-px test, and the vanilla box
// shows through. That is the flicker.
//
// So the cartridge now draws the box for the view the window shows, from the
// same four words:
//   * the two rounding offsets move the origin to the visible field's corner.
//     Mid-map that field is centred on the native window: its left edge is
//     camx - (vis-256)/2 (the band is centred and the frontend crops the
//     middle vis of it; the HUD anchor uses the same shift). At a map edge,
//     the band slides and the plotter's own world clamp stops the box at the
//     minimap's edge.
//   * the two literals give the view's size. Any value inside the same metatile
//     window gives the same count, so one whose <<12 is an ARM immediate is
//     chosen. The counts are capped so mt2px[] (N entries) is not overrun.
//   * the size cache is kept. When the literals change, mm+0x8CC is set back
//     to 0xFF, the constructor's value, so the cartridge recomputes the size
//     once. The update's instruction stream is otherwise the stock one.
//
// Only the words are the port's. The camera, rounding, table, clamp, quarter
// slicing and drawing are the cartridge's. While this is live the panel shows
// the game's box as drawn and does not scrub or overdraw it.
//
// Gated on Enhanced (and LB_WIDEMAP), off for a native-size view; LB_MMBOX=0
// turns it off alone, for an A/B. All four words or none, self-healing, logged
// on change. With the view back at 256x192 or Enhanced off, the stock words go
// back and the cache is reset so the stock 16x18 is recomputed.
namespace {
constexpr uint32_t kMmBoxAddr[4] = {0x02025D0Cu, 0x02025D24u,
                                    0x02025D6Cu, 0x02025D70u};
constexpr uint32_t kMmBoxStock[4] = {0xE282200Cu,   // add r2, r2, #12
                                     0xE2833008u,   // add r3, r3, #8
                                     0xE3A0C93Eu,   // mov ip, #0xF8000
                                     0xE3A03703u};  // mov r3, #0xC0000
// The encodings this function may have written: add/sub on the two rounding
// offsets, mov on the two literals.
bool mmbox_word_is_ours(int i, uint32_t w) {
    const uint32_t op = w & 0xFFFFF000u;
    switch (i) {
        case 0: return op == 0xE2822000u || op == 0xE2422000u;
        case 1: return op == 0xE2833000u || op == 0xE2433000u;
        case 2: return op == 0xE3A0C000u;
        default: return op == 0xE3A03000u;
    }
}
bool g_mmbox_live = false;
}  // namespace

extern "C" int lb_minimap_box_live(void) { return g_mmbox_live ? 1 : 0; }

// add/sub rd, rd, #|delta| with the immediate nearest to |delta| that ARM can
// encode (exact below 256). 0 when none is found.
static uint32_t mmbox_addsub(uint32_t add_op, uint32_t sub_op, int delta) {
    const uint32_t mag = (uint32_t)(delta < 0 ? -delta : delta);
    if (mag == 0u) return add_op;                       // add rd, rd, #0
    for (uint32_t d = 0; d < 256u; ++d) {
        const uint32_t lo = mag > d ? mag - d : 0u, hi = mag + d;
        uint32_t imm = lo ? arm_imm12(lo) : 0u;
        if (!imm) imm = arm_imm12(hi);
        if (imm) return (delta < 0 ? sub_op : add_op) | imm;
    }
    return 0u;
}

// The fx32 literal for a view extent: a value in [lo, hi] (one metatile
// count's window) whose <<12 is an ARM immediate, `prefer` when it is one.
static uint32_t mmbox_extent_imm(uint32_t prefer, uint32_t lo, uint32_t hi,
                                 uint32_t* chosen) {
    if (prefer >= lo && prefer <= hi)
        if (const uint32_t imm = arm_imm12(prefer << 12u)) {
            *chosen = prefer;
            return imm;
        }
    for (uint32_t v = lo; v <= hi; ++v)
        if (const uint32_t imm = arm_imm12(v << 12u)) {
            *chosen = v;
            return imm;
        }
    return 0u;
}

static void apply_minimap_box(void) {
    bool want = lb_setting_enhanced();
    if (const char* v = std::getenv("LB_WIDEMAP"))
        if (v[0] == '0') want = false;
    if (const char* v = std::getenv("LB_MMBOX"))
        if (v[0] == '0') want = false;

    // The view the player sees, in guest pixels, clamped to the surface the
    // same way the HUD anchor clamps it (lb_hud_scan).
    const int gw = (int)nds_gpu3d_output_width();
    const int gh = (int)nds_gpu3d_output_height();
    int vis = 0, visr = 0;
    lb_hud_visible_size(&vis, &visr);
    if (vis <= 0 || vis > gw) vis = gw;
    if (visr <= 0 || visr > gh) visr = gh;
    if (vis < 256) vis = 256;
    if (visr < 192) visr = 192;
    if (vis == 256 && visr == 192) want = false;

    // N, the map's size in metatiles (u8 map+0x14): mt2px[] has N entries. 0
    // outside a battle, and then the counts are not capped (the words do
    // nothing until a minimap exists, and the constructor starts the cache at
    // 0xFF).
    //
    // NOT 0x02156CB0 ALONE (docs/378). At this hook that pointer reads 0 --
    // measured in battle: every [lb_mmbox] line said "map 0 metatiles wide"
    // while the debug server, between frames, read 0x022BD84C with +0x14 = 64
    // -- so N was always 0 and the caps below never applied. Past a surface
    // of 24*(N-2)+8 px (1496 on a 64-cell map; a 3440x1440 window
    // is 1720) the cartridge then read mt2px[] past its end. So when the
    // pointer reads 0, the same byte comes from the map pointer
    // lb_field_map_chain captured (host terrain only), else from the last
    // value lb_widemap's own fill read from it (lb_widemap_map_px: width =
    // u8 map+0x14 cells, times 24).
    uint32_t n = 0u;
    const uint32_t map = bus_read_u32(0x02156CB0u);
    if (map >= 0x02000000u && map < 0x02400000u) {
        n = bus_read_u8(map + 0x14u);
    } else {
        uint32_t cm = 0u;
        int cw = 0, ch = 0, mw = 0, mh = 0;
        if (lb_field_map_chain(&cm, &cw, &ch) && cm >= 0x02000000u &&
            cm < 0x02400000u)
            n = bus_read_u8(cm + 0x14u);
        else if (lb_widemap_map_px(&mw, &mh))
            n = (uint32_t)(mw / 24);
    }

    uint32_t word[4];
    uint32_t cw = 248u, ch = 192u;
    if (want) {
        const int c = (vis - 256) / 2, ct = (visr - 192) / 2;
        word[0] = mmbox_addsub(0xE2822000u, 0xE2422000u, 12 - c);
        word[1] = mmbox_addsub(0xE2833000u, 0xE2433000u, 8 - ct);
        // The stock width is 256 - 8 (/24, +1 metatile at 02025DA0); the
        // widened one keeps that relationship.
        uint32_t k = (uint32_t)(vis - 8) / 24u, t = (uint32_t)visr / 16u;
        if (n >= 3u) {
            if (k > n - 2u) k = n - 2u;        // mt2px[k + 1] must exist
            if (t > n - 1u) t = n - 1u;
        }
        const uint32_t wimm = mmbox_extent_imm((uint32_t)(vis - 8), 24u * k,
                                               24u * k + 23u, &cw);
        const uint32_t himm = mmbox_extent_imm((uint32_t)visr, 16u * t,
                                               16u * t + 15u, &ch);
        word[2] = wimm ? (0xE3A0C000u | wimm) : 0u;
        word[3] = himm ? (0xE3A03000u | himm) : 0u;
        if (!word[0] || !word[1] || !word[2] || !word[3]) {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr, "[lb_mmbox] DECLINED: a word for a %dx%d "
                             "view is not encodable; the stock box stays\n",
                             vis, visr);
                std::fflush(stderr);
            }
            want = false;
        }
    }
    if (!want)
        for (int i = 0; i < 4; ++i) word[i] = kMmBoxStock[i];

    // ALL FOUR OR NONE: each site must hold the stock word or one of ours,
    // which also waits out the boot window before the ARM9 image is in RAM.
    for (int i = 0; i < 4; ++i) {
        const uint32_t cur = bus_read_u32(kMmBoxAddr[i]);
        if (cur != kMmBoxStock[i] && !mmbox_word_is_ours(i, cur)) {
            static unsigned fails = 0;
            if (++fails == 600u) {
                std::fprintf(stderr, "[lb_mmbox] NOT PATCHED: %08X holds %08X, "
                             "neither stock nor ours\n", kMmBoxAddr[i], cur);
                std::fflush(stderr);
            }
            g_mmbox_live = false;
            return;
        }
    }
    bool size_changed = false, wrote = false;
    for (int i = 0; i < 4; ++i) {
        if (bus_read_u32(kMmBoxAddr[i]) == word[i]) continue;
        bus_write_u32(kMmBoxAddr[i], word[i]);
        wrote = true;
        if (i >= 2) size_changed = true;
    }
    if (size_changed) {
        // The cached size was computed from the old literals: have the
        // cartridge compute it again, exactly as it does for a new minimap.
        const uint32_t view = bus_read_u32(0x02152034u);
        const uint32_t mm = (view >= 0x02000000u && view < 0x02400000u)
                                ? bus_read_u32(view + 0x34u) : 0u;
        if (mm >= 0x02000000u && mm < 0x02400000u)
            bus_write_u8(mm + 0x8CCu, 0xFFu);
    }
    if (wrote) {
        if (want)
            std::fprintf(stderr, "[lb_mmbox] the cartridge draws the minimap "
                         "box for a %dx%d view: origin %08X %08X, extent %ux%u "
                         "(map %u metatiles wide)\n", vis, visr, word[0],
                         word[1], cw, ch, n);
        else
            std::fprintf(stderr, "[lb_mmbox] stock minimap box (%dx%d view)\n",
                         vis, visr);
        std::fflush(stderr);
    }
    g_mmbox_live = want;
}

// ---- THE GAME'S OWN CADENCE, steady 30 Hz on request (docs/351) ------------
//
// NitroMain's loop (020F6730) waits for a VBlank, runs the whole task list --
// input, the simulation step, camera, 3D, BG staging -- and then waits for a
// SECOND VBlank if less than 20 ms have passed since just before the first:
//
//     020F6910  subs r2, r0, r8 / sbc r1, r1, sl / mov r0, #0 / cmp r1, r0
//     020F6920  cmpeq r2, r4          r4 = 20, set once at 020F68B0
//     020F6924  bhs  skip             >= 20 ms: no second wait
//
// With the task list taking 3.3-16.7 ms that yields passes 1, 2, 1, 2 VBlanks
// apart, and the port's emulated ARM9 sits there (task list ends a median 13.7
// ms after the VBlank on a recorded replay): 37 passes a second, the interval
// changing on 84% of passes -- uneven motion, one simulation step per pass.
//
// ON BY DEFAULT since 2026-09-23 (docs/351), knowing it
// slows the game from ~37 to 30 steps a second in such scenes; the Display
// tab's "Steady game pacing" turns it off. Which cadence is in force is
// decided in one place, lb_cadence_effective: LB_CADENCE=20/30 for a
// measurement, else a PLAYING demo's own (a recording replays at the pacing it
// was made at -- lb_demo.cpp), else the setting.
//
// HOW, SECOND VERSION. The first patched the per-pass compare to 33 ms, and a
// fixed threshold is wrong in one regime or the other: the pass after an
// overrun always measures ~33.4 ms, so 33 ms stalled heavy frames 2-3-2-3
// (docs/351 section 6). Steady pacing is now get_ms_override: NitroMain's two
// clock reads count VBlanks, and the cartridge's own 20 ms compare then waits
// again exactly when no VBlank came during the task list. So this function no
// longer patches anything -- it puts the compare back if an earlier build's
// word is there, and says when the pacing changes.
namespace {
constexpr uint32_t kCadenceCmpAddr = 0x020F6920u;
constexpr uint32_t kCadenceCmpStock = 0x01520004u;   // cmpeq r2, r4
constexpr uint32_t kCadenceCmp33 = 0x03520021u;      // cmpeq r2, #33 (v1)
}  // namespace

extern "C" int lb_cadence_effective(void) {
    static const int env = [] {
        const char* v = std::getenv("LB_CADENCE");
        if (v && std::strcmp(v, "30") == 0) return 30;
        if (v && std::strcmp(v, "20") == 0) return 20;
        return 0;
    }();
    if (env) return env;
    if (const int d = lb_demo_play_cadence()) return d;
    return lb_setting_steady_cadence() ? 30 : 20;
}

static void apply_cadence(void) {
    if (bus_read_u32(kCadenceCmpAddr) == kCadenceCmp33)
        bus_write_u32(kCadenceCmpAddr, kCadenceCmpStock);
    // Only once the ARM9 image is in: before that there is no game to pace.
    if (bus_read_u32(kCadenceCmpAddr) != kCadenceCmpStock) return;
    static int said = 0;
    const int now = lb_cadence_effective();
    if (now == said) return;
    said = now;
    std::fprintf(stderr, "[lb_cadence] %s\n",
                 now == 30 ? "steady 30 Hz: NitroMain's clock reads count "
                             "VBlanks (a pass waits again only if no VBlank "
                             "came during its task list)"
                           : "the cartridge's own 20 ms clock");
    std::fflush(stderr);
}

// ---- per-item unlocks (docs/181) ------------------------------------------
//
// The profile record carries TWO parallel bitfields recording which coded items
// the player has unlocked. Every bit below was measured one at a time by typing
// that item's code on the cartridge's own keyboard and diffing the record, so
// each is the game's own write rather than a guess about layout.
//
// The two fields are the SAME MSB-first bit stream, based 0x24 bytes apart,
// with field B one position further along. That relation held for all 52 codes
// measured without a single exception, which is why it is relied on here rather
// than writing field B from its own table.
static const uint32_t kProfileRecord = 0x02168500u;   // docs/176
static const uint32_t kUnlockFieldA = 0xB4u;
static const uint32_t kUnlockFieldB = 0xD8u;

// UNITS: the 38 characters and vehicles whose bits were measured. Catapult
// (registry index 71) is deliberately ABSENT -- its code set no bit on the run
// that measured the others, and an unmeasured bit is not going to be guessed at
// from its neighbours.
static const uint8_t kUnlockUnits[] = {
    18, 19, 20, 26, 28, 29, 31, 32, 33, 39, 42, 44, 45, 46, 52, 54, 55,
    57, 58, 59, 65, 67, 68, 70, 71, 72, 78, 80, 81, 83, 84, 85, 86, 87,
    88, 89, 90, 91
};

// EXTRAS: the cheat codes themselves, as ITEMS the profile records as found --
// which is a different thing from the flags block above that turns one ON.
// Stream index 108 is a gap in this otherwise solid 93..110 run and is very
// likely Reveal Map, whose record is kind 16 and carries no kind-8 code, so it
// was never typed. Not included, for the same reason Catapult is not.
static const uint8_t kUnlockExtras[] = {
    93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107,
    109, 110
};

// Counts the bits it actually flips, so "the unlock switches are wired" can
// be a measurement instead of a code reading. The panel badges these READ,
// and a badge should only be upgraded on evidence.
static unsigned g_unlock_bits_set = 0;

static void set_stream_bit(uint32_t field_base, int n) {
    const uint32_t addr = kProfileRecord + field_base + (uint32_t)(n / 8);
    const uint8_t mask = (uint8_t)(0x80u >> (n % 8));
    const uint8_t cur = bus_read_u8(addr);
    if (!(cur & mask)) {
        bus_write_u8(addr, (uint8_t)(cur | mask));
        ++g_unlock_bits_set;
    }
}

static void apply_unlocks(void) {
    // Only ever SETS bits, and only the ones a code would have set. Turning a
    // switch off does not re-lock anything: the game has no "forget this code"
    // and forging one would be inventing behaviour the cartridge does not have.
    const bool units = lb_setting_unlock_units() != 0;
    const bool extras = lb_setting_unlock_extras() != 0;
    if (!units && !extras) return;

    // The record is not always resident -- there is no profile before one is
    // chosen. A record whose two fields disagree about a bit they must share is
    // not the record, so nothing is written until they agree.
    int seen = 0, agree = 0;
    for (int n = 0; n < 24 * 8 - 1; ++n) {
        const uint8_t a = bus_read_u8(kProfileRecord + kUnlockFieldA + (uint32_t)(n / 8));
        if (!(a & (uint8_t)(0x80u >> (n % 8)))) continue;
        ++seen;
        const int m = n + 1;
        const uint8_t b = bus_read_u8(kProfileRecord + kUnlockFieldB + (uint32_t)(m / 8));
        if (b & (uint8_t)(0x80u >> (m % 8))) ++agree;
    }
    if (seen < 2 || agree != seen) return;   // not a loaded profile record

    if (units)
        for (size_t i = 0; i < sizeof kUnlockUnits; ++i) {
            set_stream_bit(kUnlockFieldA, kUnlockUnits[i]);
            set_stream_bit(kUnlockFieldB, kUnlockUnits[i] + 1);
        }
    if (extras)
        for (size_t i = 0; i < sizeof kUnlockExtras; ++i) {
            set_stream_bit(kUnlockFieldA, kUnlockExtras[i]);
            set_stream_bit(kUnlockFieldB, kUnlockExtras[i] + 1);
        }
    static unsigned last = 0u;
    if (g_unlock_bits_set != last) {
        last = g_unlock_bits_set;
        std::fprintf(stderr,
                     "[lb_unlock] units=%d extras=%d: %u profile bits set\n",
                     units ? 1 : 0, extras ? 1 : 0, g_unlock_bits_set);
        std::fflush(stderr);
    }
}

// ---- a cosmetic name for the Space Criminal Leader (docs/356) --------------
//
// CLIENT-LOCAL AND COSMETIC. The option makes "Space Criminal Leader" read
// Kranxx or Plisken on this machine, with gameplay and online play untouched.
// So this rewrites words the screen draws and nothing else.
//
// WHERE THE WORDS LIVE. The whole LOC/<language>.lng is loaded into one heap
// block, and the language manager at 0x0215378C keeps it at +0x0C (+0x00 is the
// pending load's handle, +0x04 the language asked for, +0x08 the one loaded).
// The load callback, func_0203E114, stores the block, and func_0203E0B4 then
// turns it into a live table in place: file+0x08 := file+0x0C, and each of the
// 0xBF8 offsets at file+0x0C becomes an absolute pointer. func_0203E0FC,
// `return table[id]`, is the only reader, and its only caller, func_0203E1D4,
// COPIES the text into a String for whichever screen asked. So a rewrite here
// reaches every fetch made after it, and a screen that already holds its copy
// keeps the old words until it is rebuilt.
//
// WHY NOTHING ELSE SEES IT. Of the 14 literals pointing into the manager, the
// five at its base are all inside the manager's own functions and the other
// nine read the language NUMBER; none holds +0x0C, and no overlay has any. The
// 22 callers of func_0203E1D4 are menus, widgets and HUD text. Peers exchange
// commands, never text, and the multiplayer start (func_020F5168) names each
// player from the lobby record, not from this table.
//
// OFF IS THE CARTRIDGE. At 0, with nothing of ours in guest RAM, not one guest
// byte is read or written. Turning it off puts the shipped bytes back from a
// saved copy, and only where this patch's text is still what is there. A block
// the game has freed or replaced is forgotten, never written to.
//
// Every row below was checked against all eight LOC/*.lng read out of the ROM:
// each needle matches byte for byte, every result fits in the storage it
// replaces, and no other id points inside a rewritten string (docs/356).
namespace {

constexpr uint32_t kLangManager = 0x0215378Cu;
constexpr uint32_t kLangMagic = 0x474E414Cu;       // "LANG"
constexpr uint32_t kLangIds = 0xBF8u;              // func_0203E0B4's loop bound
constexpr uint32_t kLangStrings = 0x0Cu + kLangIds * 4u;   // first text byte
constexpr uint32_t kLangMaxBytes = 0x20000u;       // largest .lng: 100,036 B
constexpr size_t kLeaderMaxText = 255u;            // longest edited: 189 B

// The name itself: the SpaceCriminalLeader blueprint's display name (556, its
// +0x06) and cheat record 51's (0x0B26 = 2854, what the cheat keyboard prints
// on accepting ZVDNJSU). Rewritten whole, and only when it is one of these.
constexpr uint32_t kLeaderNameIds[] = {556u, 2854u};
constexpr const char* kLeaderShippedNames[] = {
    "Space Criminal Leader",            // UK and US English; Japanese.lng 2854
    "Criminal Leader",                  // Japanese.lng 556
    "Rumskurkenes boss",                // Danish
    "Leader des criminels spatiaux",    // French
    "Weltall Kriminellen Oberhaupt",    // German
    "Capo dei criminali spaziali",      // Italian
    "L\xC3\xAD" "der de los criminales espaciales",  // Spanish, UTF-8 i-acute
};

// Sentences that mention him, in ids first..last: `needle` gives way to the
// name, then `tail`. The English mission text of Criminal Minds is found in
// UK_English, American_English and Japanese.lng (English text there); the
// translations keep their own descriptive wording. 2697 is the briefing that
// already calls him Plisken, in every language.
struct LeaderEdit {
    uint32_t first, last;
    const char* needle;
    const char* tail;
};
constexpr LeaderEdit kLeaderEdits[] = {
    {2676u, 2676u, "the Space Criminal Leader", ""},    // Rescue ... from the Earthlings.
    {2678u, 2678u, "Space Criminal Leader", ""},        // Free the captured ...
    {2679u, 2684u, "the Space Criminal Leader", ""},    // Bust out ... / Get ... back to Base
    {2685u, 2687u, "The Space Criminal Leader", ""},    // ... died
    {2697u, 2697u, "Plisken", ""},                      // Plisken, the leader of ...
    {2702u, 2704u, "the Space Criminal Leader", "'s"},  // Use ...'s Hotwire ability
    {2767u, 2769u, "the Criminal leader", ""},          // Defend ...
    {2770u, 2772u, "The Space Criminal leader", ""},    // ... is dead.
};

struct LeaderSite {
    uint32_t addr;                       // guest address of the string
    uint32_t len;                        // its shipped length, NUL excluded
    char shipped[kLeaderMaxText + 1];    // the cartridge's bytes, for Off
    char wrote[kLeaderMaxText + 1];      // what this patch put there
};
constexpr int kLeaderMaxSites = 32;      // one language needs at most 11
LeaderSite g_leader_site[kLeaderMaxSites];
int g_leader_sites = 0;
uint32_t g_leader_file = 0u;   // the table the sites were made in
int g_leader_name = 0;         // the name they carry; 0 = none
bool g_leader_done = false;    // every row resolved against that table

}  // namespace

// The live table's block, or 0 while none is settled: no load in flight, the
// language loaded is the one asked for, the magic is there, and the offsets
// have been turned into pointers.
static uint32_t leader_table(void) {
    if (bus_read_u16(kLangManager) != 0u) return 0u;
    if (bus_read_u32(kLangManager + 4u) != bus_read_u32(kLangManager + 8u))
        return 0u;
    const uint32_t file = bus_read_u32(kLangManager + 0x0Cu);
    if (file < 0x02000000u || file > 0x02400000u - kLangMaxBytes) return 0u;
    if (bus_read_u32(file) != kLangMagic) return 0u;
    if (bus_read_u32(file + 8u) != file + 0x0Cu) return 0u;
    return file;
}

// Where string `id` is, or 0 while its entry is not yet a pointer into the
// text (func_0203E0B4 can be part-way through the table).
static uint32_t leader_string(uint32_t file, uint32_t id) {
    const uint32_t at = bus_read_u32(file + 0x0Cu + id * 4u);
    return (at >= file + kLangStrings && at < file + kLangMaxBytes) ? at : 0u;
}

// Copy the NUL-terminated string at `addr` out of guest RAM. False if it runs
// past kLeaderMaxText, which nothing this patch edits comes near.
static bool leader_read(uint32_t addr, char* out) {
    for (size_t i = 0; i <= kLeaderMaxText; ++i) {
        out[i] = (char)bus_read_u8(addr + (uint32_t)i);
        if (out[i] == '\0') return true;
    }
    return false;
}

static bool leader_known(uint32_t addr) {
    for (int i = 0; i < g_leader_sites; ++i)
        if (g_leader_site[i].addr == addr) return true;
    return false;
}

// Replace live[at, at + cut) with name + tail, in place. The result must fit
// where the shipped text was; it is NUL-terminated and the rest of the old
// storage is zeroed, so a reader resumed part-way along the string still stops.
static void leader_edit(uint32_t addr, const char* live, size_t at, size_t cut,
                        const char* name, const char* tail) {
    const size_t len = std::strlen(live);
    char text[kLeaderMaxText + 1];
    const int n = std::snprintf(text, sizeof text, "%.*s%s%s%s", (int)at, live,
                                name, tail, live + at + cut);
    if (n < 0 || (size_t)n > len || std::strcmp(text, live) == 0) return;
    if (g_leader_sites >= kLeaderMaxSites) return;
    LeaderSite& s = g_leader_site[g_leader_sites++];
    s.addr = addr;
    s.len = (uint32_t)len;
    std::memcpy(s.shipped, live, len + 1);
    std::memcpy(s.wrote, text, (size_t)n + 1);
    for (size_t i = 0; i < len; ++i)
        bus_write_u8(addr + (uint32_t)i,
                     (uint8_t)(i < (size_t)n ? text[i] : '\0'));
}

// Put the cartridge's bytes back wherever this patch's text is still what is
// there, then forget every site.
static void leader_restore(void) {
    char live[kLeaderMaxText + 1];
    for (int i = 0; i < g_leader_sites; ++i) {
        const LeaderSite& s = g_leader_site[i];
        if (!leader_read(s.addr, live) || std::strcmp(live, s.wrote) != 0)
            continue;
        for (uint32_t k = 0; k < s.len; ++k)
            bus_write_u8(s.addr + k, (uint8_t)s.shipped[k]);
    }
    g_leader_sites = 0;
}

static void apply_leader_name(void) {
    const int want = lb_setting_criminal_leader_name();   // 0 off, 1, 2
    if (want == 0 && g_leader_sites == 0) {
        g_leader_file = 0u;     // nothing of ours in guest RAM: read nothing
        g_leader_name = 0;
        return;
    }

    const uint32_t file = leader_table();
    if (file != g_leader_file) {
        // Freed, reloading, or another language: the sites belong to a block
        // that is no longer the table, so they are dropped, not restored.
        g_leader_sites = 0;
        g_leader_name = 0;
        g_leader_done = false;
        g_leader_file = file;
    }
    if (file == 0u) return;

    // A site whose text is no longer ours was reloaded under us: drop it, and
    // let the pass below look at that string again.
    char live[kLeaderMaxText + 1];
    for (int i = 0; i < g_leader_sites;) {
        if (leader_read(g_leader_site[i].addr, live) &&
            std::strcmp(live, g_leader_site[i].wrote) == 0) {
            ++i;
            continue;
        }
        if (i != --g_leader_sites) g_leader_site[i] = g_leader_site[g_leader_sites];
        g_leader_done = false;
    }

    if (want != g_leader_name) {
        const int undone = g_leader_sites;
        leader_restore();
        g_leader_name = want;
        g_leader_done = false;
        if (undone > 0) {
            std::fprintf(stderr, "[lb_leader] shipped text restored in %d "
                                 "string(s)\n", undone);
            std::fflush(stderr);
        }
    }
    if (want == 0 || g_leader_done) return;

    const char* name = (want == 1) ? "Kranxx" : "Plisken";
    const int before = g_leader_sites;
    bool resolved = true;
    for (uint32_t id : kLeaderNameIds) {
        const uint32_t addr = leader_string(file, id);
        if (addr == 0u) { resolved = false; continue; }
        if (leader_known(addr) || !leader_read(addr, live)) continue;
        for (const char* shipped : kLeaderShippedNames) {
            if (std::strcmp(live, shipped) != 0) continue;
            leader_edit(addr, live, 0u, std::strlen(live), name, "");
            break;
        }
    }
    for (const LeaderEdit& e : kLeaderEdits) {
        for (uint32_t id = e.first; id <= e.last; ++id) {
            const uint32_t addr = leader_string(file, id);
            if (addr == 0u) { resolved = false; continue; }
            if (leader_known(addr) || !leader_read(addr, live)) continue;
            if (const char* hit = std::strstr(live, e.needle))
                leader_edit(addr, live, (size_t)(hit - live),
                            std::strlen(e.needle), name, e.tail);
        }
    }
    g_leader_done = resolved;
    if (g_leader_sites != before) {
        std::fprintf(stderr, "[lb_leader] \"%s\": %d string(s) rewritten in the "
                             "language table at 0x%08X\n",
                     name, g_leader_sites - before, file);
        std::fflush(stderr);
    }
}

// ---- docs/392: THE PIRATES' PLANTATION, RENAMED -------------------------------
//
// The option renames the Pirate structure 'Plantation' to 'Hide-Away' or
// 'Storehouse'. String 445 is blueprint 53 (P_ShantyShack, the Pirates' farm)
// by its +0x06 name id: the build menu's tooltip (FUN_020DCD34, 0x020DCF6A)
// and the info panel (FUN_020D33D8, 0x020D3970) both fetch it through
// FUN_0203E1D4. 866-868 are "Build a Plantation", one string shared by three
// ids. No other id holds the word, in any language, and no other id points
// into those two strings, so they are rewritten in place exactly as
// apply_leader_name does: the shipped bytes kept for Off, the sites dropped
// when the language block changes. The word is per language, by the loaded
// language's number at 0x0215378C+8 (1 US, 2 UK, 3 French, 4 Italian, 5
// German, 6 Spanish, 7 Danish, 8 Japanese.lng, whose text is English); a
// match by content could not tell French "Plantation" from English. Every
// replacement is no longer than the string it replaces (UTF-8, as the files
// are).
namespace {
struct PlantWords { const char* name[2]; const char* build[2]; };
// [language 0..8] -> {Hide-Away, Storehouse} name and objective.
const PlantWords kPlantWords[9] = {
    {{nullptr, nullptr}, {nullptr, nullptr}},
    {{"Hide-Away", "Storehouse"}, {"Build a Hide-Away", "Build a Storehouse"}},
    {{"Hide-Away", "Storehouse"}, {"Build a Hide-Away", "Build a Storehouse"}},
    {{"Cachette", "Entrep\xC3\xB4t"},
     {"Construis une Cachette", "Construis un Entrep\xC3\xB4t"}},
    {{"Rifugio", "Magazzino"},
     {"Costruisci un rifugio.", "Costruisci un magazzino."}},
    {{"Versteck", "Lager"}, {"Erstelle ein Versteck.", "Erstelle ein Lager."}},
    {{"Escondite", "Dep\xC3\xB3sito"},
     {"Construye un escondite", "Construye un dep\xC3\xB3sito"}},
    {{"Skjul", "Lagerhus"}, {"Byg et skjul", "Byg et lagerhus"}},
    {{"Hide-Away", "Storehouse"}, {"Build a Hide-Away", "Build a Storehouse"}},
};
constexpr uint32_t kPlantNameId = 445u;
constexpr uint32_t kPlantBuildIds[3] = {866u, 867u, 868u};
struct PlantSite {
    uint32_t addr = 0, len = 0;
    char shipped[64] = {};
    char wrote[64] = {};
};
PlantSite g_plant_site[4];
int g_plant_sites = 0;
uint32_t g_plant_file = 0u;
int g_plant_name = 0;
bool g_plant_done = false;

bool plant_read(uint32_t addr, char* out, size_t cap) {
    for (size_t i = 0; i < cap; ++i) {
        out[i] = (char)bus_read_u8(addr + (uint32_t)i);
        if (out[i] == '\0') return true;
    }
    return false;
}

void plant_restore() {
    char live[64];
    for (int i = 0; i < g_plant_sites; ++i) {
        const PlantSite& s = g_plant_site[i];
        if (!plant_read(s.addr, live, sizeof live) || std::strcmp(live, s.wrote))
            continue;
        for (uint32_t k = 0; k < s.len; ++k)
            bus_write_u8(s.addr + k, (uint8_t)s.shipped[k]);
    }
    g_plant_sites = 0;
}

// Rewrite the string at addr to `text` if it fits; remember the shipped bytes.
void plant_edit(uint32_t addr, const char* text) {
    char live[64];
    if (!plant_read(addr, live, sizeof live) || g_plant_sites >= 4) return;
    for (int i = 0; i < g_plant_sites; ++i)
        if (g_plant_site[i].addr == addr) return;   // 866..868 share one
    const size_t len = std::strlen(live), n = std::strlen(text);
    if (n > len || std::strcmp(live, text) == 0) return;
    PlantSite& s = g_plant_site[g_plant_sites++];
    s.addr = addr;
    s.len = (uint32_t)len;
    std::memcpy(s.shipped, live, len + 1);
    std::memcpy(s.wrote, text, n + 1);
    for (size_t i = 0; i < len; ++i)
        bus_write_u8(addr + (uint32_t)i, (uint8_t)(i < n ? text[i] : '\0'));
}
}  // namespace

static void apply_plantation_name(void) {
    const int want = lb_setting_plantation_name();   // 0 off, 1, 2
    if (want == 0 && g_plant_sites == 0) {
        g_plant_file = 0u;
        g_plant_name = 0;
        return;
    }
    const uint32_t file = leader_table();
    if (file != g_plant_file) {        // freed, reloading or another language
        g_plant_sites = 0;
        g_plant_name = 0;
        g_plant_done = false;
        g_plant_file = file;
    }
    if (file == 0u) return;
    // A site reloaded under us: drop it so the pass below looks again.
    char live[64];
    for (int i = 0; i < g_plant_sites;) {
        if (plant_read(g_plant_site[i].addr, live, sizeof live) &&
            std::strcmp(live, g_plant_site[i].wrote) == 0) {
            ++i;
            continue;
        }
        if (i != --g_plant_sites) g_plant_site[i] = g_plant_site[g_plant_sites];
        g_plant_done = false;
    }
    if (want != g_plant_name) {
        plant_restore();
        g_plant_name = want;
        g_plant_done = false;
    }
    if (want == 0 || g_plant_done) return;
    const uint32_t lang = bus_read_u32(kLangManager + 8u);
    if (lang < 1u || lang > 8u) return;
    const PlantWords& w = kPlantWords[lang];
    const int before = g_plant_sites;
    bool resolved = true;
    if (const uint32_t a = leader_string(file, kPlantNameId))
        plant_edit(a, w.name[want - 1]);
    else
        resolved = false;
    for (uint32_t id : kPlantBuildIds) {
        if (const uint32_t a = leader_string(file, id))
            plant_edit(a, w.build[want - 1]);
        else
            resolved = false;
    }
    g_plant_done = resolved;
    if (g_plant_sites != before) {
        std::fprintf(stderr, "[lb_plantation] \"%s\": %d string(s) rewritten "
                     "(language %u, table 0x%08X)\n", w.name[want - 1],
                     g_plant_sites - before, lang, file);
        std::fflush(stderr);
    }
}

// ── docs/394: the map being loaded, by its own name ───────────────────────
//
// For the master server's log ("started a match on The Pond"). The stem the
// loader is handed is "mpNN" for a stock multiplayer map, a campaign stem, or
// "z/NN" for a mod map (docs/391). A stock stem is looked up in the mission
// table (0x02147DDC: char* stem at +0, name id at +8) and its name read from
// the loaded LOC file; a mod map's name is lb_mods'. Anything else, or a LOC
// file still loading, is reported as the stem itself. Observer only.
namespace {
char g_map_name[64] = "";
unsigned g_map_gen = 0;

// (in the unnamed namespace, where the override table's declaration is)
void map_load_observer(void) {
    void (*original)(void) = lb_dispatch_original(0, kMapLoad);
    const uint32_t str = g_cpu.R[0];
    char stem[24] = "";
    if (guest_ptr(str)) {
        const uint32_t text = bus_read_u32(str + 8u);
        if (guest_ptr(text))
            for (uint32_t i = 0; i + 1 < sizeof stem; ++i) {
                stem[i] = (char)bus_read_u8(text + i);
                if (!stem[i]) break;
            }
    }
    if (stem[0]) {
        std::snprintf(g_map_name, sizeof g_map_name, "%s", stem);
        if (stem[0] == 'z' && stem[1] == '/') {
            const int k = std::atoi(stem + 2) - 1;
            if (k >= 0 && k < lb_mods_count() && lb_mods_name(k))
                std::snprintf(g_map_name, sizeof g_map_name, "%s", lb_mods_name(k));
        } else if (const uint32_t file = leader_table()) {
            for (uint32_t e = 0; e < 30u; ++e) {
                const uint32_t ent = kMissionTable + 28u * e;
                const uint32_t s = bus_read_u32(ent);
                if (!guest_ptr(s)) continue;
                bool same = true;
                for (uint32_t i = 0; i < sizeof stem; ++i) {
                    const char c = (char)bus_read_u8(s + i);
                    if (c != stem[i]) { same = false; break; }
                    if (!c) break;
                }
                if (!same) continue;
                char name[kLeaderMaxText + 1];
                if (const uint32_t a = leader_string(file, bus_read_u32(ent + 8u)))
                    if (leader_read(a, name) && name[0])
                        std::snprintf(g_map_name, sizeof g_map_name, "%s", name);
                break;
            }
        }
        ++g_map_gen;
        std::fprintf(stderr, "[lb_net] map loading: %s (\"%s\")\n", stem,
                     g_map_name);
        std::fflush(stderr);
    }
    if (original) original();
}
}  // namespace

extern "C" const char* lb_level_map_name(void) { return g_map_name; }
extern "C" unsigned lb_level_map_gen(void) { return g_map_gen; }

// LB_FIELD_DUMP=<path.ppm> renders the whole battlefield once, so the C++
// locator and renderer can be checked against tools/lb_field.py's output
// headlessly. Test aid only, in the same spirit as LB_BALANCE and LB_CHEATS.
static void field_dump_if_asked(void) {
    static int state = -1;               // -1 unread, 0 disabled, 1 pending
    static const char* path = nullptr;
    static bool located = false;
    static LbFieldInfo info;
    static int last_cx = -9999, last_cy = -9999, seq = 0;
    if (state < 0) {
        path = std::getenv("LB_FIELD_DUMP");
        state = path ? 1 : 0;
    }
    if (state != 1) return;

    if (!located) {
        if (!lb_field_locate(&info)) return;   // level not up yet
        located = true;
    }
    // Learn every frame, not only when a dump fires. A unit is almost never
    // inside the camera at the exact moment a view happens to be drawn, so
    // learning on that schedule captured nothing at all.
    lb_field_learn_sprites(&info);
    // Re-dump whenever the camera moves to a new cell, so the offset between
    // entity space and map space can be checked at more than one position --
    // one measurement could not tell a constant from a coincidence.
    // The camera now comes from the game's own state (docs/174) rather than from
    // matching terrain, so this works over unexplored ground too -- which is
    // exactly where the terrain tracker gave up.
    static int tick = 0;
    if (++tick % 2) return;
    int gcx = 0, gcy = 0;
    if (!lb_field_camera(&gcx, &gcy)) return;
    if (gcx - last_cx < 64 && last_cx - gcx < 64 &&
        gcy - last_cy < 64 && last_cy - gcy < 64) return;
    std::fprintf(stderr, "[lb_field] camera map px (%d,%d)\n", gcx, gcy);
    last_cx = gcx; last_cy = gcy;
    ++seq;

    int w = 0, h = 0;
    lb_field_size(&info, &w, &h);
    if (w <= 0 || h <= 0) { state = 0; return; }

    std::vector<uint32_t> px((size_t)w * (size_t)h);
    if (!lb_field_render(&info, px.data())) { state = 0; return; }

    // Put the guest's live field in place first, so the box and markers are
    // drawn over the finished picture rather than under it.
    lb_field_composite_guest(&info, px.data());

    int cx = 0, cy = 0;
    lb_field_camera_rect(&info, &cx, &cy);

    // Draw exactly what the Battlefield panel draws, so the dump verifies the
    // panel's data path and not just the terrain: the camera rectangle, and a
    // box per live entity coloured by owner.
    auto plot = [&](int x, int y, uint32_t c) {
        if (x >= 0 && x < w && y >= 0 && y < h) px[(size_t)y * (size_t)w + (size_t)x] = c;
    };
    auto rect = [&](int x0, int y0, int x1, int y1, uint32_t c) {
        for (int x = x0; x <= x1; ++x) { plot(x, y0, c); plot(x, y1, c); }
        for (int y = y0; y <= y1; ++y) { plot(x0, y, c); plot(x1, y, c); }
    };
    rect(cx, cy, cx + 255, cy + 191, 0xFFFFFFFFu);
    rect(cx + 1, cy + 1, cx + 254, cy + 190, 0xFFFFFFFFu);
    static const uint32_t kOwnerCol[4] = {0xFFFF4646u, 0xFF46A0FFu,
                                          0xFF50DC50u, 0xFFF0DC3Cu};
    int eo = 0, ec = 0, ex = 0, ey = 0, nents = 0;
    for (int i = 0; lb_field_entity_at(i, &eo, &ec, &ex, &ey); ++i) {
        const int r = (ec >= 7) ? 8 : 4;
        const uint32_t c = kOwnerCol[eo & 3];
        rect(ex - r, ey - r, ex + r, ey + r, c);
        rect(ex - r + 1, ey - r + 1, ex + r - 1, ey + r - 1, c);
        ++nents;
    }
    std::fprintf(stderr, "[lb_field] %d entities marked\n", nents);
    std::fprintf(stderr,
                 "[lb_field] primary=%08X second=%08X count=%d grid=%08X "
                 "%dx%d phase=(%d,%d) cam_cell=(%d,%d) cam_px=(%d,%d) -> %dx%d\n",
                 info.primary_table, info.secondary_table, info.table_count,
                 info.grid_base, info.grid_w, info.grid_h, info.phase_x,
                 info.phase_y, info.cam_cell_x, info.cam_cell_y, cx, cy, w, h);

    char outpath[512];
    std::snprintf(outpath, sizeof(outpath), "%s.%02d.ppm", path, seq);
    if (FILE* f = std::fopen(outpath, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        std::vector<uint8_t> row((size_t)w * 3u);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const uint32_t c = px[(size_t)y * (size_t)w + (size_t)x];
                row[(size_t)x * 3 + 0] = (uint8_t)(c >> 16);
                row[(size_t)x * 3 + 1] = (uint8_t)(c >> 8);
                row[(size_t)x * 3 + 2] = (uint8_t)(c);
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        std::fprintf(stderr, "[lb_field] wrote %s\n", outpath);
    }
    // Keep sampling until enough positions have been captured. This used to be
    // an unconditional `state = 0`, which disarmed the dump after the FIRST
    // image -- and made every later "the camera is not tracking" investigation
    // chase a tracker that was never being called again.
    if (seq >= 6) state = 0;
}

static void apply_cheats(void) {
    // Enabling must be STICKY, not one-shot. The first version wrote only when
    // the desired value changed, which looked right and failed in practice: the
    // flags block is re-initialised when a battle starts, so a cheat set at boot
    // was silently zeroed and never rewritten. Comparing against the guest byte
    // instead survives that.
    //
    // Turning a cheat OFF is still a one-shot write, so the cartridge's own
    // cheat screen stays usable -- the port only forces a byte it is being
    // asked to hold on.
    static int8_t last_want[32] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                                   -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                                   -1,-1,-1,-1};
    static bool announced = false;
    const uint32_t flags = bus_read_u32(kCheatFlagsPtr);
    if (flags < 0x02000000u || flags >= 0x02400000u) return;   // not up yet
    const int n = lb_setting_cheat_count();
    for (int i = 0; i < n && i < 32; ++i) {
        const int8_t want = (int8_t)(lb_setting_cheat(i) ? 1 : 0);
        if (want) {
            if (bus_read_u8(flags + (uint32_t)i) != 1u) {
                bus_write_u8(flags + (uint32_t)i, 1u);
                if (!announced) {
                    announced = true;
                    std::fprintf(stderr,
                                 "[lb_cheat] flags block at 0x%08X; holding "
                                 "cheat %d on\n", flags, i);
                }
            }
        } else if (last_want[i] == 1) {
            bus_write_u8(flags + (uint32_t)i, 0u);
        }
        last_want[i] = want;
    }
}

// The recompiled banks gate a per-instruction hook on this. It only bumps a
// retired-instruction counter, but it is a cross-translation-unit call on every
// emulated instruction, so it cannot inline. LB_NO_INSN_HOOK=1 clears the gate
// to MEASURE what that costs -- three code-reading guesses at the frame budget
// have already been wrong, so this one gets an A/B test instead of an argument.
extern "C" unsigned g_runtime_insn_trace;

static void apply_insn_hook_policy(void) {
    static int off = -1;
    // Default OFF in this port. The hook exists to keep insn9/insn7 retired
    // counters live for run_to_event, traversal and selftest -- debug
    // facilities, none of which run while someone is playing -- and it has
    // no control-flow or cycle effect, so clearing it changes timing only.
    // Measured at 34.7 fps on / 37.4 fps off in the same scene.
    // LB_INSN_HOOK=1 puts it back for anything that needs the ordinals.
    if (off < 0) off = std::getenv("LB_INSN_HOOK") ? 0 : 1;
    if (off) g_runtime_insn_trace = 0u;   // reset() re-arms it, so every frame
}

extern "C" void lb_widemap_fill(void);   /* src/lb_widemap.cpp */
extern "C" void lb_music_tick(void);     /* src/lb_music.cpp */
extern "C" void lb_demo_tick(void);      /* src/lb_demo.cpp */
extern "C" void lb_demo_apply_env(void);
extern "C" void lb_demo_note_extended(int x);
extern "C" void lb_demo_note_extended_y(int y);   /* docs/383 */

// ---- touch beyond the original aspect (interaction) -----------------------
//
// GOAL: the touch ability must extend beyond the original aspect: clicking,
// selecting units, commanding units, click-and-drag select, and clicking HUD
// elements. HUD elements are handled in lb_hud.cpp by translating the click
// back to where the cartridge thinks the button is. This is the WORLD half: a
// unit standing in a widened column is at a screen x the DS panel cannot
// express, so the coordinate itself has to reach the game.
//
// The hardware cannot carry it -- nds_set_touch does `x << 4` into a 12-bit
// field, so 300 would overflow and wrap to a wrong IN-RANGE value, worse than
// dropping the click. And an external poke loses: measured, the game rewrites
// its touch state within 50 ms.
//
// WHERE THE TOUCH ACTUALLY LIVES (docs/214, found with the watchpoint under
// deep_trace). One struct at 0x02153760:
//
//     +0x08  Y previous     +0x0A  X current
//     +0x0C  Y current      +0x0E  X previous
//
// written at 0203DD14 and read by the TAP path at 0203DE4C and the DRAG path at
// 0203DE9C -- the latter computing |dx| + |dy| against a 3-px threshold, which
// is the click-and-drag select the goal names.
//
// TWO PROBLEMS, TWO ONE-INSTRUCTION FIXES.
//
// 1. The game samples X from its own stack. Repointing that load at a slot the
//    port owns means the game's OWN writer copies the port's value into +0x0A,
//    so there is no race at all -- nothing but the port writes the slot.
//
//        0203DD14  ldrh  r2, [sp]  ->  ldrsh r2, [r0, #0x18]
//
//    r0 already holds the struct base there, which is what makes it fit.
//
// 2. Every reader uses `ldrh` -- UNSIGNED -- so a negative X (a click in the
//    LEFT widened column) would read as 65491 and every subtraction after it
//    would be wrong. LDRH and LDRSH differ by one nibble, B -> F.
//
// THE SLOT IS +0x18, NOT +0x10. The plan said +0x10; measurement said no. With
// the touch screen actually being driven, 0x02153770 (+0x10) changes -- as do
// +0x12 and +0x14 -- while 0x02153778 (+0x18) does not. The first version of
// that probe sat still for 40 s and called the touch X itself "free", because
// with no touch the touch code never runs; the known-written fields are kept in
// its candidate list as a control precisely so that cannot pass unnoticed.
const uint32_t kTouchBase = 0x02153760u;
const uint32_t kTouchSlot = kTouchBase + 0x18u;   // measured unwritten
// AND THE VERTICAL SLOT, chosen the same way (docs/284).
//
// 600 pen-down samples on a recorded route, with +0x0A and +0x0C carried as
// CONTROLS so a sample taken while the touch code never ran could not pass:
// both controls moved, and +0x1A, +0x1C, +0x1E, +0x20 and +0x22 all stayed
// still. +0x1A is the adjacent halfword to the x shadow, so it is the one used.
//
// Evidence, not proof: that is one battle and one input pattern, and a field
// written only on a path this route never takes would read as still. The same
// caveat has applied to +0x18 for a long time without trouble.
const uint32_t kTouchYSlot = kTouchBase + 0x1Au;  // measured unwritten

struct TouchPatch { uint32_t addr, stock, want; const char* what; };

// WHICH HALFWORDS DOES THIS ENTRY ACTUALLY OWN?
//
// THIS EXISTS BECAUSE THE TWO PATCH TABLES COLLIDED AND THE TOUCH SET LOST.
//
// kTouchPatches is written as 32-bit words, because ARM instructions are, but
// several of its entries sit on THUMB code and change only one of the two
// halfwords in the word they name. kOrderFix is written as halfwords and owns
// the other half of two of them:
//
//     020AF29C  kTouchPatches low half   7A29 -> 89E9   (pick reads x)
//     020AF29E  kOrderFix group 128      7A6A -> 8A6A   (pick reads y)
//     020AF220  kTouchPatches low half   79A9 -> ...    (press x)
//     020AF222  kOrderFix group 128      79EA -> 8AAA   (press y)
//
// kOrderFix applies first. The readiness test above then compared the FULL
// word against a stock value that no longer described it, `ok` could never
// reach `total`, and the entire touch set -- ALL THIRTY SITES -- was refused
// forever. Measured, not deduced:
//
//     [lb_touch] STILL WAITING: 28 of 30 sites match ... after 10s
//     [lb_touch]   020AF29C reads 8A6A7A29, expected 7A6A7A29
//     [lb_touch]   020AF220 reads 8AAA79A9, expected 79EA79A9
//
// The cost was far larger than the two sites named. 0200A3C4 is in this table
// -- the `movhi r5,#0xFB` that pins every out-of-range click to column 251 --
// so with the set refused, EVERY click outside the native panel landed at the
// native edge. That is exactly the observed defect: a click outside the
// original aspect just moved to the edge of the original aspect.
// The defect was not in the touch widening; it was that the touch widening
// had been switched off by an unrelated patch table since group 128 was
// enabled by default.
//
// THE FIX IS ORDER-INDEPENDENT ON PURPOSE. Deciding to apply kTouchPatches
// first would work today and break the next time a table is added or the
// order changes. Instead each entry owns exactly the halfwords it modifies:
// `stock ^ want` names the bits that differ, widened to halfword granularity
// because that is the unit the other table works in. The check tests only
// those, and the write preserves the rest.
//
// WHAT THIS GIVES UP, stated plainly: the readiness test no longer proves the
// untouched halfword is the expected build. It still proves every bit the
// entry changes is exactly the stock encoding, which is the part that makes
// the patch safe to apply -- and it is what kOrderFix has always checked for
// its own sites.
static inline uint32_t touch_mask(const TouchPatch& p) {
    const uint32_t diff = p.stock ^ p.want;
    uint32_t m = 0u;
    if (diff & 0x0000FFFFu) m |= 0x0000FFFFu;
    if (diff & 0xFFFF0000u) m |= 0xFFFF0000u;
    // An entry that changes nothing would mask to zero and pass vacuously.
    // None exists, but a future no-op entry should be checked, not skipped.
    return m ? m : 0xFFFFFFFFu;
}
// THE TOUCH PUMP POSTS ITS EVENT TWICE, AND ONLY ONE POST WAS PATCHED.
//
// This table was five sites for a long time and the band still would not
// select. docs/230-232 narrowed the fault to "a negative x lands at column 255"
// -- not refused, relocated -- and the sixth site is why.
//
// func_0203DCB8 builds a touch event and posts it in TWO places, with byte-for
// byte identical instruction pairs:
//
//   0203DE40  BL 020097F8            get the event queue
//   0203DE4C  LDRH r1, [r2, #0x0A]   X          <- patched
//   0203DE50  LDRH r2, [r2, #0x0C]   Y
//   0203DE58  BL 0200A350            build the event
//
//   0203DF4C  BL 020097F8
//   0203DF58  LDRH r1, [r2, #0x0A]   X          <- NOT patched
//   0203DF5C  LDRH r2, [r2, #0x0C]   Y          <- NOT patched
//   0203DF64  BL 0200A350
//
// r2 comes from the literal at 0203DF88, which is 0x02153760 -- kTouchBase
// itself, so there is no doubt about what is being read. The second post is
// the one with a constant event kind of 3, the held/move event.
//
// LDRH zero-extends, so a native x of -150 reaches the event constructor as
// 65386, and something downstream saturates that to the panel width. That is
// exactly the measured behaviour: the click lands at column 255 whatever
// negative value was clicked, bracketed by parking the base at 140..240 and
// clicking the same -150 (docs/232).
//
// The Y halves of both posts are added at the same time. They are inert today,
// because the port does not yet publish an extended Y and the hardware sample
// is clamped to 0..191 -- but the surface is now taller than the DS's own
// picture, so a click above the field is the same bug waiting, and a value of
// 0..191 sign-extends to itself.
const TouchPatch kTouchPatches[] = {
    {0x0203DD14u, 0xE1DD20B0u, 0xE1D021F8u, "writer sources X from the port"},
    // THE Y TWIN OF THE WRITER REPOINT -- the one thing the vertical axis was
    // still missing, and the reason the symptom survived the shadow work:
    // a click outside the original aspect just moved to its edge.
    //
    // The whole Y effort so far published to the RECOGNISER (obj+0x12/+0x14),
    // which serves the gesture path -- tap/drag classification and the select
    // rectangle. It does not serve the ORDER path. func_020A8794 reads the
    // click it turns into a command straight out of the touch base:
    //
    //   020A87F8  ldr   r1, [pc, #0x30]   = 0x0215376C
    //   020A87FC  ldrsh r2, [r1, r0]      y   <- ALREADY SIGNED IN STOCK
    //   020A87FE  ldr   r1, [pc, #0x30]   = 0x0215376A
    //   020A8800  ldrsh r1, [r1, r0]      x   <- fed by the repoint above
    //
    // So the order path was reading an x the port had widened and a y the
    // hardware had clamped to 0..191. A click above the band arrived at row 0
    // and the order landed at the native top edge -- exactly the report.
    //
    // The instruction pair is the same shape as X's, four words apart:
    //
    //   0203DD14  ldrh r2, [sp]       -> ldrsh r2, [r0, #0x18]   (shipped)
    //   0203DD1C  strh r2, [r0, #0xa]
    //   0203DD20  ldrh r2, [sp, #2]   -> ldrsh r2, [r0, #0x1a]   (this)
    //   0203DD24  strh r2, [r0, #0xc]
    //
    // r0 already holds the struct base, which is what let X's fit and lets
    // this one fit. LDRSH because the row above the band is NEGATIVE; the
    // store at 0203DD24 is already a halfword, so nothing truncates after it.
    //
    // NOTHING ELSE CLAMPS THE Y, and that was checked rather than assumed:
    // FUN_0200A350's `cmp/movlo #4` + `cmp/movhi #0xFB` pair operates on r5,
    // which is the X -- the Y rides in r4 and reaches func_0200AA2C untouched.
    // And the field-click gate at 020A87D8 tests [r4,#0x1D], the high byte of
    // the event's X, with NO sibling test on [r4,#0x1F] for the Y.
    {0x0203DD20u, 0xE1DD20B2u, 0xE1D021FAu, "writer sources Y from the port"},
    {0x0203DE4Cu, 0xE1D210BAu, 0xE1D210FAu, "tap X sign-extended"},
    {0x0203DE50u, 0xE1D220BCu, 0xE1D220FCu, "tap Y sign-extended"},
    {0x0203DE9Cu, 0xE1D050BAu, 0xE1D050FAu, "drag X current sign-extended"},
    {0x0203DEA0u, 0xE1D010BEu, 0xE1D010FEu, "drag X previous sign-extended"},
    {0x0203DEFCu, 0xE1D010BAu, 0xE1D010FAu, "X carry-forward sign-extended"},
    {0x0203DF58u, 0xE1D210BAu, 0xE1D210FAu,
     "event-post X sign-extended (the sixth site)"},
    {0x0203DF5Cu, 0xE1D220BCu, 0xE1D220FCu, "event-post Y sign-extended"},
    // AND THE CLAMP ITSELF, which is what actually moved the click.
    //
    // FUN_0200A350 builds the touch event. Its second argument is the x, held
    // in r5, and before the event is constructed it does this:
    //
    //   0200A3B8  cmp   r5, #4      E3550004
    //   0200A3BC  movcc r5, #4      33A05004
    //   0200A3C0  cmp   r5, #0xFB   E35500FB      251
    //   0200A3C4  movhi r5, #0xFB   83A050FB
    //
    // `movhi` is UNSIGNED higher, so it does not merely bound large positives
    // -- a negative x is a very large unsigned number and takes the same
    // branch. Every out-of-range click, in either direction, arrives at the
    // pick as 251.
    //
    // That is exactly what was measured before this was found: clicking native
    // -150 and clicking native +300 both select a base parked at 240 and
    // neither selects one at 200 or below, and the landing point brackets to
    // "a bit under 255" (docs/232). 251 fits the bracket precisely.
    //
    // ONLY THE UPPER MOV IS REMOVED. The lower one is left alone: it fires on
    // an unsigned compare against 4, which a negative x never satisfies, so it
    // is vanilla behaviour for a real stylus and irrelevant here. And the
    // upper bound is not replaced with a wider constant because the right
    // bound depends on the window: the port already bounds this coordinate
    // itself, since the only thing that writes the slot is the enhanced touch
    // mapping, which cannot produce an x outside the surface it just drew.
    //
    // E1A00000 is `mov r0, r0`, the classic ARM no-op. r0 is dead here -- the
    // next instruction but one loads it with the allocation size.
    {0x0200A3C4u, 0x83A050FBu, 0xE1A00000u,
     "event x no longer clamped to 4..251"},
    // AND THE SECOND TRUNCATION, in the pick's own widget hit test.
    //
    // With the clamp gone the true coordinate reaches the event object -- a
    // value-filtered watch shows 300 stored by 0200AA2C, which never happened
    // while the clamp was live -- and the next thing to touch it is
    // FUN_020E489C, which passes it to the widget hit test like this:
    //
    //   020E498E  8BB8  ldrh r0, [r7, #0x1C]    event x
    //   020E4990  8BF9  ldrh r1, [r7, #0x1E]    event y
    //   020E4994  0600  lsl  r0, r0, #24
    //   020E4996  0609  lsl  r1, r1, #24
    //   020E4998  0E00  lsr  r0, r0, #24        zero-extend the low byte
    //   020E499A  0E09  lsr  r1, r1, #24
    //   020E499E  bl    020E3EAC
    //
    // `lsl #24` then `lsr #24` is `& 0xFF`. So a click at native -150 is
    // offered to the hit test as 106 and one at +300 as 44 -- both plausible
    // in-panel positions, so a band click can spuriously hit a HUD button that
    // is nowhere near it.
    //
    // The fix keeps the same two instruction slots and changes the shift
    // amount and the second shift's kind: `lsl #16` then `asr #16` is a 16-bit
    // SIGN extension. ARMv5TE Thumb has no SXTH -- that is ARMv6 -- so the
    // shift pair is not a workaround, it is how this is written on this core.
    //
    // Safe for out-of-range values: FUN_020E3EAC walks a linked list of
    // widgets and calls a containment test on each. It does not index anything
    // by the coordinate, so a negative x simply fails to be inside any
    // rectangle, which is the correct answer for a click out in the band.
    {0x020E4994u, 0x06090600u, 0x04090400u,
     "hit-test x,y: lsl #24 -> lsl #16"},
    {0x020E4998u, 0x0E090E00u, 0x14091400u,
     "hit-test x,y: lsr #24 -> asr #16 (sign extend, not truncate)"},
    // THE SAME TRUNCATION IN THE HELD-BUTTON RE-TEST (docs/371). FUN_020E5C34
    // checks, while a button is held, that the pen is still on it, and passes
    // the event point to the same rectangle test after `& 0xFF`:
    //
    //   020E5C4A  8BA2  ldrh r2, [r4, #0x1C]    event x
    //   020E5C4C  8BE3  ldrh r3, [r4, #0x1E]    event y
    //   020E5C50  0612  lsl  r2, r2, #24 / 020E5C52  061B  lsl  r3, r3, #24
    //   020E5C58  0E12  lsr  r2, r2, #24 / 020E5C5A  0E1B  lsr  r3, r3, #24
    //   020E5C5C  bl    020EA1A4
    //
    // With the widget test corrected for moved elements, a press on a button
    // drawn out in the band arrives with its true x (past 255 or below 0), and
    // the truncated copy would test a different point from the press. The
    // same sign extension as the pair above.
    {0x020E5C50u, 0x061B0612u, 0x041B0412u,
     "held re-test x,y: lsl #24 -> lsl #16"},
    {0x020E5C58u, 0x0E1B0E12u, 0x141B1412u,
     "held re-test x,y: lsr #24 -> asr #16"},
    // THE GATE ITSELF. This is the branch that refuses the click.
    //
    // FUN_020A8794 runs the field-selection path only when four conditions
    // hold, and the fourth is a test on the event's byte at +0x1D:
    //
    //   020A87D8  7F61  ldrb r1, [r4, #0x1D]
    //   020A87DA  2900  cmp  r1, #0
    //   020A87DC  D120  bne  <skip the whole field branch>
    //
    // The event's x is a SIXTEEN BIT field at +0x1C -- FUN_0200AA2C stores it
    // with `strh r5, [r7, #0x1C]` -- so +0x1D is that x's HIGH BYTE, and this
    // is a test that the coordinate fits in 0..255. In vanilla it always does,
    // so the branch is never taken and the test reads as dead code; with the
    // widened field it is taken for every click outside the native panel, in
    // either direction.
    //
    // MEASURED, not deduced. A read watch on 020A87CE and 020A87D8 shows both
    // reads firing for in-band AND band clicks -- so execution reaches this
    // test in both cases -- while the reads inside the branch (020A87FC,
    // 020A8800, 020A880E, 020A8812, which fetch the touch coordinate for the
    // selection rectangle) fire ONLY for in-band clicks. There is nothing
    // between the two but this branch.
    //
    // 46C0 is `mov r8, r8`, the canonical Thumb no-op. The `cmp` above it is
    // left in place: it only sets flags, and nothing after this reads them.
    {0x020A87DCu, 0xA801D120u, 0xA80146C0u,
     "field-click gate: drop the x-fits-in-a-byte test"},
    // THE ROOT CAUSE (docs/234): the pick reads its x from a BYTE.
    //
    //   020AF29C  7A29  ldrb r1,[r5,#0x08]   touch x, from a byte
    //   020AF29E  7A6A  ldrb r2,[r5,#0x09]   touch y
    //   020AF2A2  bl 020F2A52                make point(x, y)
    //   020AF2AA  bl 020A9570                classify what is under it
    //
    // A native x of -85 is stored as 0xAB and read back as 171, so the query
    // lands three hundred pixels from where the player clicked. Measured: the
    // two operands of the world-point add (LDRSH at 02023FEC and 02023FF0)
    // read touch=122 in band and touch=171,203 out of it, and 171 and 203 are
    // the low bytes of -85 and -53.
    //
    // ONLY THE X MOVES. y is 0..191 and a byte holds it, so 020AF29E is left
    // alone. The x moves to the object's free halfword at +0x0E, which the
    // port fills each frame (see lb_touch_publish).
    //
    // A ZERO-EXTENDING LOAD IS ENOUGH, which is what makes this a one-halfword
    // patch instead of a thunk. FUN_020F2A52 stores x as a HALFWORD and
    // 02023FEC reads it back with LDRSH, so 0xFFAB put in the halfword comes
    // out as -85 on the far side. That matters because ARMv5TE Thumb has no
    // immediate-offset LDRSH and no SXTB -- but it does have
    // `LDRH Rd,[Rn,#imm5]`, which is all this needs.
    //
    // 0x89E9 = LDRH r1,[r5,#0x0E]: 0x8800 | (7 << 6) | (5 << 3) | 1.
    {0x020AF29Cu, 0x7A6A7A29u, 0x7A6A89E9u,
     "pick reads x from the free halfword, not a byte"},
    // AND ITS THREE SIBLINGS. The gesture recogniser reads that byte pair in
    // FOUR places, and patching one moved the classifier's query while leaving
    // the click handler's own query on the old value:
    //
    //   band click, world x queried by the classifier   (lr 020A96A9): 259
    //   band click, world x queried by FUN_020E2408     (lr 020E2491): 515
    //
    // 259 is right; 515 is camera + 171, the byte-derived value again. So the
    // rect FUN_020E2408 builds comes through a different one of the four.
    //
    // All four load `[r5,#8]` and `[r5,#9]` from the same recogniser object and
    // mean the same thing -- the current touch position -- so all four get the
    // same treatment, and in each case only the X moves. y stays a byte.
    {0x020AF27Cu, 0x7A2A492Fu, 0x89EA492Fu,
     "tap handler x from the halfword (1 of 3 siblings)"},
    {0x020AF348u, 0x7A2A491Eu, 0x89EA491Eu,
     "move handler x from the halfword (2 of 3)"},
    {0x020AF368u, 0x7A6A7A29u, 0x7A6A89E9u,
     "move handler x from the halfword (3 of 3)"},
    // AND THE PRESS POSITION, which is a SEPARATE byte pair at +6/+7.
    //
    // With the four current-position reads moved, the classifier's query is
    // right (world x 262, the target) and FUN_020E2408's is still wrong (518 =
    // 262 + 256, i.e. -106 read as the unsigned byte 150). Its rect has two
    // corners, and the second one comes from the PRESS position, which the
    // scan above did not cover because it reads `[r5,#6]`/`[r5,#7]`.
    //
    // 020AF220 is inside the press handler, and there the two positions are
    // the same value: FUN_020AF040's press branch sets +6/+7 from the event,
    // copies them to +8/+9, and only then calls it -- while the port has
    // written +0x0E with that same x on the same frame. So reading +0x0E here
    // is not an approximation, it is the identical value with its sign intact.
    //
    // The other press pair, at 020AF3AA, is NOT patched. It sits where press
    // and current can genuinely differ, and substituting one for the other
    // there would collapse a real drag's rectangle -- including in-band, where
    // drag-select works today. That one needs a second free halfword, which a
    // 16-byte object does not have, and is left alone deliberately.
    {0x020AF220u, 0x79EA79A9u, 0x79EA89E9u,
     "press-position x from the halfword"},
    // THE LAST CONSUMER: the selection RECTANGLE, built inside FUN_020AF340.
    //
    // Tracing the byte value itself -- click at native -150 and watch main RAM
    // for writes of 106 (0x6A, its low byte) -- names every place it lands:
    //
    //   020AF06A -> obj+6    the press store
    //   020AF072 -> obj+8    the copy to current
    //   020AF134 -> obj+8    the held store
    //   020AA8EE -> stack    FUN_020AA8E4 writing a rect corner, lr 020AF32D
    //   020AA8F0 -> stack    the other corner
    //
    // FUN_020AA8E4 is the rect builder, and the call at ~020AF328 feeds it from
    // 020AF314 (`ldrb r3,[r5,#6]`, the press x) and 020AF324 (`ldrb r3,[r5,#8]`,
    // the current x). That rect is what FUN_020E2408 finally queries, which is
    // why its world x stayed 256 too high while the classifier's was correct.
    //
    // BOTH READS GO TO +0x0E, AND THAT IS A DELIBERATE COMPROMISE. For a TAP
    // the press and current positions are the same, so a rect built from
    // +0x0E twice is exactly right. For a genuine click-DRAG they differ, and
    // taking the current position for both collapses the rectangle's x extent
    // -- which would break in-band drag-select, where it works today.
    //
    // The proper fix is a second free halfword, and it is one instruction
    // away: FUN_020A8794 allocates this object with `mov r0,#0x10` at
    // 020A87AA, so widening it to 0x18 gives +0x10..+0x17, the port can latch
    // the press x there on pen-down, and 020AF314 can read THAT instead. That
    // is the next change; this pair proves the diagnosis first, because a
    // wrong diagnosis would make the allocation change pointless.
    // The object is eight bytes larger so the press x has its own halfword,
    // and this corner reads THAT rather than the current position -- so a real
    // click-drag keeps its x extent and in-band drag-select is not regressed.
    // 0x8A2B = LDRH r3,[r5,#0x10]: 0x8800 | (8 << 6) | (5 << 3) | 3.
    {0x020A87A8u, 0x2010491Fu, 0x2018491Fu,
     "recogniser object 0x10 -> 0x18 bytes, for the press x"},
    // THE OTHER TWO ALLOCATION SITES, which widening one of three left behind.
    //
    // func_0203b7fc allocates this same object -- same class pointer at
    // 0x02144C2C, same 0x21, same `str r0,[0x02157AF4]` afterwards -- in two
    // more places, and both still asked for sixteen bytes. Whenever the object
    // came from either of them, the port's own
    // `bus_write_u16(obj + 0x10, press_x)` wrote two bytes past the end of the
    // allocation.
    //
    // Nothing has been seen to break, which is the worst way for a heap
    // overwrite to behave: it corrupts whatever the allocator happened to put
    // next and shows up later as something unrelated. Found by disassembling
    // func_0203b7fc for docs/275 and noticing `mov r0,#0x10` twice in a
    // function that was only being read for its byte loads.
    {0x0203B8A4u, 0xE3A00010u, 0xE3A00018u,
     "recogniser allocation 2 of 3 widened to 0x18"},
    {0x0203B8E8u, 0xE3A00010u, 0xE3A00018u,
     "recogniser allocation 3 of 3 widened to 0x18"},
    {0x020AF314u, 0x1C2079ABu, 0x1C208A2Bu,
     "rect press corner x from its own halfword at +0x10"},
    {0x020AF324u, 0x14127A2Bu, 0x141289EBu,
     "rect current corner x from the halfword"},

    // ── THE OTHER ELEVEN ALLOCATIONS. This was a HEAP OVERWRITE. ──────────
    //
    // The port stores a 16-bit press x at recogniser +0x10 and a current x at
    // +0x0E. +0x0E fits inside a 0x10-byte object; +0x10 DOES NOT -- it is the
    // first two bytes PAST the end of a stock allocation.
    //
    // Three allocation sites were widened to 0x18 (above). A full scan of the
    // image for the DragManager construction pattern -- `mov r0,#0x10` /
    // `mov r2,#0x21` / `bl func_0204C6E4` / `blx func_020AF010` -- finds
    // FOURTEEN. The other eleven still allocate 0x10, and the object pointer
    // they publish to 0x02157AF4 is the same one lb_touch_publish writes
    // through. Whenever one of those instances was live, the press-x write ran
    // two bytes past its allocation, over whatever the heap put next.
    //
    // All fourteen are verified present with the stock encoding, and all three
    // sampled sites were confirmed to construct a DragManager rather than some
    // other 0x10-byte object. Widening an allocation cannot corrupt anything on
    // its own -- the cost of being wrong about a site is eight wasted bytes.
    //
    // This also makes room for the vertical shadows the Y axis needs: with the
    // object at 0x18, bytes +0x12..+0x17 are free and unused by the cartridge.
    {0x02039464u, 0xE3A00010u, 0xE3A00018u, "recogniser alloc (func_0203942C 1/3)"},
    {0x020394C0u, 0xE3A00010u, 0xE3A00018u, "recogniser alloc (func_0203942C 2/3)"},
    {0x020394F4u, 0xE3A00010u, 0xE3A00018u, "recogniser alloc (func_0203942C 3/3)"},
    {0x0203B868u, 0xE3A00010u, 0xE3A00018u, "recogniser alloc (func_0203B7FC 1/3)"},
    {0x020ABB5Cu, 0xE3A00010u, 0xE3A00018u, "recogniser alloc (func_020ABB0C 1/3)"},
    {0x020ABBF0u, 0xE3A00010u, 0xE3A00018u, "recogniser alloc (func_020ABB0C 2/3)"},
    {0x020ABCC0u, 0xE3A00010u, 0xE3A00018u, "recogniser alloc (func_020ABB0C 3/3)"},

    // docs/348: THE DRAG EDGE-SCROLL FIRED IN THE MIDDLE OF THE PICTURE.
    //
    // While a drag is in progress (recogniser +4 and +5 set), Task_Input
    // (020ABB0C) scrolls the camera 8 px a frame when the pen is within 8 px of
    // the DS screen's edge:
    //
    //     020ABC20  ldrb r2,[r3,#8]    current x      <= 8 or >= 0xF8
    //     020ABC24  ldrb r3,[r3,#9]    current y      <= 8 or >= 0xB8
    //     020ABCA0  bl   020ABDA0      scroll (-> CamSet 02039038)
    //
    // In the widened window those bytes hold the position the touch PANEL was
    // given, clamped to 0..255 x 0..191, so a pen anywhere out in the widened
    // columns or rows reads as "at the screen edge" and the camera scrolls
    // under the drag. The game's selection keeps its press corner fixed in the
    // WORLD while it scrolls, so its rectangle grew away from the box on
    // screen -- the second half of the drag-select mismatch.
    //
    // Both loads read the recogniser's free bytes +0x16/+0x17 instead (the
    // object is 0x18 bytes, above). lb_touch_publish fills them with a proxy of
    // the pen against the WINDOW's edges: 0 / 255 (0 / 191) within the game's
    // own 8 px of a surface edge, a mid-screen value otherwise -- so the scroll
    // happens where a player expects it, at the edge of what they can see.
    // Encodings verified against the ROM image.
    {0x020ABC20u, 0xE5D32008u, 0xE5D32016u, "drag edge-scroll x from the window-edge proxy"},
    {0x020ABC24u, 0xE5D33009u, 0xE5D33017u, "drag edge-scroll y from the window-edge proxy"},
};

int g_ext_touch_x = INT32_MIN;              // INT32_MIN = "nothing to say"
int g_ext_touch_y = INT32_MIN;              // the vertical twin; same convention

// The native x the runner last delivered, from ANY source -- the frontend
// mapping or the debug `touch` command. Both reach nds_set_touch, which is
// hooked to report it here.
//
// This exists because the writer patch takes the game's own sampled X out of
// the loop: once 0203DD14 reads the port's slot, the sampled value never
// reaches +0x0A on its own, so reading +0x0A back as a "fallback" is circular
// and freezes the coordinate at whatever it last held. The first build of this
// did that and every tap in the route landed at a stale x.
int g_native_touch_x = 0;
// The vertical twin, and it exists for the SAME reason, one axis later. Once
// 0203DD20 reads the port's slot at +0x1A, the sampled Y never reaches +0x0C on
// its own either -- so the Y shadow's old fallback, which read +0x0C back, is
// circular from the moment the writer is repointed. It was NOT circular before
// this change, which is why it shipped: +0x0C was still the guest's own sample.
int g_native_touch_y = 0;
int g_native_touch_down = 0;

// AN EXTENDED COORDINATE BELONGS TO THE DELIVERY IT ARRIVED WITH.
//
// The enhanced mapping calls lb_touch_set_extended immediately before
// nds_set_touch, and nds_set_touch calls this. Other deliveries -- the debug
// server's `touch` command, a demo replaying an in-band tap -- call
// nds_set_touch with no extended coordinate at all, and for those the game must
// be handed the native x.
//
// Without this rule the two paths bleed into each other. Once the release path
// stopped clearing the extended value (so that an out-of-band click still has
// its true x on the frame the cartridge reads one), the last click's extended
// coordinate simply stayed published -- and the very next raw `touch` was
// measured arriving as -14, the x of the deselect click before it, instead of
// the 147 it asked for.
//
// So: a set_extended is good for exactly one delivery. This clears it when a
// delivery arrives without one.
bool g_ext_fresh = false;

extern "C" void lb_touch_note_native(int x, int y, int down) {
    g_native_touch_x = x;
    g_native_touch_y = y;
    g_native_touch_down = down;
    // BOTH axes are cleared here, which they were not before. The rule above
    // -- "a set_extended is good for exactly one delivery" -- was enforced for
    // x only, so a raw `touch` arriving after a band click kept publishing that
    // click's ROW while correctly forgetting its column. Harmless while the
    // extended y went nowhere but the recogniser shadow; not harmless now that
    // it feeds the order path.
    if (!g_ext_fresh) {
        g_ext_touch_x = INT32_MIN;
        g_ext_touch_y = INT32_MIN;
    }
    g_ext_fresh = false;
}

// Called from the enhanced touch mapping with the TRUE screen x, which may be
// negative (left widened column) or past 255 (right one).
// Was the pen already down BEFORE the event being handled right now?
//
// lb_touch_note_native is called from nds_set_touch, so during a frontend event
// handler this still holds the state of the PREVIOUS delivery -- which makes it
// exactly the press-edge test the HUD phantom guard needs. Its caller must
// distinguish a fresh press from a drag continuation, and dropping a drag
// sample would freeze the guest's touch position partway through a selection
// rectangle instead of merely refusing a click.
extern "C" int lb_touch_is_down(void) { return g_native_touch_down; }

extern "C" void lb_touch_set_extended(int x) {
    g_ext_touch_x = x;
    g_ext_fresh = true;                 // good for exactly one delivery
    // Tell the recorder, so a demo carries the out-of-band coordinate too.
    // It is only meaningful alongside a touch, so lb_demo attaches it to
    // the next touch event rather than storing it separately -- a demo that
    // kept them apart could replay them out of order.
    lb_demo_note_extended(x);
}
// THE VERTICAL TWIN, which did not exist until now.
//
// Two symptoms -- units could not reach the top bounds of the map, and a
// click outside the original aspect just moved to its edge -- are one
// defect. The whole touch widening was built
// for X: one 16-bit shadow, ~40 patched reads, and nothing at all for Y. The
// guest keeps both coordinates in BYTES (recogniser +7 press, +9 current), the
// test surface is 348 tall, and 348 > 255 -- so every click below native row
// 255 wrapped modulo 256, exactly as X did before kOrderFix.
//
// Publishing a Y without also widening its readers would be worse than leaving
// it: the drag rect's corners are ordered by a SIGNED compare, so a wrapped Y
// inverts the rectangle vertically -- the same failure the X axis had. The
// readers are repointed in the same change (kOrderFix group 128).
extern "C" void lb_touch_set_extended_y(int y) {
    g_ext_touch_y = y;
    g_ext_fresh = true;                 // shares X's one-delivery rule
    lb_demo_note_extended_y(y);         // docs/383: demos carry it too
}

extern "C" void lb_touch_clear_extended(void) {
    g_ext_touch_x = INT32_MIN;
    g_ext_touch_y = INT32_MIN;
    // The recorder has to learn about the CLEAR as well as the set, or a demo
    // keeps publishing the last out-of-band x for every later tap -- including
    // the ones the player made inside the native columns.
    lb_demo_note_extended(INT32_MIN);
    lb_demo_note_extended_y(INT32_MIN);
}

static void apply_touch_extension(void) {
    // Gated on the widening, so a vanilla session never has its input path
    // touched at all. This is the first patch in this port that changes how the
    // cartridge reads its own INPUT rather than how the port draws, and a
    // mistake here would show up as units selecting slightly wrong rather than
    // as something obviously broken.
    // ON BY DEFAULT NOW, and the reason it was not is worth keeping.
    //
    // It used to say: "the delivery chain is proven end to end, but what is NOT
    // proven is that anything downstream acts on it -- with the hero at screen
    // x -45 and -45 delivered, tapping still does not select him. Until that
    // works there is no benefit to weigh against altering how the cartridge
    // reads its own input."
    //
    // THAT WAS A MEASUREMENT OF A BROKEN PROBE. docs/217 found that
    // nds_frontend_debug_touch refuses any x >= 256, so the harness's clicks
    // never reached the frontend at all -- not the out-of-band ones and not its
    // own control. Every later attempt then failed for its own reason: the
    // reported entity position is not the clickable one, and a click near the
    // boundary "succeeds" from the CLAMP alone, because a clamped click lands
    // at native x 0 and a target whose hit area covers 0 is hit either way.
    //
    // THAT MEASUREMENT WAS ALSO WRONG, and it is retracted here rather than
    // left standing. It read:
    //
    //     Controlled properly -- the player's BASE parked at screen x -61, its
    //     whole hit area far from where a clamp can reach, twelve clicks
    //     across it at one camera position -- the answer is unambiguous:
    //         with this patch:     4/12 select, at native x -49 and -25
    //         without it:          0/12
    //
    // The base's hit area is NOT far from where a clamp can reach: it covers
    // native x 0, and a click that clamps to 0 hits it wherever it was aimed.
    // The four "successes" are that artifact -- the very one the paragraph
    // above warns about -- and the control was not controlling for it.
    //
    // Redone with a control that works and a target a clamp cannot reach (the
    // BUILDER, which stands still, at one camera position, same delivery and
    // same offset either side of the boundary -- docs/224):
    //
    //     in band,  native x 123:   hud 0 -> 6   SELECTED
    //     out band, native x -37:   hud 0 -> 0   nothing, slot held -38
    //
    // So the coordinate ARRIVES out of band and the cartridge does not act on
    // it. The patch stays on: it does no harm -- the in-band control above is
    // unchanged by it -- and delivering the true coordinate is a precondition
    // for any fix. But it does not yet do the job it is meant to, and
    // saying otherwise here would make the next reader trust it.
    //
    // LB_TOUCHEXT=0 turns it off, which is how the control above was run.
    if (const char* v = std::getenv("LB_TOUCHEXT"))
        if (v[0] == '0') return;
    if (!lb_setting_enhanced()) return;

    static int state = 0;             // 0 = untried, 1 = patched, -1 = refused
    static int waited = 0;
    // docs/396: a game reset reloads the ARM9 image, and these sites with it.
    static unsigned gen = 0;
    if (gen != g_boot_gen) { gen = g_boot_gen; state = 0; waited = 0; }
    if (state == 0) {
        const int total = (int)(sizeof kTouchPatches /
                                sizeof kTouchPatches[0]);
        int ok = 0;
        for (const TouchPatch& p : kTouchPatches)
            if ((bus_read_u32(p.addr) & touch_mask(p)) ==
                (p.stock & touch_mask(p))) ++ok;
        // "NOTHING MATCHES YET" IS NOT A USABLE READINESS TEST, and it broke
        // the moment a site was added in always-resident code.
        //
        // The old rule was: zero matches means the overlay has not loaded, so
        // try again; anything between one and all-but-one means a different
        // build, so refuse forever. That held while every site lived in the
        // same overlay, which loaded as a unit. The clamp at 0200A3C4 is core
        // code and is resident from boot, so `ok` was 1 on the very first
        // frame -- neither zero nor total -- and the patch refused permanently
        // on a build where all nine sites were, in fact, exactly right. The
        // refusal printed, which is the only reason it took minutes instead of
        // hours to find.
        //
        // The readiness test is now the passage of time, not the count. Keep
        // retrying while the match is partial; a genuinely different build
        // will still never reach `total`, and after ten seconds of that the
        // reason is printed ONCE, naming the sites that disagree so the next
        // person is not left counting.
        if (ok != total) {
            if (++waited == 600) {
                std::fprintf(stderr,
                             "[lb_touch] STILL WAITING: %d of %d sites match "
                             "the stock encoding after 10s. Sites that do "
                             "not:\n", ok, total);
                for (const TouchPatch& p : kTouchPatches) {
                    const uint32_t got = bus_read_u32(p.addr);
                    const uint32_t m = touch_mask(p);
                    if ((got & m) != (p.stock & m))
                        std::fprintf(stderr,
                                     "[lb_touch]   %08X reads %08X, expected "
                                     "%08X (mask %08X)  (%s)\n",
                                     p.addr, got, p.stock, m, p.what);
                }
                std::fflush(stderr);
            }
            return;                   // not loaded yet, or not this build
        }
        // LB_TOUCHSKIP=<hex address>,... leaves named sites at stock.
        //
        // These patches interact, and the interaction is the interesting part.
        // The event's x is used for TWO different things: the classification
        // flags and the widget hit test read it, while the selection RECTANGLE
        // is built from the touch struct at 0215376A instead -- so keeping the
        // event's x in range and letting the rectangle carry the true value is
        // a configuration worth testing, and it is the opposite of what the
        // clamp patch does.
        //
        // A/B-ing that by rebuilding twice invites exactly the mistake this
        // project has already made (a stale binary answering the new
        // question), so the switch lives in the binary, like LB_EXTCLAMP.
        const char* skip = std::getenv("LB_TOUCHSKIP");
        for (const TouchPatch& p : kTouchPatches) {
            char key[12];
            std::snprintf(key, sizeof key, "%X", (unsigned)p.addr);
            if (skip && std::strstr(skip, key)) {
                std::fprintf(stderr, "[lb_touch] %08X SKIPPED by "
                             "LB_TOUCHSKIP (%s)\n", p.addr, p.what);
                continue;
            }
            // READ-MODIFY-WRITE, so the halfword this entry does NOT own is
            // preserved rather than reverted. A plain bus_write_u32(p.want)
            // would undo kOrderFix at the two contended words below.
            const uint32_t m = touch_mask(p);
            bus_write_u32(p.addr,
                          (bus_read_u32(p.addr) & ~m) | (p.want & m));
            std::fprintf(stderr, "[lb_touch] %08X %s\n", p.addr, p.what);
        }
        state = 1;
        std::fprintf(stderr,
                     "[lb_touch] clicks past the native 256 columns now reach "
                     "the game, through %08X\n", kTouchSlot);
        std::fflush(stderr);
    }
    if (state != 1) return;

    // LB_TOUCH_FORCE=<n> publishes a fixed extended X while the pen is down, so
    // the mechanism can be tested without depending on the frontend mapping.
    static int forced = -100000;
    if (forced == -100000) {
        const char* v = std::getenv("LB_TOUCH_FORCE");
        forced = v ? std::atoi(v) : INT32_MIN;
    }
    // The force is a TEST AID and must not touch the menus. Applied
    // unconditionally it sent every tap in the route to x = -45, so the run
    // never reached a battle at all and reported "not selected" for a reason
    // that had nothing to do with the feature. Only in a battle, and only while
    // the pen is actually down.
    int cx = 0, cy = 0;
    const bool in_battle = lb_field_camera(&cx, &cy);
    int x = (forced != INT32_MIN && in_battle && g_native_touch_down)
                ? forced : g_ext_touch_x;
    // Nothing to say: hand the game back the native x the runner delivered, so
    // menus, the route, and every click inside the band behave exactly as they
    // always did. NOT the struct's own +0x0A -- see g_native_touch_x.
    if (x == INT32_MIN) x = g_native_touch_x;
    // docs/396: the range is the halfword's, not the old 1024-column
    // surface's. Zoomed fully out the surface is the whole map (1536 columns
    // and more) and the DS's own 256 can sit 740 columns from the window's
    // left edge; clamped at -512, a click on the HUD bricks anchored there
    // arrived 228 columns away from them and became a move order (run t396hud3,
    // LB_HUDHITLOG: "press -512,-214"). The readers take it as a signed
    // halfword, so +-4096 leaves room for any surface the frontend can make.
    if (x < -4096) x = -4096;
    if (x > 4095) x = 4095;
    bus_write_u16(kTouchSlot, (uint16_t)(int16_t)x);

    // AND THE Y, THROUGH THE SAME DOOR.
    //
    // Computed here rather than down in the recogniser block because there are
    // now TWO consumers of the extended row -- the guest's own writer, which
    // copies this slot into +0x0C for the order path, and the recogniser
    // shadow at +0x12/+0x14 for the gesture path -- and they must agree. When
    // the shadow computed its own y from a different fallback the two could
    // disagree for a frame, which is precisely the class of bug that made a
    // polygon stretch when two vertices disagreed about the band.
    //
    // No LB_TOUCH_FORCE twin: the force exists to isolate coordinate delivery
    // from the click mapping, and one axis already does that.
    int y = g_ext_touch_y;
    // The fallback is the NATIVE row the runner delivered, not +0x0C. Reading
    // +0x0C back stopped being valid the moment 0203DD20 was repointed -- the
    // guest's writer now fills +0x0C FROM this slot, so a fallback that read it
    // would publish its own last output and freeze. That is not hypothetical:
    // it is the exact failure the X path hit and documented above.
    if (y == INT32_MIN) y = g_native_touch_y;
    // docs/396: as the x above; a 768-row surface puts the band up to 576
    // rows below the top edge.
    if (y < -4096) y = -4096;
    if (y > 4095) y = 4095;
    bus_write_u16(kTouchYSlot, (uint16_t)(int16_t)y);

    // FINDING A FREE HALFWORD FOR THE Y SHADOW, BY MEASUREMENT.
    //
    // A click outside the original aspect still just moved to its edge, and
    // units still could not reach the top bounds of the map. Those are the
    // same defect -- the vertical touch coordinate
    // is clamped to the native screen before the game sees it, so a click above
    // the band arrives at row 0 and the order lands at the native top edge. The
    // units can reach the top; the CLICK cannot.
    //
    // X solves this by publishing to kTouchSlot (+0x18) and repointing the
    // guest's own load at it, which bypasses the touchscreen emulation
    // entirely. Y needs the same, and therefore needs its own free halfword.
    //
    // +0x18 was chosen by measurement, and the note above records how the first
    // attempt got it wrong: with no touch being driven the touch code never
    // runs, so EVERY candidate looks unwritten. So this samples only while the
    // pen is down, and carries +0x0A and +0x0C -- the known-written X and Y --
    // as CONTROLS. If the controls do not move, the sample proves nothing and
    // says so rather than nominating a slot.
    //
    // LB_TOUCHSLOTPROBE=1. Off by default; it is a measurement, not a feature.
    {
        static int probe = -1;
        if (probe < 0) probe = std::getenv("LB_TOUCHSLOTPROBE") ? 1 : 0;
        if (probe && g_native_touch_down) {
            static const uint32_t off[] = {0x0Au, 0x0Cu, 0x1Au, 0x1Cu, 0x1Eu,
                                           0x20u, 0x22u};
            static uint16_t first[7];
            static bool seeded = false;
            static unsigned moved = 0u;   // bit per offset
            static unsigned samples = 0u;
            if (!seeded) {
                for (int i = 0; i < 7; ++i)
                    first[i] = bus_read_u16(kTouchBase + off[i]);
                seeded = true;
            }
            for (int i = 0; i < 7; ++i)
                if (bus_read_u16(kTouchBase + off[i]) != first[i])
                    moved |= (1u << i);
            if (++samples == 600u) {
                std::fprintf(stderr,
                             "[lb_slotprobe] after %u pen-down samples, "
                             "controls +0A=%s +0C=%s | candidates "
                             "+1A=%s +1C=%s +1E=%s +20=%s +22=%s\n",
                             samples,
                             (moved & 1u) ? "MOVED" : "still",
                             (moved & 2u) ? "MOVED" : "still",
                             (moved & 4u) ? "MOVED" : "still",
                             (moved & 8u) ? "MOVED" : "still",
                             (moved & 16u) ? "MOVED" : "still",
                             (moved & 32u) ? "MOVED" : "still",
                             (moved & 64u) ? "MOVED" : "still");
                if ((moved & 3u) != 3u)
                    std::fprintf(stderr,
                                 "[lb_slotprobe] CONTROLS DID NOT BOTH MOVE -- "
                                 "this sample proves nothing about the "
                                 "candidates\n");
                std::fflush(stderr);
            }
        }
    }

    // AND THE ONE FIELD THE PICK ACTUALLY READS.
    //
    // docs/234: the routine that decides what is under the touch builds its
    // query point from a pair of SINGLE-BYTE fields, so a native x of -85
    // arrives as 171 and the query lands three hundred pixels away. Everything
    // downstream of those bytes is already 16-bit and already signed -- the
    // point constructor stores x as a halfword and 02023FEC reads it back with
    // LDRSH -- so the whole defect is the one byte in the middle.
    //
    // The read side is patched to `ldrh r1,[r5,#0x0E]` (kTouchPatches), which
    // moves it to the object's one FREE halfword. Nothing in the cartridge
    // writes that halfword, and there was no spare instruction slot to make it
    // do so: at the press store the 16-bit x is live for exactly one
    // instruction, and the held store needs both of its two slots to keep +8
    // and +9 correct for the drag threshold.
    //
    // So the port writes it. The object's address is not a guess: FUN_020A8794
    // passes `*DAT_020a8824` as the recogniser's `this`, and DAT_020a8824 is
    // 0x02157AF4 -- the same pointer the cartridge itself allocates and
    // dereferences. Reading it back each frame also means a reallocated object
    // is followed automatically, the same self-healing rule apply_cull_width
    // uses.
    {
        const uint32_t obj = bus_read_u32(0x02157AF4u);
        if (obj >= 0x02000000u && obj < 0x02400000u) {
            bus_write_u16(obj + 0x0Eu, (uint16_t)(int16_t)x);

            // THE VERTICAL SHADOWS, at +0x12 (current) and +0x14 (press).
            //
            // Room exists because every one of the FOURTEEN allocation sites is
            // now 0x18 rather than 0x10 -- eleven of them were widened in the
            // same change that found the press-x write was running two bytes
            // past a stock allocation. Bytes +0x12..+0x17 are free and the
            // cartridge never touches them.
            //
            // The row is the one computed for the slot above, NOT recomputed.
            //
            // THIS BLOCK USED TO READ kTouchBase+0x0C AS ITS FALLBACK, and the
            // reasoning recorded here was that +0x0C "is written by the pump
            // every frame and the port does not repoint its load, so it always
            // holds the real delivered row". That was true when it was written
            // and is FALSE NOW: 0203DD20 is repointed in this same change, so
            // +0x0C is filled from kTouchYSlot and reading it back would be a
            // feedback loop that freezes the row at its last value.
            //
            // Sharing one value also removes the possibility of the gesture
            // path and the order path disagreeing about where the pen is.
            bus_write_u16(obj + 0x12u, (uint16_t)(int16_t)y);
            // docs/348: THE DRAG EDGE-SCROLL'S VIEW OF THE PEN, at +0x16/+0x17
            // (see the two kTouchPatches entries at 020ABC20/020ABC24). The
            // game scrolls when its byte is within 8 of its screen edge
            // (<= 8, >= 0xF8, >= 0xB8); these say so exactly when the pen is
            // within 8 px of the SURFACE's edge -- the edge of the picture the
            // player is looking at -- and never out in the middle of it. The
            // surface position is the guest position plus the band the frame
            // on screen was composited with, the same one the touch mapping
            // took off. A surface no wider (taller) than the DS's reduces to
            // the stock rule on that axis.
            {
                const int sw = (int)nds_gpu3d_output_width();
                const int sh = (int)nds_gpu3d_output_height();
                const int sx = x + (sw > 256 ? lb_shown_band_x() : 0);
                const int sy = y + (sh > 192 ? lb_shown_band_y() : 0);
                const int pw = sw > 256 ? sw : 256;
                const int ph = sh > 192 ? sh : 192;
                const uint8_t px = sx <= 8 ? 0u : sx >= pw - 8 ? 0xFFu : 0x80u;
                const uint8_t py = sy <= 8 ? 0u : sy >= ph - 8 ? 0xBFu : 0x60u;
                bus_write_u8(obj + 0x16u, px);
                bus_write_u8(obj + 0x17u, py);
            }
            {
                static int y_was_down = 0;
                if (g_native_touch_down && !y_was_down)
                    bus_write_u16(obj + 0x14u, (uint16_t)(int16_t)y);
                y_was_down = g_native_touch_down;
            }
            // AND THE PRESS POSITION, at +0x10.
            //
            // The selection rectangle has two corners: where the pen went
            // down and where it is now. Serving both from +0x0E makes a TAP
            // exactly right -- the two are the same value -- but collapses a
            // real click-drag's x extent, which would regress in-band
            // drag-select.
            //
            // So the object is allocated eight bytes larger (the `mov r0,#0x10`
            // at 020A87AA becomes #0x18) and the press x gets its own halfword.
            // The port latches it on the pen-down edge, which is the same
            // moment the cartridge fills its own +6/+7, and holds it for the
            // duration of the press.
            static int was_down = 0;
            if (g_native_touch_down && !was_down)
                bus_write_u16(obj + 0x10u, (uint16_t)(int16_t)x);

            // LB_ORDERLOG=1 dumps the recogniser's own fields on each pen
            // edge, which is the measurement docs/238 needs and could not get
            // any other way.
            //
            // The cartridge keeps the touch position in BYTES -- +6/+7 press,
            // +8/+9 current -- and this port publishes 16-bit copies at +0x0E
            // and +0x10. Selection reads the 16-bit copy and works out in the
            // band; a move order to the same place arrives truncated modulo
            // 256. So the question is simply which field still holds a byte
            // when the order is built, and reading them side by side answers
            // it without patching anything or guessing which consumer is at
            // fault.
            static int log_on = -1;
            if (log_on < 0) log_on = std::getenv("LB_ORDERLOG") ? 1 : 0;
            if (log_on && g_native_touch_down != was_down) {
                std::fprintf(stderr,
                             "[lb_touchfields] %s  press byte=(%u,%u) cur "
                             "byte=(%u,%u)  port x16=%d press16=%d  "
                             "published=%d\n",
                             g_native_touch_down ? "DOWN" : "up  ",
                             bus_read_u8(obj + 6u), bus_read_u8(obj + 7u),
                             bus_read_u8(obj + 8u), bus_read_u8(obj + 9u),
                             (int)(int16_t)bus_read_u16(obj + 0x0Eu),
                             (int)(int16_t)bus_read_u16(obj + 0x10u), x);
                // THE VERTICAL CHAIN, END TO END, IN ONE LINE.
                //
                // The Y fix has three links and each can fail independently,
                // so a single "did the unit move" answer cannot say WHICH
                // broke. Printed in the order the value travels:
                //
                //   want   what the port decided to publish
                //   slot   0215377A, what it actually wrote
                //   +0C    0215376C, what the guest's REPOINTED writer copied
                //          there -- this is the link the repoint creates, and
                //          if it tracks `slot` the patch is live
                //   sh     obj+0x12/+0x14, the recogniser shadows
                //   byte   the cartridge's own +9/+7, which stay truncated by
                //          design and are the control: if `+0C` equals `byte`
                //          the repoint did NOT take
                //
                // Same pen-edge gate as the line above, so it is silent unless
                // LB_ORDERLOG is set and costs nothing in a normal session.
                std::fprintf(stderr,
                             "[lb_touchy]      %s  want=%d slot=%d +0C=%d  "
                             "sh=(%d,%d) byte=(%u,%u)\n",
                             g_native_touch_down ? "DOWN" : "up  ", y,
                             (int)(int16_t)bus_read_u16(kTouchYSlot),
                             (int)(int16_t)bus_read_u16(kTouchBase + 0x0Cu),
                             (int)(int16_t)bus_read_u16(obj + 0x12u),
                             (int)(int16_t)bus_read_u16(obj + 0x14u),
                             bus_read_u8(obj + 9u), bus_read_u8(obj + 7u));
                std::fflush(stderr);
            }
            was_down = g_native_touch_down;
        }
    }

    // DID IT ARRIVE? "Not selected" has two very different causes -- the value
    // never reached the struct, or it reached it and something downstream
    // rejected it -- and only reading the struct back tells them apart.
    if (std::getenv("LB_TOUCHLOG") && g_native_touch_down) {
        static unsigned n = 0;
        if ((++n % 30u) == 0u)
            std::fprintf(stderr,
                         "[lb_touch] published %d -> slot=%d  struct +0A=%d "
                         "+0C=%d +0E=%d  native=%d\n",
                         x, (int16_t)(uint16_t)bus_read_u16(kTouchSlot),
                         (int16_t)(uint16_t)bus_read_u16(kTouchBase + 0x0Au),
                         (int16_t)(uint16_t)bus_read_u16(kTouchBase + 0x0Cu),
                         (int16_t)(uint16_t)bus_read_u16(kTouchBase + 0x0Eu),
                         g_native_touch_x);
    }
}

// ---- LB_FXLOG: find live EFFECTS and say which band they are in -----------
//
// The last unaudited part of the widened field is effects and particles. docs/210
// answered the unit half by naming entities out of the pool at 0x02155028;
// effects are not in that pool, and RTTI says why:
//
//     Battles::Effect         -> Battles::DListItem
//     Battles::ParticleSystem -> Battles::DListItem
//
// They are linked-list items, so there is no array to walk and no count to
// read. But every one of them carries a vtable pointer in its first word, and
// the RTTI extract already knows all 41 of those addresses -- so main RAM can
// be scanned for them directly, which finds live effects without having to
// locate a list head first.
//
// The scan is over a direct pointer to main RAM, one pass, and only when the
// variable is set. Nothing here runs in a normal session.
//
// The position offset is NOT assumed. The first version of this printed a
// window of the object's words so the position could be identified from data
// rather than guessed at -- see docs/211.
static void effect_log(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_FXLOG") ? 1 : 0;
    if (!on) return;
    // LB_FXPERIOD=<vblanks> sets how often the heap is swept; 60 (once a
    // second) is the default and was fine while the question was "what effects
    // exist". It is NOT fine for the question that is actually open -- whether
    // a named effect is ever DRAWN in a widened column -- because the effects
    // that answer it are transient. A selection box lasts as long as the drag
    // that makes it, well under a second, so a once-a-second sampler can miss
    // every one of them and report nothing while the thing is on screen.
    static unsigned period = 0;
    if (!period) {
        period = 60u;
        if (const char* v = std::getenv("LB_FXPERIOD")) {
            const int p = std::atoi(v);
            if (p > 0 && p <= 600) period = (unsigned)p;
        }
    }
    static unsigned n = 0;
    if ((++n % period) != 0u) return;

    BusRegion ram{};
    if (!bus_get_region("mainram", &ram) || !ram.ptr) return;
    const uint32_t* w = (const uint32_t*)ram.ptr;
    const size_t words = ram.len / 4u;
    const uint32_t kMainRamBase = 0x02000000u;   // fixed on NDS

    int cx = 0, cy = 0;
    const bool have_cam = lb_field_camera(&cx, &cy);

    // SKIP THE CODE IMAGE. The first version of this scanned all of main RAM
    // and reported 24 "live effects" that were nothing of the kind: every hit
    // sat below 0x02012000 and the words after it disassembled as ARM --
    // E92D4010 is `push {r4, lr}`. Those are vtable addresses appearing in
    // literal pools and in constructors that store them, not objects that have
    // one. The ARM9 static image is 0x150400 bytes, so anything at or below
    // that is code and data the linker placed, and a live heap object is not
    // there.
    const size_t kHeapStart = 0x00152000u / 4u;
    // COUNTED SEPARATELY FROM PRINTED. The on-screen filter's first version
    // decremented the single counter to "give the slot back", which also
    // corrupted the summary into "0 live effect/particle objects" for frames
    // that had plenty. A filter that silently rewrites the number it is
    // filtering is worse than no filter.
    int found = 0, scanned = 0;
    // LB_FXONSCREEN=1 reports only effects that are actually being DRAWN.
    //
    // The scan stops at the first sixteen matches by heap address, and those
    // turn out to be long-lived objects far from the camera -- a run of 539
    // samples, with the y axis finally being tested, reported every single one
    // off screen. That is not evidence that effects never reach the band; it is
    // evidence that this sampler never looked at one that was on it.
    //
    // With the filter on, an off-screen object costs a position test and is
    // skipped without consuming one of the sixteen slots, so a transient effect
    // near the camera can actually be seen.
    static int fx_onscreen = -1;
    if (fx_onscreen < 0) fx_onscreen = std::getenv("LB_FXONSCREEN") ? 1 : 0;
    for (size_t i = kHeapStart; i < words && found < 16; ++i) {
        const uint32_t v = w[i];
        // Cheap reject first: every vtable here is in 0x0214xxxx.
        if ((v & 0xFFFF0000u) != 0x02140000u) continue;
        const char* name = nullptr;
        for (int k = 0; k < kLbFxClassCount; ++k)
            if (g_lb_fx_classes[k].vtable == v) { name = g_lb_fx_classes[k].name; break; }
        if (!name) continue;
        ++found;
        const uint32_t obj = kMainRamBase + (uint32_t)(i * 4u);
        char buf[512];
        // WHERE IT IS, AND WHICH BAND THAT IS.
        //
        // docs/211 left the effects row as "structurally sound but not
        // visually proven for one NAMED effect in a widened column", because
        // the log printed a window of the object and left the position fields
        // to be found. They are found now: +0x14 and +0x18 are the world x and
        // y in 20.12 fixed point -- they read as plausible map pixels and they
        // track smoothly as an effect moves (1427 -> 1370 -> 1291 over three
        // frames for a HeroStartSpellEffect), which no other field in the
        // window does.
        //
        // Screen x is world minus camera, exactly as for entities, so the same
        // three-way classification the entity log uses applies: LEFT band,
        // native, RIGHT band. That turns "effects presumably reach the band"
        // into a named effect with a coordinate.
        // THE OFFSET IS NOT THE SAME FOR EVERY CLASS, so it is found rather
        // than assumed. A BuildConstructionEffect keeps its position at
        // +0x14/+0x18 and a ParticleSystem at +0x1C/+0x20; reading +0x14 for
        // both gave a ParticleSystem "sx = 8695", which is a POINTER shifted
        // right by twelve, not a coordinate.
        //
        // The test is what makes a coordinate a coordinate: neither word looks
        // like a pointer (this heap's pointers are all >= 0x02000000), and both
        // read as plausible map pixels in 20.12. The map is 192 tiles of 8 px,
        // so anything past 4096 is not on it.
        int fx_sx = 0, fx_sy = 0;
        const char* fx_band = "no-position";
        for (int o = 0x14; o <= 0x24 && have_cam; o += 4) {
            if (i + (size_t)(o / 4) + 1u >= words) break;
            const uint32_t qx = w[i + (size_t)(o / 4)];
            const uint32_t qy = w[i + (size_t)(o / 4) + 1u];
            if (qx >= 0x02000000u || qy >= 0x02000000u) continue;
            const int px = (int)((int32_t)qx >> 12);
            const int py = (int)((int32_t)qy >> 12);
            if (px <= 0 || px >= 4096 || py <= 0 || py >= 4096) continue;
            fx_sx = px - cx;
            fx_sy = py - cy;
            // OFF SCREEN AND IN THE EXTRA COLUMNS ARE NOT THE SAME THING --
            // the same trap the entity log already documents. The band comes
            // from the LIVE surface, and anything beyond it is simply not
            // drawn. The two sides are asked for SEPARATELY because they are no
            // longer equal: at a map edge lb_band_left slides to nothing and
            // the whole overhang moves to the other side, so one `ext` for both
            // would call a drawn entity off screen on one side and an
            // off-screen one drawn on the other.
            int fw_ = 0, fh_ = 0, fx_ = 0, fy_ = 0, gw_ = 256, gh_ = 192;
            if (!lb_overlay_field_rect(&fx_, &fy_, &fw_, &fh_, &gw_, &gh_) ||
                gw_ < 256)
                gw_ = 256;
            const int ext = lb_band_left(gw_);
            const int extr = lb_band_right(gw_);
            // AND THE SAME TEST ON Y, or this repeats the very mistake it was
            // written to avoid one axis over. The first run of this reported a
            // BuildConstructionEffect "in the RIGHT-BAND" at sy 696 -- which
            // is far below a surface that is 512 rows tall with a 160-row band,
            // so it was off the bottom of the screen and in no band at all.
            const int exty = lb_band_top(gh_);
            const bool on_y = fx_sy >= -exty &&
                              fx_sy < 192 + (gh_ - 192 - exty);
            if (!on_y)                                  fx_band = "offscreen";
            else if (fx_sx >= 0 && fx_sx < 256)         fx_band = "native";
            else if (fx_sx < 0 && fx_sx >= -ext)        fx_band = "LEFT-BAND";
            else if (fx_sx >= 256 && fx_sx < 256 + extr) fx_band = "RIGHT-BAND";
            else                                        fx_band = "offscreen";
            break;
        }
        ++scanned;
        if (fx_onscreen && std::strcmp(fx_band, "offscreen") == 0) {
            --found;                 // an off-screen object uses no slot
            continue;
        }
        int k = std::snprintf(buf, sizeof buf,
                              "[lb_fx] %-24s @%08X sx=%d sy=%d %s", name, obj,
                              fx_sx, fx_sy, fx_band);
        // A window of the object, so the position fields can be FOUND rather
        // than assumed. Printed as 20.12 fixed point too, because that is the
        // format the entity positions use and a world coordinate will read as a
        // plausible map pixel in it.
        for (int o = 4; o <= 0x2C && k < (int)sizeof buf - 32; o += 4) {
            if (i + (size_t)(o / 4) >= words) break;
            const uint32_t q = w[i + (size_t)(o / 4)];
            k += std::snprintf(buf + k, sizeof buf - (size_t)k, " +%02X=%08X(%d)",
                               o, q, (int)((int32_t)q >> 12));
        }
        std::fprintf(stderr, "%s\n", buf);
    }
    std::fprintf(stderr,
                 "[lb_fx] %d live effect/particle objects (%d shown); "
                 "cam=(%d,%d)%s\n",
                 scanned, found, cx, cy, have_cam ? "" : " (no camera)");
    std::fflush(stderr);
}

// ---- LB_ENTLOG: name every entity and say which band it is drawn in --------
//
// The widened columns must render modelled special units too, and docs/209 could only answer half of it. A capture
// shows a grey shape standing at surface x 360, which proves SOMETHING renders
// out there; it does not say what. "Modelled special unit" is a claim about
// which entity it is, and that has to come from the data.
//
// The entity pool carries it. Each entity's blueprint record address maps
// straight into out/eur/entities.json, where category 0 is "Hero" -- the game's
// own word for its special units, and there are 25 of them.
//
// Screen x is entity x minus camera x, both in the same map space, so a
// negative value is in the LEFT widened column and >= 256 is in the RIGHT one.
// Those are exactly the two bands the vanilla game never draws.
static void entity_log(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_ENTLOG") ? 1 : 0;
    if (!on) return;

    // ── WHICH entities are selected ────────────────────────────────────────
    //
    // Whether drag-selection selects the right units is a question about a SET,
    // and nothing so far could read that set: every selection probe in this
    // project uses the COUNT at 0x0215802C, which answers "did anything get
    // selected" and cannot answer "were they the right ones".
    //
    // docs/226 found the shape from the per-frame consumer -- `param_1+0x54`
    // is the selection list, `param_1+0x58` its count -- and left param_1
    // open. The count's known address closes it: 0x0215802C = param_1 + 0x58
    // puts the list pointer at 0x02158028, which is also what --bandselect
    // reads. THE ENTRIES ARE TWO-BYTE HANDLES, not pointers; reading them as
    // words was this probe's first mistake and produced nothing.
    //
    // BEFORE the once-a-second gate below, and before the camera and field
    // early-returns, because a selection that is made and cleared between two
    // samples is invisible -- which was this probe's second mistake: sampled
    // once a second it reported `none` for a whole run in which drags plainly
    // selected. Printed only when the set CHANGES, so a held selection is
    // silent and the per-frame cost is one word read and a compare.
    {
        const uint32_t count = bus_read_u32(0x0215802Cu);
        const uint32_t list = bus_read_u32(0x02158028u);
        static uint32_t last_hash = 0xFFFFFFFFu;
        uint32_t hash = count * 2654435761u;
        uint16_t handles[16];
        unsigned hn = 0;
        if (count <= 64u && list >= 0x02000000u && list < 0x02400000u) {
            for (unsigned i = 0; i < count && i < 16u; ++i) {
                const uint16_t h = bus_read_u16(list + i * 2u);
                handles[hn++] = h;
                hash = hash * 31u + h;
            }
        } else if (count != 0u) {
            hash = 0xDEADBEEFu;
        }
        if (hash != last_hash) {
            last_hash = hash;
            if (count == 0u) {
                std::fprintf(stderr, "[lb_sel] none\n");
            } else if (hn == 0u) {
                std::fprintf(stderr,
                             "[lb_sel] count=%u but list=%08X unusable\n",
                             count, list);
            } else {
                char sb[512];
                int sn = std::snprintf(sb, sizeof sb,
                                       "[lb_sel] count=%u handles:", count);
                for (unsigned i = 0; i < hn && sn < (int)sizeof sb - 8; ++i)
                    sn += std::snprintf(sb + sn, sizeof sb - (size_t)sn,
                                        " %04X", handles[i]);
                // IS THE HANDLE A POOL INDEX? Test it rather than assume it.
                //
                // The pool at 0x02155028 holds an entity array at +8 and its
                // count at +0xC (lb_field.cpp), and an entity's map position
                // is at +0xEC / +0xF0 in 20.12 with the same origin offsets
                // lb_field_entity_at applies. If the handle indexes that array
                // directly, the position printed here will match the position
                // the entity feed reports for the same unit; if it does not,
                // the two will disagree and the index theory is dead. Either
                // way the reader can see which, which is the point -- docs/313
                // has already spent two mistakes on assumptions here.
                const uint32_t pool = bus_read_u32(0x02155028u);
                const uint32_t arr = (pool >= 0x02000000u && pool < 0x02400000u)
                                     ? bus_read_u32(pool + 8u) : 0u;
                const uint32_t pn = arr ? bus_read_u32(pool + 0xCu) : 0u;
                for (unsigned i = 0; i < hn && sn < (int)sizeof sb - 48; ++i) {
                    const uint32_t h = handles[i];
                    if (!arr || h >= pn || pn > 4096u) {
                        sn += std::snprintf(sb + sn, sizeof sb - (size_t)sn,
                                            "  [%04X:out-of-pool]", h);
                        continue;
                    }
                    const uint32_t e = bus_read_u32(arr + h * 4u);
                    if (e < 0x02000000u || e >= 0x02400000u) {
                        sn += std::snprintf(sb + sn, sizeof sb - (size_t)sn,
                                            "  [%04X:bad-ent]", h);
                        continue;
                    }
                    const int mx = (int)((int32_t)bus_read_u32(e + 0xECu) >> 12) - 29;
                    const int my = (int)((int32_t)bus_read_u32(e + 0xF0u) >> 12) - 1;
                    const uint32_t bp = bus_read_u32(e + 0x160u);
                    int cat = -1;
                    if (bp >= 0x02000000u && bp < 0x02400000u) {
                        const uint32_t info = bus_read_u32(bp + 8u);
                        if (info >= 0x02000000u && info < 0x02400000u)
                            cat = (int)bus_read_u8(info + 0x5Cu);
                    }
                    sn += std::snprintf(sb + sn, sizeof sb - (size_t)sn,
                                        "  [%04X ptr=%08X cat=%d map=%d,%d]",
                                        h, e, cat, mx, my);
                }
                std::fprintf(stderr, "%s\n", sb);
            }
            std::fflush(stderr);
        }
    }

    static unsigned n = 0;
    if ((++n % 60u) != 0u) return;   // once a second, so aiming has feedback

    int cx = 0, cy = 0;
    if (!lb_field_camera(&cx, &cy)) return;
    const uint32_t base = g_blueprint_base;

    // The widened columns are ONLY these two 96-px strips. The first version of
    // this counted "sx outside 0..255" as being in a widened column, which is
    // every entity anywhere else on the map -- it reported 18 of 23 entities
    // "in a widened column" for a scene where the truth was none, because most
    // of them sat hundreds of pixels off screen. Off screen and in the extra
    // columns are not the same thing, and only the second one is the question.
    //
    // FROM THE LIVE SURFACE, not from 96. That number was what a fixed 448-wide
    // surface added per side, and the surface has been adaptive since docs/215
    // -- a 1600x900 window gives 392x212, so the strips are 68 px. Left as 96
    // this counts entities that are OFF the drawn surface as being in a widened
    // column, which is the same mistake the paragraph above describes, one step
    // smaller. It matters because this line is what the harness reads to decide
    // where to aim, and a trial aimed at native -80 on a 68-px band is aimed at
    // nothing.
    int fx_ = 0, fy_ = 0, fw_ = 0, fh_ = 0, gw_ = 256, gh_ = 192;
    if (!lb_overlay_field_rect(&fx_, &fy_, &fw_, &fh_, &gw_, &gh_) || gw_ < 256)
        gw_ = 256;
    // Take the vertical bound from the live surface too, for consistency with
    // the horizontal one. `gh_` was already being fetched and then ignored while
    // the scanline test below used a literal 192.
    //
    // THIS IS NOT A BUG FIX, and an earlier version of this comment claimed it
    // was. The claim came from run model2, where [lb_model] showed a transport
    // at sx=312 -- inside the right widened column -- while the band lines
    // reported none, and the hardcoded 192 looked like the culprit because the
    // unit's sy was 288..443. Checking rather than assuming killed it: that
    // run's surface is `guest=512x192` on all 385 geometry samples, so sy=288 is
    // genuinely off the bottom of the screen and rejecting it was correct. The
    // unit was beside the camera horizontally and below it vertically.
    //
    // The change is kept because a constant and a live value that are supposed
    // to mean the same thing should not be written two different ways -- if the
    // guest surface ever grows taller than 192 this stays right on its own. It
    // changes nothing for any run to date, where gh_ is 192.
    if (gh_ < 192) gh_ = 192;
    // Both sides, separately: the band is no longer split evenly once the
    // clamp slides it at a map edge.
    const int kExt = lb_band_left(gw_);
    const int kExtR = lb_band_right(gw_);
    int total = 0, in_ext = 0, heroes_ext = 0, native = 0;
    char line[256];
    for (int i = 0; i < 256; ++i) {
        int owner = 0, cat = -1, ex = 0, ey = 0;
        if (!lb_field_entity_at(i, &owner, &cat, &ex, &ey)) break;
        ++total;
        const int sx = ex - cx, sy = ey - cy;
        // Every Hero, wherever it is. This is what lets the harness AIM: it
        // reads this line, works out which way the camera has to move to put
        // the hero in a widened column, and pans until it is there -- instead
        // of panning blind and hoping something interesting drifts through.
        if (cat == 0)
        {
            // THE POINTER TOO, exactly as [lb_builder] carries it.
            //
            // Without it a probe that orders "the on-screen unit" cannot prove
            // it reported the SAME entity twice, and "the unit did not move"
            // becomes indistinguishable from "a different unit was named". The
            // band-order probe hit precisely that: it matched a hero line and
            // could only print `?` for identity.
            uint32_t hptr = 0u;
            lb_field_entity_ptr(i, &hptr);
            std::fprintf(stderr, "[lb_hero] sx=%d sy=%d owner=%d ptr=%08X\n",
                         sx, sy, owner, hptr);
        }
        // The player's BASE, wherever it is, in band or out.
        //
        // A hero walks, and every attempt to measure out-of-band CLICKING has
        // been confounded by aiming at where one used to be (docs/217). A base
        // does not move, so panning the camera and clicking it at each step
        // gives a clean sx-versus-selected curve -- which is what says whether
        // the cut-off is exactly screen x 0 (a sign or bounds test) or
        // somewhere else entirely.
        //
        // Category 7 is Base: out/eur/entities.json labels E_EagleCommandBase
        // (blueprint 90) that way, and the port read cat=7 for it live.
        // EVERY base, not only the player's.
        //
        // The owner filter was costing a measurement. Testing whether a click
        // works in the RIGHT widened band needs a target parked deep into it,
        // and the player's own base and builder cannot get there: they sit near
        // the map's left edge, so the camera runs out of map before they reach
        // native 300. The best a run could manage was 259, four pixels past the
        // clamp point, which is close enough that the clamped copy of the click
        // hits the target anyway and the trial proves nothing (docs/225).
        //
        // The OPPONENT'S base is at the other end of the map and has no such
        // problem. Whether clicking it selects anything is a separate question,
        // which is why the probe runs an in-band control on it first.
        if (cat == 7)
            // THE WORLD POSITION TOO, and the camera it was derived from.
            //
            // The pick builds its query point as camera + touch, and a watch on
            // the point-add (STRH at 02023FF8) reports what it came out as --
            // 514 for a band click. Whether that is RIGHT can only be decided
            // against the entity's own world x, and until now this line printed
            // only sx, which is that same subtraction already applied. Printing
            // ex and the camera makes the comparison direct: the click's world
            // point should equal the entity's world position when the click is
            // aimed at it, in the band exactly as in the native columns.
            std::fprintf(stderr,
                         "[lb_base] sx=%d sy=%d owner=%d ex=%d ey=%d "
                         "cam=%d,%d\n",
                         sx, sy, owner, ex, ey, cx, cy);
        // THE PLAYER'S BUILDER, wherever it is. It is the quickest way to put
        // this port's in-field HUD on screen: tap the builder (the grey robot
        // minifig in freeplay, the blue, black-haired one in the King story)
        // or a hero, tap the red brick to view buildings, click an icon, place
        // it in the world on a green space, then tap the checkmark to command
        // the builder to build.
        //
        // That whole sequence is the richest HUD the game shows, so it is what
        // the anchoring and scaling work needs to be tested against -- and a
        // harness cannot aim at a unit whose position is never printed. The
        // builder was reported only when it happened to be in a widened
        // column, which is the one case where the harness does not need to be
        // told. Category 1 is Builder (blueprint indices 2/22/42/62/82/102,
        // one per faction).
        // EVERY builder, not just owner 0's.
        //
        // `owner == 0` was a single-player assumption and it silently broke the
        // multiplayer load generator: on a client, owner 0 is the HOST, whose
        // builder sits hundreds of pixels off the side of this instance's own
        // camera. The harness read this log, found only an off-screen unit,
        // rejected it, and ordered nothing -- so two of three worlds sat still
        // while the desync check called them in sync. Measured, on a real
        // three-player run:
        //
        //     inst0  [lb_builder] sx=114  sy=52   owner=0
        //     inst1  [lb_builder] sx=-861 sy=-468 owner=0
        //     inst2  [lb_builder] sx=-876 sy=45   owner=0
        //
        // The line already prints the owner, so nothing that reads it has to
        // change to keep the old behaviour -- it just now has the option of
        // picking a different one. Builders are one per player, so this is at
        // most a few extra lines per sample.
        if (cat == 1) {
            // The entity's ADDRESS as well, so a watch can be pointed at the
            // struct itself. An in-band click that selects must write something
            // in here, and whichever instruction does that names the routine
            // that decides -- which is what is left to find after docs/226.
            uint32_t eptr = 0u;
            lb_field_entity_ptr(i, &eptr);
            std::fprintf(stderr,
                         "[lb_builder] sx=%d sy=%d owner=%d ptr=%08X\n",
                         sx, sy, owner, eptr);
        }
        // MODEL-BACKED UNITS, wherever they are -- not only in a band.
        //
        // docs/242 measured, from the `asset` column of docs/data/entities.tsv,
        // that categories 5 and 6 are the ONLY `Models/`-backed entities in the
        // game; every other category is `Sprites/`. They are therefore exactly
        // what "modelled special unit rendering" means.
        //
        // The band lines below cannot help aim at one, because they print only
        // once a unit is ALREADY in a widened column -- which is the thing
        // being arranged. Run `model1` is the case in point: a transport and a
        // siege unit were both on the field for hundreds of samples, the pan
        // swept a melee unit through the left column, and nothing ever said
        // where the two model-backed units actually were. This prints their
        // screen position every sample, so a pan can be aimed rather than
        // hoped through.
        if (cat == 5 || cat == 6) {
            int mbp = -1;
            uint32_t mrec = 0u;
            if (base && lb_field_entity_blueprint(i, &mrec) && mrec >= base + 4u)
                mbp = (int)((mrec - base - 4u) / 0x7Cu);
            std::fprintf(stderr,
                         "[lb_model] sx=%d sy=%d owner=%d cat=%d bp=%d\n",
                         sx, sy, owner, cat, mbp);
        }
        if (sy < 0 || sy >= gh_) continue;             // not on this scanline set
        if (sx >= 0 && sx < 256) { ++native; continue; }
        const bool left = (sx >= -kExt && sx < 0);
        const bool right = (sx >= 256 && sx < 256 + kExtR);
        if (!left && !right) continue;                 // simply off screen
        ++in_ext;
        if (cat == 0) ++heroes_ext;                    // category 0 is Hero
        int bp = -1;
        uint32_t rec = 0u;
        const bool got = lb_field_entity_blueprint(i, &rec);
        // >= , not > . The first record sits exactly at base + 4, so a strict
        // test excluded blueprint 0 and reported bp=-1 for it -- and blueprint
        // 0 is K_King, the Knights' Hero, which is precisely the entity this
        // whole diagnostic exists to identify.
        if (base && got && rec >= base + 4u)
            bp = (int)((rec - base - 4u) / 0x7Cu);
        // STAMPED WITH THE SHARED DUMP FRAME, so an entity position can be
        // paired with the layer/composite dump it belongs to.
        //
        // Without it this log cannot be compared with anything. An attempt to
        // check "is a unit drawn where the game says it is" read the newest
        // lb_layers dump against these lines and got nonsense, because the
        // lines in one run span TWO camera positions (sx sets 97 px apart) and
        // the dump is a third moment. lb_dump_frame_tick() already exists for
        // exactly this - it was added when "pair by nearest frame number"
        // matched a composite with a camera rect it was never drawn with and
        // produced the retracted "RIGHT band 12-23%". This log simply never
        // joined the scheme.
        std::snprintf(line, sizeof line,
                      "[lb_ent] f=%-6u %-9s sx=%-4d sy=%-4d owner=%d cat=%-3d"
                      " bp=%-4d rec=%08X base=%08X",
                      lb_dump_frame_get(),
                      left ? "LEFT-EXT" : "RIGHT-EXT", sx, sy, owner, cat, bp,
                      rec, base);
        std::fprintf(stderr, "%s\n", line);
    }
    std::fprintf(stderr,
                 "[lb_ent] f=%-6u cam=(%d,%d) %d entities: %d on screen "
                 "natively, %d IN A WIDENED COLUMN, %d of those category 0 "
                 "(Hero)\n",
                 lb_dump_frame_get(), cx, cy, total, native, in_ext,
                 heroes_ext);
    // WHAT EXISTS, not only what is in a band.
    //
    // The per-entity lines above are printed ONLY for entities in a widened
    // column, so a run with no `cat=6` line cannot distinguish "no
    // Siege/Special rendered out there" from "the battle never contained one".
    // docs/209 leaves exactly that claim open -- "no capture yet identifies a
    // modelled special by name" -- and one census line answers it. Free Play
    // starts with none: the kill-test census found only categories 0, 1, 7, 10,
    // 11 and 13 at match start, so a special has to be BUILT before there is
    // anything to look for.
    {
        unsigned census[24] = {0};
        for (int i = 0; i < 4096; ++i) {
            int o2 = 0, c2 = -1, x2 = 0, y2 = 0;
            if (!lb_field_entity_at(i, &o2, &c2, &x2, &y2)) break;
            if (c2 >= 0 && c2 < 24) ++census[c2];
        }
        char cbuf[256];
        int k = std::snprintf(cbuf, sizeof cbuf, "[lb_ent] categories:");
        for (unsigned c = 0; c < 24u && k < (int)sizeof cbuf - 16; ++c)
            if (census[c])
                k += std::snprintf(cbuf + k, sizeof cbuf - (size_t)k,
                                   " %u:%u", c, census[c]);
        std::fprintf(stderr, "%s%s\n", cbuf,
                     census[6] ? "   <-- SIEGE/SPECIAL PRESENT" : "");
    }

    std::fflush(stderr);
}

// ── Cheat: "Hotwire anything" ──────────────────────────────────────────────
//
// GOAL WITHIN THE PROJECT: cheat settings, including a let-anything-hotwire
// option. That combo entry existed in the panel for a long time with an
// accessor NO CODE CALLED -- a selectable option that did nothing, and no
// ImGui setting or button is meant to remain unwired.
//
// WHY IT WAS UNWIRED, AND WHY THAT REASON IS NOW RETRACTED
//
// The panel said the vanilla rule "is in the targeting layer, not yet located",
// and docs/125, /134, /143 and /148 all recorded the same. It IS located, and
// this is it -- read from the ROM, not guessed:
//
//   func_0207B73C(?, caster, order, target)   returns 0 = the cast is refused
//
//     0207B790  ldrb r1, [r7]           ; the order's effect id
//     0207B798  cmp  r1, #0x20          ; 32 == HotWire. That id is not a
//     0207B79C  bne  0207B7FC           ;   guess: func_0207BC0C's 35-arm jump
//                                       ;   table sends case 32 to the arm that
//                                       ;   calls the HotWireSpell constructor
//                                       ;   func_02078C18, the one function
//                                       ;   that stores vtable 0x0214A920.
//     0207B7A0  ldr  r0, [sb, #0x160]   ; target->definition
//     0207B7A4  ldr  r0, [r0, #8]       ;         ->info
//     0207B7A8  ldrb r0, [r0, #0x5c]    ;         ->category   (docs/72)
//     0207B7AC  cmp  r0, #6             ; 6 == Siege/Special
//     0207B7B0  bne  0207B7FC           ; anything else: allowed, fall through
//     ...
//     0207B7DC  bl   func_020859F4      ; sb = how many category 6 the caster
//                                       ;      owns  (docs/58 names this
//                                       ;      (player, category, live))
//     0207B7E8  bl   func_02085DB0      ; r0 = the caster's farm allowance
//                                       ;      (docs/58 names this the farm
//                                       ;      counter)
//     0207B7EC  cmp  r0, sb
//     0207B7F0  addle sp, sp, #4        ; allowance <= owned  ->  REFUSE
//     0207B7F4  movle r0, #0
//     0207B7F8  pople {r3-r8, sb, sl, pc}
//
// So vanilla does NOT keep a whitelist of hotwirable categories. It restricts
// exactly one thing: stealing an enemy SPECIAL when your farms will not support
// another one. Every other target is allowed already. That is the whole of the
// shipped restriction, and it is why no category whitelist was ever found --
// there is none to find.
//
// WHAT THE CHEAT THEREFORE DOES
//
// "Hotwire anything" means: do not let the farm allowance refuse the steal. The
// three LE-conditional words above are the entire refusal, they are reached
// only after `cmp r1, #0x20` has already established this is a hotwire, and
// falling through lands on 0207B7FC, the ordinary path the non-special case
// takes. Replacing them with NOPs is the smallest change that expresses it.
//
// It is applied and REMOVED with the setting, so the toggle is live rather than
// needing a restart, and it verifies the stock words before writing so a
// different build is left alone rather than corrupted.
struct HotwirePatch { uint32_t addr, stock, want; const char* what; };

// docs/392: AND THE RULE THAT MATTERS, WHICH IS IN THE TAP, NOT HERE.
//
// The farm refusal above was the only category test docs/236 found, so it
// concluded there was no whitelist. There is one, in the stylus targeting:
// the armed-spell tap handler func_020A9350 builds a filter from the spell's
// flags word, and Hotwire (the only spell with flag 0x20) requires tags 0x2B
// (an enemy's) and 0x3B, which func_020A9B7C gives only to category 5
// (0x020A9DD4) and 6 (0x020A9E06): an enemy transport or special. The hotwire
// glitch is func_020A9570 ORing the tags of everything under
// the stylus while keeping the LAST hit's handle -- so a special overlapping
// a unit or building to its south hands the cast that unit. Requiring 0x2B
// twice instead of 0x2B and 0x3B lets the tap take any enemy, hero or
// building directly; nothing downstream tests the category but the farm cap
// above. It only changes which casts this machine issues.
const HotwirePatch kHotwireAnything[] = {
    {0x0207B7F0u, 0xD28DD004u, 0xE1A00000u, "addle sp, sp, #4"},
    {0x0207B7F4u, 0xD3A00000u, 0xE1A00000u, "movle r0, #0"},
    {0x0207B7F8u, 0xD8BD87F8u, 0xE1A00000u, "pople {r3-r8, sb, sl, pc}"},
    {0x020A9458u, 0x213BA802u, 0x212BA802u, "tap filter: movs r1,#0x3B -> #0x2B"},
};

// "SPECIALS ONLY": refuse to hotwire anything that is not category 6.
//
// This is an ADDED restriction, not the vanilla rule -- the note above is right
// that there is no category whitelist in the game to restore. Vanilla refuses
// exactly one thing, a stolen special the farms cannot support, and lets melee,
// ranged, mounted, transports and heroes through.
//
// The validator is func_0207B73C, and its hotwire arm reads (disassembled from
// the ROM with tools/lb_code.py, and the words below verified against it):
//
//     0207B790  ldrb r1,[r7]        ; the order's spell id
//     0207B798  cmp  r1,#0x20       ; 32 = HotWire
//     0207B79C  bne  0207B7FC       ; other spells are not our business
//     0207B7A0  ldr  r0,[sb,#0x160] ; target -> definition
//     0207B7A4  ldr  r0,[r0,#8]     ;        -> info
//     0207B7A8  ldrb r0,[r0,#0x5C]  ;        -> category
//     0207B7AC  cmp  r0,#6          ; 6 = Siege/Special
//     0207B7B0  bne  0207B7FC       ; ANYTHING ELSE IS ALLOWED  <-- patched
//     ...
//     0207B7EC  cmp  r0,r9          ; allowance vs specials owned
//     0207B7F0  addle sp,sp,#4      ; and the LE arm refuses the cast
//
// So the one word at 0207B7B0 decides whether a non-special falls through to
// the allow path. Redirecting it to 0207B7EC sends non-specials into the
// refusal instead: there r0 is the category byte (at most 255) and r9 is the
// target entity pointer (at least 0x02000000, and proven non-null by the beq at
// 0207B770), so the signed compare gives LT, which is inside LE, and the
// existing `movle r0,#0` refusal fires. No new code, and specials keep vanilla
// behaviour exactly.
//
//     bne 0207B7FC = 1A000011   (0x7FC - 0x7B8) / 4 = 17
//     bne 0207B7EC = 1A00000D   (0x7EC - 0x7B8) / 4 = 13
//
// THIS REPLACES A GATE THAT COULD NEVER FIRE. The previous attempt filtered
// inside set_owner_override while a flag marked "inside the hotwire tick" was
// set. That flag is always false by then: HotWireSpell's tick body at
// 0x02078D80 is not lifted, so it runs under tier 3, and tier 3 unwinds the
// host stack the moment it reaches a banked address -- which SetOwner is. The
// tick override has therefore already returned and cleared the flag before
// SetOwner is ever dispatched.
//
// Mutually exclusive with kHotwireAnything by construction: the panel control
// is a single-valued combo, "specials only" is mode 1 and "anything" is mode 2,
// so the two appliers can never both want their patch. That matters, because
// mode 2 NOPs 0207B7F0/F4/F8 -- the exact words this mode redirects into.
const HotwirePatch kHotwireSpecialsOnly[] = {
    {0x0207B7B0u, 0x1A000011u, 0x1A00000Du, "bne 0207B7FC -> bne 0207B7EC"},
};

// ── Commanding into the band: the readers still on the byte path ──────────
//
// GOAL WITHIN THE PROJECT: the widened field must be just as interactable as
// the native one -- tapping or COMMANDING a unit. Selection out there is
// 4 of 4; a move order to the same place lands truncated. docs/238 measured the
// cause directly, on the pen edges of a real order to x=260:
//
//     DOWN  press byte=(147,95) cur byte=(147,95)  port x16=260 press16=260
//     up    press byte=(  4,95) cur byte=(  4,95)  port x16=260 press16=260
//
// The port's 16-bit shadows hold 260; the cartridge's own one-byte fields hold
// 4, which is 260 & 0xFF. Delivery is correct and always was -- the defect is a
// reader still looking at +6/+8 instead of the shadows at +0x10/+0x0E.
//
// THIRTEEN SUCH READS REMAIN, and thirteen is exactly the number of classifier
// patches that once broke deselect outright and had to be reverted. So this is
// gated and grouped BY FUNCTION rather than applied wholesale, so a run can say
// which group fixes the order and which breaks something else:
//
//     LB_ORDERFIX=1  gesture recogniser  func_020AF040
//     LB_ORDERFIX=2  camera-delta        func_020AF15C
//     LB_ORDERFIX=4  tap handler         func_020AF23C
//     LB_ORDERFIX=8  move handler        func_020AF340
//
// bits combine, so 15 is all four. The test is sharp: `--fxband 260` sends the
// unit to ~4 while broken and ~260 when fixed, and band selection's 4-of-4 is
// the regression guard.
//
// `0x020AF2DC` and `0x020AF39E` are deliberately NOT here. They are
// `ldrb +8` / `strb +6` COPY PAIRS: redirecting the load alone would still
// store through a byte, and redirecting both halves would stop maintaining +6
// for the recogniser's own tap-vs-drag threshold. Changing them needs its own
// measurement, not a guess bundled into this one.
struct HalfPatch { uint32_t addr; uint16_t stock, want; uint8_t group; const char* what; };

const HalfPatch kOrderFix[] = {
    {0x020AF070u, 0x79A8u, 0x8A28u, 1u, "recogniser press x"},
    {0x020AF0ACu, 0x7A28u, 0x89E8u, 1u, "recogniser current x"},
    {0x020AF0D0u, 0x7A29u, 0x89E9u, 1u, "recogniser current x (2)"},
    {0x020AF0D2u, 0x79A8u, 0x8A28u, 1u, "recogniser press x (2)"},
    {0x020AF1C0u, 0x79ABu, 0x8A2Bu, 2u, "camera-delta press x"},
    {0x020AF1D0u, 0x7A2Bu, 0x89EBu, 2u, "camera-delta current x"},
    {0x020AF1DAu, 0x7A28u, 0x89E8u, 2u, "camera-delta current x (2)"},
    {0x020AF224u, 0x7A2Bu, 0x89EBu, 4u, "tap handler current x"},
    {0x020AF22Eu, 0x7A28u, 0x89E8u, 4u, "tap handler current x (2)"},
    {0x020AF32Eu, 0x7A28u, 0x89E8u, 4u, "tap handler current x (3)"},
    {0x020AF3AAu, 0x79A9u, 0x8A29u, 8u, "move handler press x"},
    {0x020AF3AEu, 0x7A2Bu, 0x89EBu, 8u, "move handler current x"},
    {0x020AF3B8u, 0x7A28u, 0x89E8u, 8u, "move handler current x (2)"},

    // ── GROUP 16: the command cursor's OWN truncation ────────────────────
    //
    // The contextual command icon -- the harvest icon on a tree or crystal,
    // the move arrow, patrol, attack -- is spawned by func_020A8CCC, and the
    // point it is spawned at comes through func_020AA814 (and its twin
    // func_020AA874). Both mask the click to a byte before converting it to a
    // map tile:
    //
    //     020AA82E  0609  lsls r1,r1,#24
    //     020AA830  0612  lsls r2,r2,#24
    //     020AA832  0E09  lsrs r1,r1,#24     <- r1 = x & 0xFF
    //     020AA834  0E12  lsrs r2,r2,#24     <- r2 = y & 0xFF
    //
    // `lsl #24 / lsr #24` is exactly the `& 0xFF` docs/233 removed at
    // 020E4994/020E4998, and the fix is the same: shift by 16 and come back
    // ARITHMETICALLY, so a negative or past-256 coordinate survives instead of
    // wrapping modulo 256. These eight are the only sites of their kind on
    // this path -- the four the gesture recogniser uses are already above.
    {0x020AA82Eu, 0x0609u, 0x0409u, 16u, "cursor point x: lsl 24 -> 16"},
    {0x020AA830u, 0x0612u, 0x0412u, 16u, "cursor point y: lsl 24 -> 16"},
    {0x020AA832u, 0x0E09u, 0x1409u, 16u, "cursor point x: lsr 24 -> asr 16"},
    {0x020AA834u, 0x0E12u, 0x1412u, 16u, "cursor point y: lsr 24 -> asr 16"},
    {0x020AA88Eu, 0x0609u, 0x0409u, 16u, "cursor point x (2): lsl 24 -> 16"},
    {0x020AA890u, 0x0612u, 0x0412u, 16u, "cursor point y (2): lsl 24 -> 16"},
    {0x020AA892u, 0x0E09u, 0x1409u, 16u, "cursor point x (2): lsr -> asr"},
    {0x020AA894u, 0x0E12u, 0x1412u, 16u, "cursor point y (2): lsr -> asr"},

    // ── GROUP 32: the native-screen visibility CULL ──────────────────────
    //
    // This is why the cursors do not merely land in the wrong place out in the
    // band -- they DO NOT APPEAR AT ALL. func_020A8CCC tests the icon's screen
    // position against the native 256x192 screen and, if it is outside, jumps
    // to 020A8D24, which finishes the LERP and ends the icon:
    //
    //     020A8D0C  DB0A  blt 020A8D24    x < -24   -> abort
    //     020A8D18  DB04  blt 020A8D24    y < -16   -> abort
    //     020A8D1E  DC01  bgt 020A8D24    x > 256   -> abort    (0x1E + 0xE2)
    //     020A8D22  DD04  ble 020A8D2E    y <= 192  -> draw
    //
    // The bounds are built from 8-bit Thumb immediates (`subs r1,#0x34`,
    // `adds r1,#0xE2`, `cmp r5,#0xC0`), so they cannot simply be widened to a
    // band that changes with the window. The three aborts become NOPs and the
    // final conditional becomes unconditional, which removes the test rather
    // than re-tuning it.
    //
    // WHAT IS LOST BY REMOVING IT, stated because it is a real behaviour
    // change and not free: the abort arm was also how an icon ENDED when it
    // scrolled off the native screen. Without it an icon ends only when its
    // own LERP finishes (alpha 31 -> 0 over 800, set at 020A8DA8), which is
    // the normal path anyway. The icon is a 16x16 quad positioned from the
    // camera every frame, so one that leaves the visible surface is simply
    // drawn outside it.
    // ── GROUP 64: the THUMB half of the allocation widening ───────────────
    //
    // The same heap overwrite as the seven ARM sites in kTouchPatches: the port
    // writes a 16-bit press x at recogniser +0x10, which is past the end of a
    // 0x10-byte object. These four construct the same object with a THUMB
    // `movs r0,#0x10`. 0x020A87AA is absent because kTouchPatches already
    // covers it as part of a 32-bit write at 0x020A87A8.
    {0x020A8768u, 0x2010u, 0x2018u, 64u, "recogniser alloc (func_020A875C)"},
    {0x020A886Cu, 0x2010u, 0x2018u, 64u, "recogniser alloc (func_020A8860)"},
    {0x020DDA56u, 0x2010u, 0x2018u, 64u, "recogniser alloc (func_020DDA34)"},
    {0x020E34CCu, 0x2010u, 0x2018u, 64u, "recogniser alloc (func_020E34BC)"},

    // ── GROUP 128: THE VERTICAL AXIS ──────────────────────────────────────
    //
    // Units could not reach the top bounds of the map, and a click outside
    // the original aspect just moved to its edge. One defect, and this is it.
    //
    // The guest keeps the touch position in BYTES -- recogniser +7 press, +9
    // current. The port publishes 16-bit shadows for X only, so on a 348-tall
    // surface every row past 255 wrapped modulo 256: a click near the top of
    // the window arrived as a row near the top of the NATIVE panel, the
    // classifier queried empty ground there, found no entity, and the order
    // fell through to a plain move -- landing at the native edge.
    //
    // These eight are the readers that decide where an order goes and what is
    // under the cursor. Current Y moves to the +0x12 shadow, press Y to +0x14.
    // Both live in the space the allocation widening created; all fourteen
    // construction sites are 0x18 now, so the fields exist wherever the object
    // was made.
    //
    // THE PRESS/CURRENT SPLIT MATTERS. Serving both corners from the current
    // value makes a TAP correct (they are equal) and collapses a DRAG's
    // vertical extent -- which would break in-band drag-select while appearing
    // to fix the band. That is why press gets its own halfword rather than
    // sharing one, exactly as X does.
    {0x020AF32Cu, 0x7A69u, 0x8A69u, 128u, "tap: order Y from the +0x12 shadow"},
    {0x020AF3B6u, 0x7A69u, 0x8A69u, 128u, "drag: order Y from the shadow"},
    {0x020AF22Cu, 0x7A69u, 0x8A69u, 128u, "pen-up: order Y from the shadow"},
    {0x020AF29Eu, 0x7A6Au, 0x8A6Au, 128u, "classifier: query Y from the shadow"},
    {0x020AF31Au, 0x79EBu, 0x8AABu, 128u, "tap: rect press Y from +0x14"},
    {0x020AF3ACu, 0x79EAu, 0x8AAAu, 128u, "drag: rect press Y from +0x14"},
    {0x020AF222u, 0x79EAu, 0x8AAAu, 128u, "pen-up: rect press Y from +0x14"},
    {0x020AF1C6u, 0x79EBu, 0x8AABu, 128u, "camera-delta: press Y from +0x14"},

    // docs/348: a drag worked from anywhere, but what it selected did not
    // match the bounding box. The press
    // corner's Y was moved above; the CURRENT corner's Y was not. The
    // selection rectangle is built by FUN_020AA8E4 from a stack argument that
    // these load with `ldrb rX,[r5,#9]`, and FUN_020AA8E4 reads it back with
    // LDRSH (020AA8F8), so a 16-bit signed row survives the trip:
    //
    //   020AF1BC  FUN_020AF15C  the drag in progress -> the preview set
    //             at 0x0215800C (the units highlighted while dragging)
    //   020AF310  FUN_020AF23C  pen-up -> THE SELECTION, 0x02158024
    //
    // With the byte, a row outside 0..255 wrapped (true_y & 0xFF) -- and the
    // drag-crash clamp (docs/344) pins it to 0/191, so the in-drag preview and
    // the final selection could not even agree with each other. The other
    // four are the same field read for the remaining rectangles and points in
    // these handlers (their X halves are already moved), so no reader of the
    // current row is left on the byte except the box DRAWING (0203B8D4),
    // which the port replaces and whose byte must stay clamped.
    // Encodings verified against the ROM image; ldrh imm5 9 = offset 0x12.
    {0x020AF1BCu, 0x7A68u, 0x8A68u, 128u, "drag: selection rect current Y from +0x12"},
    {0x020AF310u, 0x7A68u, 0x8A68u, 128u, "pen-up: selection rect current Y from +0x12"},
    {0x020AF21Cu, 0x7A68u, 0x8A68u, 128u, "pen-up: rect current Y from +0x12"},
    {0x020AF3A6u, 0x7A68u, 0x8A68u, 128u, "move handler: rect current Y from +0x12"},
    {0x020AF1D8u, 0x7A69u, 0x8A69u, 128u, "drag: ctx+0x22 current Y from +0x12"},
    {0x020AF36Au, 0x7A6Au, 0x8A6Au, 128u, "move handler: point Y from +0x12"},

    {0x020A8D0Cu, 0xDB0Au, 0x46C0u, 32u, "cursor cull: x < -24 -> nop"},
    {0x020A8D18u, 0xDB04u, 0x46C0u, 32u, "cursor cull: y < -16 -> nop"},
    {0x020A8D1Eu, 0xDC01u, 0x46C0u, 32u, "cursor cull: x > 256 -> nop"},
    {0x020A8D22u, 0xDD04u, 0xE004u, 32u, "cursor cull: y <= 192 -> always"},
};

static void apply_order_fix(void) {
    // ON BY DEFAULT, with the whole set applied rather than a minimal subset.
    //
    // The bug exists BECAUSE the redirect was partial: docs/229-234 moved the
    // readers that mattered for selection onto the 16-bit shadows and left
    // thirteen others on the cartridge's byte fields, so selection worked out
    // in the band while a move order to the same place wrapped modulo 256. A
    // mixed state is the disease; completing the redirect is the cure, and
    // hunting the smallest subset that happens to work would preserve exactly
    // the inconsistency that caused this.
    //
    // Gated on the one adaptive-window toggle, like every other touch patch,
    // so "off" is still pure-vanilla rendering and behaviour. LB_ORDERFIX
    // overrides for A/B -- including LB_ORDERFIX=0 to turn it off in an
    // otherwise Enhanced run, which is how the before/after in docs/238 was
    // measured.
    static int groups = -1;
    if (groups < 0) {
        // 255 = all eight groups. 15 covered the four order-path groups; 16 is
        // the command cursor's own byte truncation, 32 its native-screen
        // visibility cull, 64 the THUMB half of the recogniser allocation
        // widening -- a memory-safety fix, not a feature -- and 128 THE WHOLE
        // VERTICAL AXIS, which had no coverage at all before.
        //
        // LB_ORDERFIX=127 disables only the vertical work, which is the A/B
        // that isolates it from everything else on this path.
        const char* v = std::getenv("LB_ORDERFIX");
        groups = v ? std::atoi(v) : 255;
    }
    if (!groups || !lb_setting_enhanced()) return;
    static int done = 0;
    static unsigned gen = 0;   // docs/396: re-armed by a game reset
    if (gen != g_boot_gen) { gen = g_boot_gen; done = 0; }
    if (done) return;

    // Every selected site must read its stock encoding before anything is
    // written, so a build that does not match is left alone entirely rather
    // than half-patched.
    int want = 0, ready = 0;
    for (const HalfPatch& p : kOrderFix) {
        if (!(p.group & groups)) continue;
        ++want;
        if (bus_read_u16(p.addr) == p.stock) ++ready;
    }
    if (want == 0 || ready != want) return;

    for (const HalfPatch& p : kOrderFix) {
        if (!(p.group & groups)) continue;
        bus_write_u16(p.addr, p.want);
        std::fprintf(stderr, "[lb_orderfix] %08X %s -> the 16-bit shadow\n",
                     p.addr, p.what);
    }
    done = 1;
    std::fprintf(stderr, "[lb_orderfix] groups=%d, %d site(s) moved off the "
                 "byte path\n", groups, want);
    std::fflush(stderr);
}

static void apply_hotwire_anything(void) {
    static int applied = 0;           // 0 = stock, 1 = patched
    static unsigned gen = 0;          // docs/396: a reset restores stock
    if (gen != g_boot_gen) { gen = g_boot_gen; applied = 0; }
    const int want = lb_setting_hotwire_anything();
    if (want == applied) return;

    // Only touch the site when every word reads exactly what this build should
    // have -- either the stock encoding or our own. Anything else is a
    // different build, and writing into it would be guesswork.
    const int n = (int)(sizeof kHotwireAnything / sizeof kHotwireAnything[0]);
    int ready = 0;
    for (const HotwirePatch& p : kHotwireAnything) {
        const uint32_t got = bus_read_u32(p.addr);
        if (got == p.stock || got == p.want) ++ready;
    }
    if (ready != n) return;           // not loaded yet, or not this build

    for (const HotwirePatch& p : kHotwireAnything)
        bus_write_u32(p.addr, want ? p.want : p.stock);
    applied = want;
    std::fprintf(stderr,
                 "[lb_hotwire] hotwire-anything %s: the farm allowance %s "
                 "refuse a stolen special (%08X)\n",
                 want ? "ON" : "off", want ? "can no longer" : "can again",
                 kHotwireAnything[0].addr);
    std::fflush(stderr);
}

static void apply_hotwire_specials_only(void) {
    static int applied = 0;           // 0 = stock, 1 = patched
    static unsigned gen = 0;          // docs/396: a reset restores stock
    if (gen != g_boot_gen) { gen = g_boot_gen; applied = 0; }
    // docs/392: RETIRED, kept only to put the word back. Its redirect also
    // refused category 5, and vanilla hotwires an enemy transport directly
    // (the tap's 0x3B tag is given to 5 and 6). cast_spell_override does the
    // job now, and lets 5 and 6 through.
    const int want = 0;
    if (want == applied) return;

    // Same discipline as apply_hotwire_anything: only touch the site when it
    // reads exactly what this build should have, so a different build is left
    // alone rather than corrupted.
    const int n = (int)(sizeof kHotwireSpecialsOnly /
                        sizeof kHotwireSpecialsOnly[0]);
    int ready = 0;
    for (const HotwirePatch& p : kHotwireSpecialsOnly) {
        const uint32_t got = bus_read_u32(p.addr);
        if (got == p.stock || got == p.want) ++ready;
    }
    if (ready != n) return;           // not loaded yet, or not this build

    for (const HotwirePatch& p : kHotwireSpecialsOnly)
        bus_write_u32(p.addr, want ? p.want : p.stock);
    applied = want;
    std::fprintf(stderr,
                 "[lb_hotwire] specials-only %s: a non-special %s be hotwired "
                 "(%08X %s)\n",
                 want ? "ON" : "off",
                 want ? "can no longer" : "can again",
                 kHotwireSpecialsOnly[0].addr, kHotwireSpecialsOnly[0].what);
    std::fflush(stderr);
}

// ── Self-test for the kill path (LB_KILLTEST=1) ───────────────────────────
//
// GOAL: `issue_waiting_to_die` is the newest and riskiest code in this file --
// it makes five chained guest calls and moves the guest stack pointer -- and
// its only consumer, hotwire-transport mode 1, needs a transport carrying
// occupants to be hotwired, which is hard to stage on demand. Testing the
// mechanism through that trigger would confound "the kill does not work" with
// "the scenario did not occur", which is precisely the mistake docs/238 records
// five times over.
//
// So exercise the machinery directly: once, a few seconds into a battle, pick a
// live unit and issue the command for it. If the entity leaves the pool, the
// path works and mode 1 rests on something proven. If the game survives but
// nothing dies, the command was built wrong. If the game destabilises, the
// guest-call sequence or the stack handling is wrong -- and better to learn
// that here than inside a hotwire.
// CONTEXT MATTERS, AND THE FIRST VERSION OF THIS TEST GOT IT WRONG.
//
// Run from `lb_patches_on_vblank` -- a HOST-side hook, outside any guest
// execution -- the very first call unwound:
//
//     [lb_call] UNWOUND at init (020EC5A8)
//
// That is not a fault in the kill path. `runtime_call_push_return` and
// `runtime_dispatch` expect to be running inside the guest's own call stack,
// which is why the occupant sweep's SetOwner call works: `set_owner_override`
// is itself a dispatched override, so it already has that context. A vblank
// hook does not.
//
// So the test must piggyback on a real guest call. `lb_kill_test_pending`
// arms it here, and `set_owner_override` -- which the game enters constantly --
// performs it. Reporting the first version's UNWOUND as "mode 1 is broken"
// would have been the same mistake docs/238 records five times: an instrument
// failure read as a fact about the game.
bool g_kill_test_pending = false;
static uint32_t g_kill_victim = 0u;
static uint32_t g_kill_before_cmds = 0u;
static int32_t  g_kill_before_pool = 0;

static void apply_kill_test(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_KILLTEST") ? 1 : 0;
    if (!on) return;
    static int done = 0;
    if (done) return;

    int cx = 0, cy = 0;
    if (!lb_field_camera(&cx, &cy)) return;      // not in a battle yet
    static int settle = 0;
    if (++settle < 600) return;                  // ten seconds of battle first

    const uint32_t pool = bus_read_u32(kEntityPoolPtr);
    if (!pool) return;
    const uint32_t array = bus_read_u32(pool + 8u);
    const int32_t count = (int32_t)bus_read_u32(pool + 0xCu);
    if (!array || count <= 0) return;

    // A unit, never a structure, and never OWNER 0's -- so the test cannot
    // knock out the player's own base or the builder every other probe steers
    // by, and cannot be mistaken for the harness breaking its own scenario.
    //
    // The first filter asked for category 2..6 and found nothing. That was not
    // the scan failing: the census showed 20 live of 20 slots with
    // unreadable=0, holding 12 of category 0, 4 builders and 4 structures. A
    // fresh Free Play battle simply has no melee/ranged/mounted units yet. So
    // category 0 is fair game -- there are twelve of them -- provided it
    // belongs to somebody else.
    uint32_t victim = 0u;
    for (int32_t i = 0; i < count && i < 4096; ++i) {
        const uint32_t e = bus_read_u32(array + (uint32_t)i * 4u);
        if (!e) continue;
        const uint8_t cat = entity_category(e);
        if (cat > 6u) continue;                       // structures
        if (bus_read_u8(e + 0xE4u) == 0u) continue;   // not the local player's
        victim = e;
        break;
    }
    if (!victim) {
        // SHOW THE WORKING. "No unit to test on" is a claim about the GAME
        // made by a scan whose own correctness is unproven -- exactly the
        // shape of null result docs/238 was burned by five times. Print the
        // category census so the next reader can tell "the battle has no such
        // unit" from "entity_category is returning nonsense here".
        unsigned census[24] = {0};
        unsigned bad = 0, live = 0;
        for (int32_t i = 0; i < count && i < 4096; ++i) {
            const uint32_t e = bus_read_u32(array + (uint32_t)i * 4u);
            if (!e) continue;
            ++live;
            const uint8_t c = entity_category(e);
            if (c < 24u) ++census[c]; else ++bad;
        }
        std::fprintf(stderr,
                     "[lb_killtest] no category 2..6 unit yet: %u live of %d "
                     "slots, unreadable=%u\n", live, (int)count, bad);
        for (unsigned c = 0; c < 24u; ++c)
            if (census[c])
                std::fprintf(stderr, "[lb_killtest]   category %2u: %u\n",
                             c, census[c]);
        std::fflush(stderr);
        // Not done: a unit may simply not exist yet. Retry in ten seconds.
        settle = 0;
        static int waits = 0;
        if (++waits >= 8) {
            std::fprintf(stderr, "[lb_killtest] giving up after 8 tries\n");
            done = 1;
        }
        return;
    }

    const uint32_t before = bus_read_u32(0x021585ACu);
    const int32_t pool_before = count;
    std::fprintf(stderr,
                 "[lb_killtest] issuing WaitingToDie for %08X (cat %u), "
                 "pool=%d commands=%u\n",
                 victim, entity_category(victim), (int)pool_before, before);
    std::fflush(stderr);

    // ARM IT; do not call from here. See the note above this function: a
    // vblank hook has no guest call stack, and the first call would unwind.
    g_kill_victim = victim;
    g_kill_before_cmds = before;
    g_kill_before_pool = pool_before;
    g_kill_test_pending = true;
    std::fprintf(stderr, "[lb_killtest] armed; waiting for a guest call "
                 "context (set_owner_override)\n");
    std::fflush(stderr);
    done = 1;
}

// Performed from inside set_owner_override, which IS a dispatched override and
// therefore already has the guest call stack this sequence needs.
void lb_kill_test_run_if_armed(void) {
    if (!g_kill_test_pending) return;
    const uint32_t victim = g_kill_victim;
    if (!victim) { g_kill_test_pending = false; return; }

    const bool ok = issue_waiting_to_die(victim);

    // UNWINDING IS NORMAL HERE, and the first version treated it as final.
    //
    // This runs inside the entity SPAWN path, which is where
    // set_owner_override is reached from, and a slice can end part-way through
    // five nested guest calls. The occupant sweep beside it already assumes
    // exactly that -- it bails and lets re-entry redo the work, "which is a
    // no-op for occupants already moved". So retry rather than declaring
    // failure on the first partial attempt; a real fault will still show as a
    // run of identical failures at the same step.
    if (!ok) {
        static int tries = 0;
        if (++tries < 12) {
            std::fprintf(stderr,
                         "[lb_killtest] attempt %d unwound; re-arming\n",
                         tries);
            std::fflush(stderr);
            return;                       // stay armed
        }
        std::fprintf(stderr, "[lb_killtest] gave up after %d attempts\n",
                     tries);
    }
    g_kill_test_pending = false;

    const uint32_t after = bus_read_u32(0x021585ACu);
    const uint32_t pool2 = bus_read_u32(kEntityPoolPtr);
    const int32_t pool_after =
        pool2 ? (int32_t)bus_read_u32(pool2 + 0xCu) : -1;
    std::fprintf(stderr,
                 "[lb_killtest] returned %s, commands %u -> %u (+%d), "
                 "pool %d -> %d\n",
                 ok ? "ok" : "UNWOUND", g_kill_before_cmds, after,
                 (int)(after - g_kill_before_cmds),
                 (int)g_kill_before_pool, (int)pool_after);
    std::fflush(stderr);
}

// ── Stop at the world's edge instead of shifting the band ──────────────────
//
// The requirement: rather than jitter/shake the view at the edge/corner of the
// game world, do what the vanilla game does and not attempt to pan beyond it;
// the camera could repeatedly bump against the black world edge.
//
// Both halves of that are one fault. band_compute() derives the band from how
// much map is left on each side --
//
//     before = g_cam_px_x - 4;  after = map_px - (g_cam_px_x + 256) + 4;
//
// -- so as the camera nears an edge the band moves EVERY FRAME, and the band is
// what the HUD anchor, the 3D frustum shift (docs/268) and the compositor are
// all measured against. A reference frame that slides once per frame is
// precisely what shakes. Clamping the CAMERA instead leaves the band at the
// centred split, constant, and there is nothing left to shake against.
//
// docs/112 licenses the write and sets the one rule: the camera pair is
// presentation, not simulation (docs/85 found no Sim->User calls), writing it
// takes and persists, but "a port doing this must not exceed the bounds the
// guest would have enforced". This only ever moves the camera INWARD of the
// guest's own clamp, which is strictly inside them.
//
// FIRST IN THE VBLANK HOOK, before lb_widemap_fill() reads the camera and
// before the band is latched. Clamping afterwards would leave the fill and the
// latch working from a camera the port then moved, which is the same
// disagreement in a new place.
//
// THE VIEW CAN BE WIDER THAN THE WORLD, and that is not a corner case here: a
// tested surface is 1706 px across a 1536 px map. There is then no camera
// position that keeps the view inside the world, so the map is centred and the
// camera stops moving on that axis -- which is what not panning beyond the
// world means when there is nothing to pan to.
constexpr uint32_t kCamToMapX = 48u;   // lb_field.cpp's kCameraToMapX
constexpr uint32_t kCamToMapY = 0u;

bool camera_clamp_on() {
    // OFF BY DEFAULT (docs/345); LB_CAMCLAMP=1 re-enables it.
    //
    // This post-correction rewrites the guest's camera from outside, every
    // frame it disagrees, and an external write loses the race: the game moves
    // its camera back within the frame. Measured at a 2749x1234 window,
    // panning down: with the game's clamp stock it acted on 2,761 of 9,340
    // frames (30%) -- the two authorities fighting -- which fits the observed
    // flicker of the original-aspect region while an input is held against
    // the world border. It is also redundant: the band slides the
    // overhang at an edge (band_split), so with the stock clamp the window
    // already ends exactly at the map's edge (measured: bottom at 1024 of a
    // 1024-px world) with nothing writing the camera at all.
    //
    // Its history, for whoever reconsiders it: it was turned back on when
    // apply_camera_bounds() had to be withdrawn for shrinking the world. That
    // is still withdrawn; neither is needed.
    static const bool on = [] {
        const char* v = std::getenv("LB_CAMCLAMP");
        return v && v[0] == '1';
    }();
    return on;
}

bool camera_bounds_on() {
    // DEFAULT OFF -- THIS SHRANK THE PLAYABLE WORLD. See the note above
    // apply_camera_bounds(). LB_CAMBOUND=1 re-enables it for investigation
    // only; it must not ship on.
    static const bool on = [] {
        const char* v = std::getenv("LB_CAMBOUND");
        return v && v[0] == '1';
    }();
    return on;
}

// WITHDRAWN -- THIS SHRANK THE PLAYABLE WORLD. Default OFF; do not ship it on.
//
// It trimmed the world size inward, despite how the level visually renders:
// the minimap revealed the broken width and height, and units could not be
// moved to certain places. A major regression.
//
// The mistake is precisely located. The bound rect at +0x150 /
// +0x158 is not the camera's private business -- it is THE WORLD EXTENT. The
// camera clamp reads it, and so do pathfinding, the minimap, and the AI. Moving
// those tiles in by 13 and 5 to stop the camera panning past the widened view
// did not constrain the camera; it made the world 26 tiles narrower and 10
// shorter for everything that asks. That is why units refused to walk to the
// outer columns, why a builder would not site a structure out there, why the
// minimap drew black margins, and why an AI could not leave its own base.
//
// docs/281 named this exact risk in its "NOT established" list and it was not
// checked before writing: "whether the other consumers of the bound rect ...".
// The lesson is the one docs/112 already states -- a port may move the camera,
// but it must not rewrite simulation state to do it.
//
// The correct shape is below in the note, and is NOT what this function does:
// override func_0203906C and apply the band to the POINT BEING CLAMPED, reading
// the bound rect without ever writing it.
//
// EXTEND THE GUEST'S OWN CLAMP OUTWARD, instead of correcting the camera after
// the guest has already set it. docs/281 has the derivation; the short form:
//
// func_0203906C is the game's camera clamp. It reads a MIN tile point from the
// camera object at +0x150 and a MAX tile point at +0x158 -- tile x at +4, tile
// y at +5, both bytes -- converts them with func_0201479C (x * 24, y * 16, into
// 20.12 fixed point), adjusts by one tile, subtracts the view size, and ends in
// four conditional stores: strhgt against the max, strhlt against the min.
//
//     min_x = min_tile_x * 24                 max_x = max_tile_x * 24 + 24 - 248
//     min_y = min_tile_y * 16                 max_y = max_tile_y * 16 + 16 - 192
//
// The port shows half_x extra columns on each side of the native view, so the
// camera must stop half_x sooner at each edge. Moving the bound TILES does that
// through the guest's own arithmetic -- no second writer of +0x5E, nothing to
// fight, and no code patch (which would drop the function to Tier 3).
//
// WHY THE ROUNDING GOES OUTWARD. The bound is tile-granular: 24 px across, 16
// down. Rounding the adjustment UP stops the camera up to one tile early, which
// shows a little less world; rounding down would show black past the world
// edge. The defect was bumping into black, so early is the right side to err
// on.
//
// WHY THERE IS A CENTRED CASE. The view can be wider than the world -- a
// 1706 px surface against a 1536 px map. Then no camera position keeps
// the view inside, and min would exceed max. The guest applies max first and
// min second, so min would win and the camera would pin to the left edge with
// black on the right. Collapsing both bounds onto one tile instead fixes the
// camera near the middle, which is what not panning beyond the world means
// when there is nothing to pan to.
unsigned g_cambound_frames = 0u;
unsigned g_cambound_writes = 0u;

void apply_camera_bounds() {
    if (!camera_bounds_on()) return;

    // lb_field_camera() runs the locate sweep; lb_field_camera_obj() does not.
    // Calling it first is what keeps this cheap outside a battle.
    int cam_x = 0, cam_y = 0;
    if (!lb_field_camera(&cam_x, &cam_y)) return;
    const uint32_t obj = lb_field_camera_obj();
    if (!obj) return;
    ++g_cambound_frames;

    // The denominator, which the per-write log cannot show: a run that writes
    // once and then holds prints nothing more, so without this there is no
    // evidence distinguishing "held for ten thousand frames" from "the camera
    // object went away". Once a minute at 60 fps.
    if (std::getenv("LB_CAMLOG") && (g_cambound_frames % 3600u) == 0u)
        std::fprintf(stderr,
                     "[lb_cambound] holding: %u writes in %u frames with a "
                     "camera\n", g_cambound_writes, g_cambound_frames);

    const int ow = (int)nds_gpu3d_output_width();
    const int oh = (int)nds_gpu3d_output_height();
    const int half_x = ow > 256 ? (ow - 256) / 2 : 0;
    const int half_y = oh > 192 ? (oh - 192) / 2 : 0;
    const int dx = (half_x + 23) / 24;
    const int dy = (half_y + 15) / 16;

    const uint32_t a_min_x = obj + 0x154u, a_min_y = obj + 0x155u;
    const uint32_t a_max_x = obj + 0x15Cu, a_max_y = obj + 0x15Du;

    const int cur_min_x = (int)bus_read_u8(a_min_x);
    const int cur_min_y = (int)bus_read_u8(a_min_y);
    const int cur_max_x = (int)bus_read_u8(a_max_x);
    const int cur_max_y = (int)bus_read_u8(a_max_y);

    // THE STOCK BOUNDS, and how they are kept stock across a rewrite.
    //
    // Re-deriving the adjustment from an already-adjusted value would ratchet
    // the bounds inward a tile per frame. So the stock four are remembered, and
    // are re-read only when what is in the object is NOT what this wrote --
    // which is exactly the case where the guest (a new level, a scripted bound
    // change) has put its own value there. When the guest keeps restoring
    // stock every frame this still cannot ratchet: each frame re-reads the
    // stock it just restored.
    static uint32_t seen_obj = 0u;
    static int org[4] = {0, 0, 0, 0};
    static int put[4] = {-1, -1, -1, -1};
    if (obj != seen_obj || cur_min_x != put[0] || cur_min_y != put[1] ||
        cur_max_x != put[2] || cur_max_y != put[3]) {
        seen_obj = obj;
        org[0] = cur_min_x; org[1] = cur_min_y;
        org[2] = cur_max_x; org[3] = cur_max_y;
    }

    auto axis = [](int lo, int hi, int d, int* out_lo, int* out_hi) {
        int want_lo = lo + d, want_hi = hi - d;
        if (want_lo > want_hi) {            // view wider than the world
            const int mid = (lo + hi) / 2;
            want_lo = want_hi = mid;
        }
        if (want_lo < 0) want_lo = 0;
        if (want_hi > 255) want_hi = 255;
        if (want_hi < want_lo) want_hi = want_lo;
        *out_lo = want_lo; *out_hi = want_hi;
    };
    int w_min_x = 0, w_max_x = 0, w_min_y = 0, w_max_y = 0;
    axis(org[0], org[2], dx, &w_min_x, &w_max_x);
    axis(org[1], org[3], dy, &w_min_y, &w_max_y);

    if (w_min_x == cur_min_x && w_min_y == cur_min_y &&
        w_max_x == cur_max_x && w_max_y == cur_max_y) {
        put[0] = w_min_x; put[1] = w_min_y;
        put[2] = w_max_x; put[3] = w_max_y;
        return;
    }
    bus_write_u8(a_min_x, (uint8_t)w_min_x);
    bus_write_u8(a_min_y, (uint8_t)w_min_y);
    bus_write_u8(a_max_x, (uint8_t)w_max_x);
    bus_write_u8(a_max_y, (uint8_t)w_max_y);
    put[0] = w_min_x; put[1] = w_min_y;
    put[2] = w_max_x; put[3] = w_max_y;
    ++g_cambound_writes;

    // WRITES PER FRAME IS THE WHOLE POINT, so the log has to be able to tell
    // one write from sixty. Printing on `n % 60 == 1` could not: a single line
    // meant anywhere between 1 and 60 writes, which is the difference between
    // "set the bounds once" and "fighting the guest sixty times". Every write
    // is printed for the first eight, then one in sixty.
    if (std::getenv("LB_CAMLOG")) {
        static unsigned n = 0;
        ++n;
        if (n <= 8u || (n % 60u) == 1u)
            std::fprintf(stderr,
                         "[lb_cambound] obj %08X  stock x %d..%d y %d..%d  "
                         "-> x %d..%d y %d..%d  d %d,%d  half %d,%d  "
                         "surface %dx%d  wrote %u/%u frames\n",
                         obj, org[0], org[2], org[1], org[3],
                         w_min_x, w_max_x, w_min_y, w_max_y, dx, dy,
                         half_x, half_y, ow, oh,
                         g_cambound_writes, g_cambound_frames);
    }
}

// HOW OFTEN THE CLAMP HAS TO ACT, which is the difference between "it holds the
// camera" and "it fights the guest for it every frame".
//
// Writing the camera at vblank corrects the RESULT; it does not stop the guest
// advancing the camera again during the next frame. If the guest keeps pushing
// against a pinned edge, the picture is drawn from whichever of the two won that
// frame and the view shakes by the guest's step -- which would trade bumping
// against the black world edge for something worse. fillcam alternating
// 640/648 while this pins 640 is what raised the question.
//
// acted/frames near 1.0 in steady play IS the fight. Near 0 after an initial
// correction is the clamp doing its job and nothing more.
unsigned g_camclamp_frames = 0u;
unsigned g_camclamp_acted = 0u;

void apply_camera_clamp() {
    if (!camera_clamp_on()) return;
    ++g_camclamp_frames;
    int map_w = 0, map_h = 0;
    if (!lb_widemap_map_px(&map_w, &map_h)) return;   // no level, nothing to do
    int cam_map_x = 0, cam_map_y = 0;
    if (!lb_field_camera(&cam_map_x, &cam_map_y)) return;

    // band_compute() works in the RAW value at +0x5E; lb_field_camera subtracts
    // 48 for entity-world alignment. Clamp in the raw space the band uses, then
    // hand lb_field_camera_write() the map-space value it expects.
    const int raw_x = cam_map_x + (int)kCamToMapX;
    const int raw_y = cam_map_y + (int)kCamToMapY;

    const int ow = (int)nds_gpu3d_output_width();
    const int oh = (int)nds_gpu3d_output_height();
    const int half_x = ow > 256 ? (ow - 256) / 2 : 0;
    const int half_y = oh > 192 ? (oh - 192) / 2 : 0;

    auto pick = [](int v, int lo, int hi, int centre_span) {
        if (lo > hi) return centre_span / 2;      // view wider than the world
        return v < lo ? lo : (v > hi ? hi : v);
    };
    const int want_x = pick(raw_x, half_x, map_w - 256 - half_x, map_w - 256);
    const int want_y = pick(raw_y, half_y, map_h - 192 - half_y, map_h - 192);

    if (want_x == raw_x && want_y == raw_y) return;
    ++g_camclamp_acted;
    lb_field_camera_write(want_x - (int)kCamToMapX, want_y - (int)kCamToMapY);

    if (std::getenv("LB_CAMLOG")) {
        static unsigned n = 0;
        if ((++n % 60u) == 1u)
            std::fprintf(stderr,
                         "[lb_camclamp] raw %d,%d -> %d,%d  half %d,%d  "
                         "map %dx%d surface %dx%d  acted %u/%u frames "
                         "(dx %d dy %d)\n",
                         raw_x, raw_y, want_x, want_y, half_x, half_y,
                         map_w, map_h, ow, oh,
                         g_camclamp_acted, g_camclamp_frames,
                         want_x - raw_x, want_y - raw_y);
    }
}

// ---- LB_LAGFIX: the two-frame lag between the ground and the geometry ------
//
// docs/335 measured it. The compositor draws the 2D live from VRAM and the 3D
// from a snapshot of the frame the renderer just finished, and the guest steps
// its camera on alternate frames only (30 Hz camera, 60 Hz compositor), so the
// geometry lands TWO compositor frames behind the ground:
//
//     frame   cam   dcam  mode  resid
//      1006   190    +4    -8     -4
//      1008   192    +2    -4     -2
//      1010   192    +0    -2     -2   <- camera stopped, geometry still moving
//
// mode(N) against dcam(N-2) matched on five of six pairs, negated. Frame 1010
// is the one that rules out noise: the input stopped and the output did not.
//
// THE CORRECTION is the camera's travel over those two frames, applied to the
// 3D layer when it is composited. Off by default and measured before it goes
// on: lb_jitter.py's `mode + d(cam)` must collapse to 0 on moving pairs and
// its RIGID figure must not fall below the 99.36% it reads now.
namespace {
int g_cam_hist[3] = {0, 0, 0};
unsigned g_cam_hist_n = 0u;

bool lagfix_on(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_LAGFIX") ? 1 : 0;
    return on != 0;
}
}  // namespace

// ---- LB_RESHIFT: re-present a stale 3D raster at the current camera -------
//
// THE MECHANISM, measured rather than assumed (docs/337). The guest builds its
// 3D display list at roughly 30 Hz: 42% of frames in which the camera is STILL
// submit no polygons at all, and 88% of frames in which it MOVES. On those
// frames the compositor re-presents the raster the renderer last produced,
// which is correct when nothing has moved and wrong the moment the camera has
// -- the ground is decoded live from VRAM every frame, so it scrolls while the
// sprites sit where they were when the raster was made.
//
// That is the same defect docs/335 measured as a two-frame lag, seen from the
// other side, and it explains why the lag is worst during a pan: a pan is
// exactly when the camera moves between one submission and the next.
//
// THE CORRECTION is the camera's travel since the raster was rendered -- not a
// fixed two frames. lb_gx_submitted() says whether THIS frame produced new
// geometry, so the camera is stamped when it did and the delta is measured
// from that stamp.
//
// WHY THIS CAN BE APPLIED WHERE THE PREVIOUS ATTEMPT COULD NOT. LB_LAGFIX slid
// the whole 3D row and was withdrawn because the in-field HUD is 3D geometry
// in the same raster and would have moved with it. lb_hud_shift_line() already
// relocates the HUD by RECT at composite time, so it is told the shift and
// compensates its source reads: the world moves, the HUD does not.
namespace {
int g_raster_cam_x = 0, g_raster_cam_y = 0;
bool g_raster_cam_ok = false;

bool reshift_on(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_RESHIFT") ? 1 : 0;
    return on != 0;
}
}  // namespace

void lb_reshift_sample(void) {
    if (!reshift_on()) return;
    int cx = 0, cy = 0;
    if (!lb_widemap_cam_px(&cx, &cy)) return;
    // A FRESH RASTER RESETS THE STAMP. Anything else leaves it, so the delta
    // keeps growing for as long as the guest declines to submit.
    if (lb_gx_submitted() > 0u) {
        g_raster_cam_x = cx;
        g_raster_cam_y = cy;
        g_raster_cam_ok = true;
    }
}

// ---- LB_LIVE3D: pair the 2D with the 3D from the SAME moment (docs/342) ----
//
// The widened composite runs at the 263->0 wrap (frontend.cpp runs the
// scheduler to the next frame boundary, then composites). By then, in order:
//
//     VBlank (192)   the guest's VBlank handler writes this frame's BG scroll
//                    (func_020396E0 -> func_0202D1B4), and the geometry list
//                    it submitted is latched;
//     VCount 215     that latched list is rasterised -- the LIVE raster;
//     263 -> 0       nds_gpu2d_finish_frame() flips g_front, making the
//                    snapshot of the frame that JUST ENDED the front buffer.
//
// The 2D is decoded from VRAM as it stands now, i.e. after the VBlank handler.
// The DS displays that 2D alongside the list latched at the same VBlank --
// the live raster. The snapshot is the raster that was on screen during the
// frame that just ended: one guest update older. Pairing it with the new 2D
// puts the ground one step (8 or 16 px on a pan) ahead of every sprite, which
// is the jitter docs/335-341 measured from four directions.
//
// The band (where the guest's 256 columns sit in the widened surface) is
// stamped per 3D frame too, so it switches with the raster; otherwise live
// sprites would be placed with a stale band and shear the other way.
//
// Default OFF until graded on recorded frames. Read once: this is called per
// composited scanline.
extern "C" int lb_live3d(void) {
    static const int on = [] {
        const char* v = std::getenv("LB_LIVE3D");
        return (v && v[0] == '1') ? 1 : 0;
    }();
    return on;
}

// ---- LB_DUMP3D: the 3D layer EXACTLY as composited (docs/342) ------------
//
// Every jitter gate so far read the POLYGON LIST, and a composite-time change
// never touches the polygon list -- so LB_RESHIFT was graded by a tool that
// could not see it, and LB_LIVE3D would be too. This records the 3D line the
// compositor actually used, per row, so a sprite's displacement can be read
// from PIXELS and compared with the ground's displacement in the same frames.
//
// STATIC, NOT A VECTOR. The composite rows may run on worker threads; a lazily
// resized buffer would race on the first frame. Sized for the largest surface
// the widener allows (gpu2d.cpp kMaxAdaptiveWidth x kMaxAdaptiveHeight).
namespace {
constexpr int kUsed3dMaxW = 1728;
constexpr int kUsed3dMaxH = 768;
uint32_t g_used3d[kUsed3dMaxW * kUsed3dMaxH];
uint32_t g_used3d_rgb[kUsed3dMaxW * kUsed3dMaxH];
int g_used3d_w = 0;
int g_used3d_h = 0;
}  // namespace

// docs/347 item 5: LB_OAMTRACE=1 prints engine-A sprites in the widened
// composite (see apply_hooks.py). Read once; this is called per sprite row.
extern "C" int lb_oam_trace_on(void) {
    static const int on = [] {
        const char* v = std::getenv("LB_OAMTRACE");
        return (v && v[0] == '1') ? 1 : 0;
    }();
    return on;
}

extern "C" int lb_live3d_probe(void) {
    static const int on = [] {
        const char* v = std::getenv("LB_LIVE3D_PROBE");
        return (v && v[0] == '1') ? 1 : 0;
    }();
    return on;
}

// WHEN, WITHIN THE FRAME, THE THREE THINGS THAT MUST AGREE ACTUALLY HAPPEN.
//
// The probe above refuted the stale-snapshot theory: the live raster equals
// the snapshot on 98.4% of frames, so the 3D shown IS the list latched at the
// latest VBlank -- and the sprites still trail the ground by one frame. The
// decomp (func_020396E0 at task priority 0x01, SwapBuffers at 0x25) says the
// cartridge keeps them in step on purpose. What is left is timing inside the
// frame: when the guest's SwapBuffers EXECUTES in the geometry engine (a flush
// executed after VBlank starts latches one VBlank late), when the latch
// happens, and when the guest writes BG1HOFS. Scanline-stamped, per event.
uint32_t nds_io_debug_read(int cpu, uint32_t addr, uint32_t width);
extern "C" void lb_evt(int kind) {
    if (!lb_live3d_probe()) return;
    static const char* const kName[] = {"swap-exec", "latch", "bg1hofs"};
    if (kind < 0 || kind > 2) return;
    std::fprintf(stderr, "[lb_evt] frame %u vcount %u %s\n",
                 lb_dump_frame_get(),
                 (unsigned)nds_io_debug_read(9, 0x04000006u, 16),
                 kName[kind]);
}

extern "C" int lb_dump3d_on(void) {
    static const int on = [] {
        const char* v = std::getenv("LB_DUMP3D");
        return (v && v[0] == '1') ? 1 : 0;
    }();
    return on;
}

extern "C" void lb_used3d_store(int y, const uint32_t* line, int w, int h) {
    if (!line || w <= 0 || h <= 0 || w > kUsed3dMaxW || h > kUsed3dMaxH ||
        y < 0 || y >= h)
        return;
    g_used3d_w = w;   // every worker writes the same two values
    g_used3d_h = h;
    std::memcpy(g_used3d + (size_t)y * (size_t)w, line,
                (size_t)w * sizeof(uint32_t));
}

// For the PPM writer, which takes 0x00RRGGBB. The 3D line is melonDS's
// r | g<<8 | b<<16 at 6 bits a channel with a 5-bit alpha at bit 24 (the
// compositor tests `(px >> 24) & 0x1F`). Alpha 0 is written as pure magenta,
// a colour this game's 3D never produces at 6-bit precision, so the mask
// survives a format with no alpha channel.
extern "C" const uint32_t* lb_used3d_rgb(int* w, int* h) {
    const int fw = g_used3d_w, fh = g_used3d_h;
    if (w) *w = fw;
    if (h) *h = fh;
    if (fw <= 0 || fh <= 0) return nullptr;
    const size_t n = (size_t)fw * (size_t)fh;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = g_used3d[i];
        if (((p >> 24) & 0x1Fu) == 0u) {
            g_used3d_rgb[i] = 0x00FF00FFu;
            continue;
        }
        const uint32_t r = (p & 0x3Fu) << 2;
        const uint32_t g = ((p >> 8) & 0x3Fu) << 2;
        const uint32_t b = ((p >> 16) & 0x3Fu) << 2;
        g_used3d_rgb[i] = (r << 16) | (g << 8) | b;
    }
    return g_used3d_rgb;
}

// ---- LB_PAIR: show each 3D frame with ITS OWN scroll commit (docs/343) -----
//
// THE CARTRIDGE'S DESIGN, from the decompilation. One game update is one
// task list. Task 0x01
// (func_020396E0) writes the BG scroll and DMAs the tilemaps using the camera
// committed in the PREVIOUS run; later tasks commit the new camera
// (0203942C), build the 3D list from it (02039148) and swap (02024D08). The
// scroll commit waits one run ON PURPOSE, so it pairs with the previous run's
// swap.
//
// WHAT GOES WRONG. The swap latches at the next VBlank; the matching scroll
// commit arrives with the next run's task 0x01. When the game's 20 ms pacing
// delays that next run by a VBlank, the new 3D list is on screen for one frame
// with the OLD ground under it -- measured in pixels (docs/342): sprites replay
// the ground's motion one frame late, 16 px on half the active frames of a pan.
//
// THE RULE. After a latch, keep presenting the raster (and band) that was
// paired with the current scroll commit, until the scroll commit that goes
// with the new list arrives. Three guards make it safe:
//
//   * ONE FRAME, NEVER MORE: only the composite immediately after a latch can
//     hold. The measured mismatch lasts exactly one frame; a hold can never
//     freeze the 3D.
//   * FIELD ONLY: only while the scroll callback is live (a BG1HOFS write in
//     the last 3 composites), so menus and scenes without it never hold.
//   * SAME SURFACE: a held raster from a different window size is discarded.
//
// Engine A's widened composite only. Vanilla keeps the ROM's exact pairing.
namespace {
unsigned g_pair_latch_seq = 0;      // bumped at every geometry latch
unsigned g_pair_seen_latch = 0;     // latch_seq as of the last composite
bool g_pair_bg_since_latch = true;  // a scroll commit has followed the latch
bool g_pair_bg_since_composite = false;
unsigned g_pair_composites_since_bg = 1000;
uint32_t g_pair_held[kUsed3dMaxW * kUsed3dMaxH];
int g_pair_w = 0, g_pair_h = 0;
int g_pair_band = 0, g_pair_band_y = 0;
bool g_pair_have = false;           // g_pair_held holds a complete frame
bool g_pair_storing = false;        // this composite is filling g_pair_held
unsigned g_pair_n_hold = 0, g_pair_n_live = 0;
}  // namespace

extern "C" int lb_pair_on(void) {
    static const int on = [] {
        const char* v = std::getenv("LB_PAIR");
        return (v && v[0] == '1') ? 1 : 0;
    }();
    return on;
}

// Emulation thread, at the VBlank latch (lb_band_promote).
extern "C" void lb_pair_on_latch(void) {
    ++g_pair_latch_seq;
    g_pair_bg_since_latch = false;
}

// Emulation thread, at every engine-A BG1HOFS write (func_020396E0's commit).
extern "C" void lb_pair_on_bg(void) {
    g_pair_bg_since_latch = true;
    g_pair_bg_since_composite = true;
}

// Once per widened engine-A composite, before any band or row is chosen.
extern "C" int lb_pair_decide(int w, int h) {
    if (!lb_pair_on()) return 0;
    // The previous composite finished filling the buffer.
    if (g_pair_storing) {
        g_pair_have = true;
        g_pair_storing = false;
    }
    if (g_pair_bg_since_composite) g_pair_composites_since_bg = 0;
    else if (g_pair_composites_since_bg < 1000u) ++g_pair_composites_since_bg;
    g_pair_bg_since_composite = false;
    const bool new_latch = g_pair_latch_seq != g_pair_seen_latch;
    g_pair_seen_latch = g_pair_latch_seq;
    const bool hold = new_latch && !g_pair_bg_since_latch &&
                      g_pair_composites_since_bg <= 3u && g_pair_have &&
                      g_pair_w == w && g_pair_h == h;
    if (hold) {
        ++g_pair_n_hold;
    } else {
        ++g_pair_n_live;
        g_pair_storing = true;
    }
    if (lb_live3d_probe())
        std::fprintf(stderr, "[lb_pair] frame %u %s latch_new %d bg_after %d "
                     "since_bg %u  (hold %u / live %u)\n",
                     lb_dump_frame_get(), hold ? "HOLD" : "live",
                     new_latch ? 1 : 0, g_pair_bg_since_latch ? 1 : 0,
                     g_pair_composites_since_bg, g_pair_n_hold, g_pair_n_live);
    return hold ? 1 : 0;
}

extern "C" void lb_pair_store(int y, const uint32_t* line, int w, int h,
                              int band, int band_y) {
    if (!lb_pair_on() || !g_pair_storing || !line || w <= 0 || h <= 0 ||
        w > kUsed3dMaxW || h > kUsed3dMaxH || y < 0 || y >= h)
        return;
    if (g_pair_w != w || g_pair_h != h) g_pair_have = false;
    g_pair_w = w;   // every worker writes the same values
    g_pair_h = h;
    g_pair_band = band;
    g_pair_band_y = band_y;
    std::memcpy(g_pair_held + (size_t)y * (size_t)w, line,
                (size_t)w * sizeof(uint32_t));
}

extern "C" const uint32_t* lb_pair_row(int y, int w) {
    return g_pair_held + (size_t)y * (size_t)w;
}
extern "C" int lb_pair_band(void) { return g_pair_band; }
extern "C" int lb_pair_band_y(void) { return g_pair_band_y; }

// HOW BIG A CORRECTION IS BELIEVABLE, measured rather than assumed.
//
// The first version of this clamped to +/-24 px, and a driven pan showed the
// clamp SATURATED on 882 of the 923 frames where it fired -- 95.6%. That is
// not a correction, it is a constant. The trace says why:
//
//     frame 332  cam 343,144  campick 0  subm 0  rsdx 24
//     frame 333  cam 343,144  campick 1  subm 0  rsdx 24
//     frame 334  cam 359,144  campick 0  subm 0  rsdx 24
//
// the camera moves +16 px per frame and the guest declines to resubmit for
// three frames together, so the raster is genuinely 32-48 px behind. Clamping
// that to 24 truncates a real quantity on almost every frame it applies to,
// and a systematically truncated correction is its own source of error.
//
// 64 px is four frames of travel at the +16 px/frame this pan produces. Beyond
// that a bad camera reading is likelier than real travel, so the correction is
// ABANDONED rather than clamped: returning 0 reproduces the uncorrected
// picture, which is a known quantity, where a 64 px shift of a raster that did
// not move that far is a visible tear.
extern "C" int lb_reshift_dx(void) {
    if (!reshift_on() || !g_raster_cam_ok) return 0;
    int cx = 0, cy = 0;
    // Returns 0 when g_cam_px_valid is false, and THAT is the frame the
    // polylist header prints as `cam 0,0`. Distrusting it here is what keeps a
    // failed reading from being mistaken for a 1252 px pan.
    if (!lb_widemap_cam_px(&cx, &cy)) return 0;
    const int d = cx - g_raster_cam_x;
    if (d > 64 || d < -64) return 0;
    return d;
}

// THE SHIFT ITSELF, with the edges handled instead of wrapped.
//
// The previous form of this was `line3d_raw + lb_rs` at the call site, which
// is wrong at both ends of the row: the last `dx` pixels of a positive shift
// read from the START OF THE NEXT ROW, and a negative shift on row 0 reads
// before the buffer. That is the "clamping that breaks things" this port is
// supposed to be getting rid of, not adding.
//
// Out-of-range samples are written as 0, which has alpha 0 in the compositor's
// ARGB (it tests `(px >> 24) & 0x1F`), so the uncovered edge shows the 2D
// ground rather than a smear of the wrong row. An edge strip of at most 64 px
// loses its sprites for a frame; it does not tear.
extern "C" const uint32_t* lb_reshift_line(const uint32_t* line, int width,
                                           int dx) {
    if (!line || dx == 0 || width <= 0) return line;
    // Per thread: the compositor calls this from every row worker at once
    // (lb_parallel_rows), and one shared buffer would hand each worker the
    // row another is writing. Only reachable with LB_RESHIFT set.
    thread_local std::vector<uint32_t> scratch;
    if (static_cast<int>(scratch.size()) < width) scratch.resize(width);
    for (int x = 0; x < width; ++x) {
        const int s = x + dx;
        scratch[x] = (s >= 0 && s < width) ? line[s] : 0u;
    }
    return scratch.data();
}

// Pushed once per frame from the vblank hook, which is the same cadence the
// compositor runs at.
void lb_lag_sample_camera(void) {
    if (!lagfix_on()) return;
    int cx = 0, cy = 0;
    if (!lb_field_camera(&cx, &cy)) return;
    g_cam_hist[2] = g_cam_hist[1];
    g_cam_hist[1] = g_cam_hist[0];
    g_cam_hist[0] = cx;
    if (g_cam_hist_n < 3u) ++g_cam_hist_n;
}

// How far to slide the 3D row when sampling it. A camera that moved +N puts
// the world -N on screen, and the geometry is showing where the world was two
// frames ago, so the sample index moves by the camera's travel since then.
//
// CLAMPED HARD. nds_gpu3d_line returns a row of a framebuffer whose stride is
// the render width; sampling past the end reads into the next row, and past
// the last row reads outside the allocation. The measured lag is 2-8 px on a
// normal pan, so 32 is far above anything real and still far inside a row.
extern "C" int lb_lag_dx(void) {
    if (!lagfix_on() || g_cam_hist_n < 3u) return 0;
    int d = g_cam_hist[0] - g_cam_hist[2];
    if (d > 32) d = 32;
    if (d < -32) d = -32;
    return d;
}

// ---- multiplayer: what the cartridge's own network layer is doing ----------
//
// The master-server client (lb_net.cpp) must
// know when the GAME is hosting and when its Join screen is open, and what the
// game calls this player, without any of it being guessed from the screen.
//
// THE STATE. The cartridge's network task, FUN_020F4B00, keeps its state in the
// word at 0x0215B7A0 + 0x1C, and the only writer is FUN_020F57C4 (`ldr r2,
// =0x0215B7A0; ... str r0,[r2,#0x1c]`, a 32-bit store). Who sets each value:
//
//   10  FUN_020F5FC0, the setup screen's Continue in multiplayer (FUN_020C7484)
//    2  FUN_020F4B00 case 10, once the parent is up: the host lobby, open
//    3  FUN_020F6138, from User::HostLobby (FUN_020BA5BC): the host's Start
//    4  FUN_020F4B00 case 3;  11  FUN_020F5168, the match begins (host side)
//    9  FUN_020F60A4, from User::JoinWifiGame::virtual_12: the Join screen
//    6  FUN_020F4B00 case 9, the scan running: the Join Game list is live
//    7  FUN_020F6114, a listed game tapped (FUN_020BBC8C);  5  FUN_020F4B00
//       case 7, joined;  12  FUN_020F5168, the match begins (client side)
//    8  FUN_020F60D4, leaving; then 0 (idle) or -1
//
// The cartridge's own "am I the host" test, FUN_020F6654, is exactly states
// 2, 3, 4 and 11, which is the cross-check on the host half of that table.
//
// THE NAME. FUN_0204F718 loads a profile out of the save and reads its name, 11
// bytes, into 0x02154E2D (FUN_0204B830(0x02154E2D, 0xB)); FUN_0204FE4C sets it
// from the name-entry keyboard, whole characters into a 10-byte budget. The
// Multiplayer menu (FUN_020BF970) and the network reset (FUN_020F6520) both
// pass FUN_0204F674() -- which returns 0x02154E2D -- to FUN_020F5EC8, which
// copies it to 0x0215B7D4 and 0x0215B7E4, and the host's own lobby record is
// built from the latter (FUN_020F5FC0). So this is the name the other players
// see in the game's lobby, which is why the master server should list it.
//
// Read-only, both of them: nothing here writes guest memory.
static const uint32_t kNetState = 0x0215B7BCu;
static const uint32_t kProfileName = 0x02154E2Du;

static void report_net_state(void) {
    char name[12];
    for (uint32_t i = 0; i < 11u; ++i)
        name[i] = (char)bus_read_u8(kProfileName + i);
    name[11] = 0;
    lb_net_guest_update((int)bus_read_u32(kNetState), name);
}

// ── docs/396: THE PLAYER NAME ───────────────────────────────────────────────
//
// The name the other players see is not the profile's but its two 11-byte
// copies, 0x0215B7D4 and 0x0215B7E4: FUN_020F5EC8 copies the profile name
// into both when the Multiplayer menu opens and on a network reset, and the
// host's lobby record is built from the second (FUN_020F5FC0). So a typed
// multiplayer name (the setting with "Use the save's profile name" off) is
// written into both, every VBlank they differ, which is after any copy the
// game makes. Nothing in the simulation reads them, so this cannot desync a
// match.
//
// A RENAME is different: the profile's own name, 0x02154E2D, which
// SaveGame (FUN_0204FA88) writes into the save -- its literal pool names
// that address, 11 bytes -- so the game keeps it the next time it saves. Done
// once, on request, never continuously: a continuous write would rename every
// profile the player loads.
extern "C" int lb_setting_mp_name(char* out, int cap);   /* lb_overlay.cpp */
static const uint32_t kLobbyName[2] = {0x0215B7D4u, 0x0215B7E4u};
static char g_rename[11];
static bool g_rename_pending = false;

// The game's name-entry keyboard spends a 10-byte budget on whole
// characters; plain printable ASCII is what this accepts.
static void clean_name(const char* in, char out[11]) {
    std::memset(out, 0, 11);
    int n = 0;
    for (int i = 0; in && in[i] && n < 10; ++i) {
        const unsigned char c = (unsigned char)in[i];
        if (c >= 0x20 && c < 0x7F) out[n++] = (char)c;
    }
}

extern "C" void lb_request_profile_rename(const char* name) {
    clean_name(name, g_rename);
    g_rename_pending = g_rename[0] != 0;
}

static void apply_player_name(void) {
    if (g_rename_pending) {
        g_rename_pending = false;
        for (uint32_t i = 0; i < 11u; ++i)
            bus_write_u8(kProfileName + i, (uint8_t)g_rename[i]);
        g_profile_save_pending = true;   // docs/397: saved by the game, next menu
        std::fprintf(stderr, "[lb_name] profile renamed to \"%s\"; saving it at "
                             "the next menu screen\n", g_rename);
    }
    char want[32], name[11];
    if (lb_setting_mp_name(want, (int)sizeof want) <= 0) return;
    clean_name(want, name);
    if (!name[0]) return;
    for (uint32_t c = 0; c < 2u; ++c) {
        bool same = true;
        for (uint32_t i = 0; i < 11u; ++i)
            if (bus_read_u8(kLobbyName[c] + i) != (uint8_t)name[i]) { same = false; break; }
        if (same) continue;
        for (uint32_t i = 0; i < 11u; ++i)
            bus_write_u8(kLobbyName[c] + i, (uint8_t)name[i]);
        static unsigned logged = 0;
        if (++logged <= 4u)
            std::fprintf(stderr, "[lb_name] multiplayer name -> \"%s\" (copy %u)\n",
                         name, c);
    }
}

// ── docs/396: A GAME RESET ─────────────────────────────────────────────────
//
// The menu's Reset (and Ctrl+R) runs the runner's own power-on boot() at the
// frontend's between-frames safe point, so the cartridge starts over from its
// title screen in the same window. Everything the port wrote into guest
// memory is gone with it -- boot() reloads the ARM9 image -- so this puts the
// port's own once-only state back where a fresh start has it: the one-shot
// patch sites (through g_boot_gen), the blueprint search the balance patch
// and the builders hang off, the per-level tables, and the recording, which
// starts a new file because its frame count starts again. The gameplay
// overrides live in the dispatch table, not in guest memory, and stay.
extern "C" void lb_balance_on_reset(void);   /* lb_balance.cpp */
extern "C" void lb_demo_on_reset(void);      /* lb_demo.cpp */
extern "C" void lb_port_on_guest_reset(void) {
    ++g_boot_gen;
    g_blueprint_base = 0u;
    g_bp_settled = false;
    g_builders_patched = false;
    lb_balance_on_reset();
    g_detail_done = false;
    g_leader_done = false;
    g_plant_done = false;
    g_fp_applied = 0;
    g_fp_applied_y = 0;
    g_rename_pending = false;
    g_profile_save_pending = false;
    g_save_call.active = false;
    g_kc.active = false;
    lb_demo_on_reset();
    std::fprintf(stderr, "[lb_reset] the game was reset to its title screen "
                         "(generation %u)\n", g_boot_gen);
    std::fflush(stderr);
}

extern "C" void lb_patches_on_vblank(void) {
    // Before anything reads the camera. See the note above.
    //
    // Bounds first: it widens the guest's own clamp, so the guest holds the
    // camera itself for the rest of the frame. apply_camera_clamp() is the
    // superseded post-correction and is off unless LB_CAMCLAMP=1.
    apply_camera_bounds();
    apply_camera_clamp();
    // The A/B trace hash (docs/333). Off unless LB_RAMSUM is set, and here
    // rather than in the compositor because it must read main RAM with the
    // guest between frames, on the emulation thread.
    lb_shadow_on_vblank();
    lb_lag_sample_camera();   /* docs/335 */
    lb_reshift_sample();      /* docs/337 */
    // NOT THE PER-CALL GRADE. verify_registry_find() was called from here and
    // every single probe reported "[lb_call] UNWOUND at verify-registry". This
    // hook runs from io.cpp's VCOUNT==192 branch -- the SCHEDULER, not a
    // dispatched guest slice -- so runtime_dispatch has no slice to return
    // into and unwinds immediately. It is called from the override instead,
    // which is guest context, the way issue_waiting_to_die already does it.
    entity_log();
    effect_log();
    apply_touch_extension();
    // The "Hotwire anything (cheat)" combo entry, which had an
    // accessor and no caller until the vanilla rule above was located.
    apply_hotwire_anything();
    apply_hotwire_specials_only();
    // Move the order path off the cartridge's byte touch
    // fields. Gated on LB_ORDERFIX; a no-op unless it is set. docs/238.
    apply_order_fix();
    // Self-test for the newly implemented kill path; a no-op unless
    // LB_KILLTEST is set. docs/239 flags it as compiled-but-never-run.
    apply_kill_test();
    apply_insn_hook_policy();
    // Fill the tilemap columns the guest leaves empty, so the widened field
    // shows terrain rather than black. Off unless LB_WIDEMAP is set.
    lb_widemap_fill();
    // IMMEDIATELY AFTER THE FILL, which is what reads the camera and the map
    // extent this depends on, and BEFORE the guest submits any geometry for
    // the coming frame. One value, fixed for the frame; see the note on
    // lb_band_latch in lb_widemap.cpp for why the moment matters.
    //
    // apply_touch_extension() above therefore maps a click through LAST
    // frame's band. A frame of lag on the mouse is not observable, and giving
    // touch the fresher value would cost the 2D and 3D layers their agreement
    // about the same frame, which is.
    lb_band_latch((int)nds_gpu3d_output_width(),
                  (int)nds_gpu3d_output_height());
    // Make the Music mode actually choose the music.
    // A no-op in Vanilla, which is the default and the "off" position.
    lb_music_tick();
    lb_teams_portrait_tick();   // docs/390: the Custom/Random portrait records
    opponent_open_portrait_tick();   // docs/393: and the opening choice's
    mod_names_tick();           // docs/391: the mod maps' names
    // docs/387: the level palette's colours (a no-op at Per-Mod/Vanilla).
    lb_palette_tick();
    // The demo clock. One tick per vblank, so a replay lands each input
    // on the frame the player put it on however fast the host runs.
    lb_demo_apply_env();
    lb_demo_tick();
    // docs/349 LB_SURFLOG=1: the demo frame on which the surface size
    // changes. apply_view_width/height, apply_cull_width and
    // apply_camera_view_size below write GUEST memory from this size every
    // vblank, so if the host settles the size on a different frame from one
    // run to the next, the game's view, cull rect and camera clamp differ
    // from that frame on -- a replay diverges with nothing wrong in the
    // emulator. The same replay gave two different instruction streams under
    // light and heavy machine load; this says whether this is why.
    {
        static const bool lb_surflog = std::getenv("LB_SURFLOG") != nullptr;
        static unsigned lb_sw = 0, lb_sh = 0;
        const unsigned w = nds_gpu3d_output_width();
        const unsigned h = nds_gpu3d_output_height();
        if (lb_surflog && (w != lb_sw || h != lb_sh)) {
            std::fprintf(stderr, "[lb_surf] demo frame %u surface %ux%u "
                         "(was %ux%u)\n", lb_demo_frame(), w, h, lb_sw, lb_sh);
            lb_sw = w;
            lb_sh = h;
        }
    }
    apply_cheats();
    apply_unlocks();
    // docs/356: cosmetic and this machine's only. Reads nothing while off.
    apply_leader_name();
    apply_plantation_name();   // docs/392
    // The game's network state and profile name for the
    // master-server client. Before the blueprint check below, which returns
    // early in every menu -- and the menus are where multiplayer happens.
    report_net_state();
    apply_player_name();       // docs/396
    apply_view_width();
    apply_view_height();
    apply_cull_width();
    // Effects and projectiles are tested against the DS screen by the
    // cartridge itself, outside the gather rect (docs/350).
    apply_fx_cull();
    // And so is every unit, building, spell and collectible sound.
    // After lb_band_latch, whose split it reads.
    apply_snd_cull();
    // The minimap's camera box, drawn by the cartridge for the view the
    // window shows (docs/370).
    apply_minimap_box();
    // A sprite's depth follows its feet across the vertical bands (docs/377).
    apply_depth_unwrap();
    apply_cage_fix();   // docs/385
    // The cartridge's pass cadence: steady 30 Hz by default, a playing demo's
    // own when one is replaying (docs/351). After lb_demo_tick, so a demo that
    // just started or just switched pacing is already known.
    apply_cadence();
    // The camera clamp's own view size. Every path that moves the camera
    // settles at whatever bound this computes, because SetCameraPos only sets
    // a dirty byte and the per-frame update re-clamps through func_0203906C.
    apply_camera_view_size();
    // And where the camera stops, so the picture moves the moment it does
    // (docs/373).
    apply_camera_edge();
    dump_3d_layer_if_asked();
    dump_polygon_list_if_asked();
    field_dump_if_asked();

    // Caps first, and unconditionally: they are independent of the blueprint
    // block, live in ARM9 static data that is resident from boot, and must
    // still be applied (or restored) when same-builders is off.
    apply_caps();

    // Locate the blueprint block FIRST, before any per-feature decision.
    //
    // This used to sit behind the same-builders check, which meant the block
    // was only ever searched for when that one toggle was on -- and the balance
    // patcher, which needs exactly the same block, could never find it. A
    // shared prerequisite does not belong inside one feature's branch.
    bool discovered = false;
    if (g_blueprint_base == 0u) {
        g_blueprint_base = find_blueprint_block();
        if (g_blueprint_base == 0u) return;   // not loaded yet; try next frame
        std::fprintf(stderr,
                     "[lb_patch] blueprints found at 0x%08X (Imperial builder "
                     "fingerprint matched)\n", g_blueprint_base);
        g_bp_settled = false;
        g_bp_hash = blueprint_span_hash(g_blueprint_base);
        g_bp_steady = 0;
        g_bp_frames = 0u;
        return;
    } else if (!blueprint_block_valid(g_blueprint_base) && !g_builders_patched &&
               !lb_balance_is_applied()) {
        // A balance patch legitimately changes the Imperial builder's stats, so
        // the fingerprint stops matching once one is applied. Re-finding then
        // would search forever and never validate.
        g_blueprint_base = 0u;
        return;
    }
    if (!g_bp_settled) {
        // docs/386: wait for the loader to finish the block (see
        // blueprint_span_hash), then keep every byte either edit below may
        // touch, as loaded, before either touches one.
        ++g_bp_frames;
        if (g_bp_frames % 20u != 0u) return;
        const uint32_t h = blueprint_span_hash(g_blueprint_base);
        if (h != g_bp_hash) {
            g_bp_hash = h;
            g_bp_steady = 0;
            return;
        }
        if (++g_bp_steady < 3) return;
        g_bp_settled = true;
        std::fprintf(stderr, "[lb_patch] blueprints settled %u frames after "
                     "they were found\n", g_bp_frames);
        lb_balance_snapshot(g_blueprint_base);
        snapshot_same_builders();
        discovered = true;
    }

    // What the settings want. LB_BALANCE=<slot> applies a balance patch
    // without the UI, so it can be verified headlessly (a test aid, like
    // LB_AUTOPLAY).
    int slot = lb_setting_balance_patch();
    if (const char* spec = std::getenv("LB_BALANCE")) slot = std::atoi(spec);
    int builders = lb_setting_same_builders();   // docs/396: 0, or the source
    if (builders < 0 || builders > 6) builders = 0;
    if (slot == lb_balance_applied_slot() && builders == g_builders_patched)
        return;

    // docs/386: A CHANGE TAKES EFFECT AT ONCE outside a battle and a lobby
    // (lb_blueprint_edit_blocked); inside one it waits, and the panel says
    // why. The first application, at the blueprints' discovery, is not gated:
    // it is what always happened at the load.
    if (!discovered) {
        static int said_slot = -1, said_builders = -1;
        if (const char* why = lb_blueprint_edit_blocked()) {
            if (said_slot != slot || said_builders != builders) {
                said_slot = slot;
                said_builders = builders;
                std::fprintf(stderr, "[lb_balance] the change waits: %s\n", why);
            }
            return;
        }
        said_slot = said_builders = -1;
    }

    // Undo in the reverse order of application, then apply in order. The
    // pack goes BEFORE same-builders: the setting wants all builders
    // equalised to the Imperial one, and with a pack that has to be the
    // PATCHED Imperial; applying the pack second would overwrite the
    // equalisation.
    if (g_builders_patched) {
        revert_same_builders();
        g_builders_patched = 0;
        if (!builders)
            std::fprintf(stderr, "[lb_patch] same-builders off: the builders' "
                                 "loaded stats are back\n");
    }
    if (lb_balance_applied_slot() != slot &&
        !lb_balance_revert(g_blueprint_base)) {
        std::fprintf(stderr, "[lb_balance] %s cannot be taken out (no loaded "
                     "copy of this block); restart to change it\n",
                     lb_balance_name(lb_balance_applied_slot()));
        return;
    }
    if (slot > 0 && lb_balance_applied_slot() != slot) {
        char err[128] = "";
        if (!lb_balance_apply(slot, g_blueprint_base, err, (int)sizeof(err)))
            std::fprintf(stderr, "[lb_balance] apply failed: %s\n", err);
    }
    if (builders) {
        static const char* const kNames[6] = {"King", "Wizard", "Pirate",
                                              "Imperial", "Earth", "Alien"};
        apply_same_builders(builders);
        g_builders_patched = builders;
        std::fprintf(stderr,
                     "[lb_patch] same-builders applied: %u stat fields copied "
                     "from the %s builder to the 5 others\n",
                     (unsigned)(sizeof(kStatFields) / sizeof(kStatFields[0])),
                     kNames[builders - 1]);
    }
}


// Upstream calls this before registering the title banks, so anything
// registered here is found first. Gated on the exact ROM hash: these addresses
// are meaningless for any other cartridge, and silently patching the wrong game
// is worse than not patching at all.
extern "C" void lsw2_register_dispatch_overrides(const char* rom_sha1) {
    static const char kEuropeSha1[] =
        "da60469bd143eb8f47ecf0d7951181b4783c91b4";
    if (!rom_sha1) return;
    for (int i = 0; kEuropeSha1[i] || rom_sha1[i]; ++i)
        if (rom_sha1[i] != kEuropeSha1[i]) return;

    // THE GRADING ENTRIES ARE NOT REGISTERED IN A NORMAL RUN, by construction
    // rather than by being cheap when idle.
    //
    // kRegistryFind and kDivMod are not patches -- they exist to compare the
    // decompilation against the cartridge (docs/333) -- and kDivMod in
    // particular sits on the signed divide, which every `/` and `%` in the
    // game calls. Leaving it registered would put a host dispatch and a
    // table lookup on one of the hottest paths there is, in the build that
    // ships, to serve a counter nobody is reading. This project has
    // already shipped one regression of exactly that shape.
    //
    // Filtering at registration keeps the source table sorted -- which the
    // binary search requires -- while the ARM9 never sees the entries.
    static NdsDispatchEntry filtered[kOverrideCount];
    unsigned n = 0u;
    const bool grading = lb_shadow_mode() != kLbShadowGuest;
    for (unsigned i = 0; i < kOverrideCount; ++i) {
        const uint32_t a = kOverrideTable[i].addr;
        if (!grading && (a == kRegistryFind || a == kDivMod ||
                         a == kSplitArray || a == kListFind ||
                         a == kU16IsSet || a == kFAdd || a == kFMul ||
                         a == kFSub)) continue;
        filtered[n++] = kOverrideTable[i];
    }
    if (grading)
        std::fprintf(stderr, "[lb_shadow] %u override entries registered, "
                             "grading entries INCLUDED\n", n);
    if (n > 0u)
        nds_register_dispatch(NDS_ARM9, filtered, n, 0xFFFF0000u);
    std::fprintf(stderr,
                 "[lb_patch] %u gameplay override(s) installed ahead of the "
                 "title banks\n",
                 kOverrideCount);
}
